#include "update.h"
#include "boot.h"
#include "heap.h"
#include "install.h"
#include "settings.h"
#include "task.h"
#include "luna_build_version.h"

static bool running;
static uint32_t owner;
static unsigned progress;
static char update_message[192], detail[192];
static uint8_t manifest[UPDATE_MANIFEST_BYTES + 1];
static uint8_t *packed[UPDATE_RECORDS], *backup[UPDATE_RECORDS];
static bool touched[UPDATE_RECORDS], included[UPDATE_RECORDS], changed[UPDATE_RECORDS];
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool update_busy(void) { return running; }
uint32_t update_owner(void) { return owner; }
unsigned update_progress(void) { return progress; }
const char *update_status(void) { return update_message; }
const char *update_detail(void) { return detail; }
const char *update_revision(void) { return LUNA_BUILD_FULL_COMMIT; }
static void state(const char *en, const char *pt, unsigned percent) {
    strcpy(update_message, system_text(en, pt));
    progress = percent;
    gui_invalidate_app(APP_SETTINGS);
}
static void cleanup(void) {
    for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
        if (packed[i]) heap_release(packed[i]);
        if (backup[i]) heap_release(backup[i]);
        packed[i] = backup[i] = 0; touched[i] = false;
    }
}
static const char *find(const char *s, const char *key) {
    unsigned n = strlen(key);
    for (; *s; s++) if (!strncmp(s, key, n)) return s + n;
    return 0;
}
static bool head_commit(const char *json, char *commit) {
    const char *p = find(json, "\"sha\"");
    if (!p) return false;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    if (*p++ != '"') return false;
    for (unsigned i = 0; i < 40; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return false;
        commit[i] = p[i];
    }
    commit[40] = 0;
    return p[40] == '"';
}
static bool compatible_disk(unsigned disk, uint8_t *mbr) {
    const DiskInfo *info = disk_info(disk);
    if (!info || !info->present || !info->supported || info->optical || disk == 4 ||
        info->sectors < LUNA_IMAGE_BYTES / 512 || !disk_read_on(disk, 0, mbr)) return false;
    uint32_t id = word(mbr + 440);
    return id && id != 0x4c554e41 && mbr[510] == 0x55 && mbr[511] == 0xaa &&
        mbr[450] == 0xda && word(mbr + 454) == 8192 && word(mbr + 458) == LUNA_ESP_SECTOR - 8192 &&
        mbr[466] == 0xef && word(mbr + 470) == LUNA_ESP_SECTOR && word(mbr + 474) == LUNA_ESP_BYTES / 512;
}
static bool decode(unsigned i, uint8_t *raw) {
    const uint8_t *record = manifest + 64 + i * 40;
    unsigned n = word(record + 4);
    if (n) {
        if (update_inflate((char *)raw, UPDATE_CHUNK_BYTES, (const char *)packed[i], n) != UPDATE_CHUNK_BYTES) return false;
    } else memset(raw, 0, UPDATE_CHUNK_BYTES);
    return update_digest_valid(raw, UPDATE_CHUNK_BYTES, record + 8);
}
static bool transfer(unsigned disk, unsigned sector, uint8_t *data, bool write) {
    /* ATA PIO and the native storage services both accept these bounded batches. */
    for (unsigned offset = 0; offset < UPDATE_CHUNK_BYTES / 512; offset += 64) {
        bool ok = write ? disk_write_blocks(disk, sector + offset, 64, data + offset * 512) :
                          disk_read_blocks(disk, sector + offset, 64, data + offset * 512);
        if (!ok) return false;
    }
    return true;
}
static bool write_verify(unsigned disk, unsigned i, uint8_t *raw, uint8_t *verify) {
    unsigned sector = word(manifest + 64 + i * 40);
    return transfer(disk, sector, raw, true) && disk_flush(disk) &&
        transfer(disk, sector, verify, false) && !memcmp(raw, verify, UPDATE_CHUNK_BYTES);
}

/* Bundle format:
 * Full:  9,984-byte validated LUNAUPD1 manifest + compressed blocks in index order.
 * Delta: "LUNADLT2" + 40-byte exact source commit + 32-byte include bitmap,
 *        then the same target manifest and only included compressed blocks.
 * Unincluded blocks are verified against the installed disk before any writes.
 * There are no temporary files in the 4 MiB live LunaFS slots. */
typedef struct {
    const char *expected;
    uint8_t *scratch;
    uint8_t prelude[UPDATE_DELTA_HEADER_BYTES];
    unsigned prefix_used, prefix_length, manifest_used, record, received;
    bool delta, manifest_ok, malformed;
    bool selected[UPDATE_RECORDS];
    uint64_t downloaded, total, rate_bytes;
    uint32_t drawn_at, rate_at, rate_kib;
} UpdateStream;

static bool package_error(UpdateStream *p, const char *message) {
    p->malformed = true;
    detail[0] = 0;
    append(detail, message, sizeof(detail));
    return false;
}
static void update_show_download(UpdateStream *p) {
    if (!p->manifest_ok || !p->total) return;
    uint32_t now = frame_clock;
    uint32_t elapsed = now - p->rate_at;
    if (elapsed >= 1000u) {
        p->rate_kib = (unsigned)((p->downloaded - p->rate_bytes) * 1000u / elapsed / 1024u);
        p->rate_bytes = p->downloaded;
        p->rate_at = now;
    }
    if (p->drawn_at && now - p->drawn_at < 250u && p->downloaded < p->total) return;
    p->drawn_at = now ? now : 1u;
    progress = 1u + (unsigned)(p->downloaded * 49u / p->total);
    detail[0] = 0;
    char b[24];
    append(detail, "Download: ", sizeof(detail));
    number(b, (unsigned)(p->downloaded / 1024u)); append(detail, b, sizeof(detail));
    append(detail, " / ", sizeof(detail));
    number(b, (unsigned)((p->total + 1023u) / 1024u)); append(detail, b, sizeof(detail));
    append(detail, " KiB  ", sizeof(detail));
    number(b, p->rate_kib); append(detail, b, sizeof(detail));
    append(detail, " KiB/s", sizeof(detail));
    gui_invalidate_app(APP_SETTINGS);
}
static bool update_skip_empty(UpdateStream *p) {
    while (p->record < UPDATE_RECORDS) {
        unsigned i = p->record;
        if (!p->selected[i]) {
            p->record++;
            continue;
        }
        if (word(manifest + 64 + i * 40 + 4)) break;
        if (!decode(i, p->scratch))
            return package_error(p, "Invalid SHA-256 for empty update block.");
        p->record++;
    }
    return true;
}
static bool update_receive(void *context, const void *input, size_t length) {
    UpdateStream *p = context;
    const uint8_t *data = input;
    while (length) {
        if (!p->prefix_length || p->prefix_used < p->prefix_length) {
            unsigned required = p->prefix_length ? p->prefix_length : 8u;
            unsigned take = required - p->prefix_used;
            if (take > length) take = (unsigned)length;
            memcpy(p->prelude + p->prefix_used, data, take);
            p->prefix_used += take;
            p->downloaded += take;
            data += take; length -= take;
            if (p->prefix_used == 8u && !p->prefix_length) {
                p->delta = !memcmp(p->prelude, UPDATE_DELTA_MAGIC, 8u);
                if (!p->delta && memcmp(p->prelude, "LUNAUPD1", 8u))
                    return package_error(p, "Unrecognized LunaOS package header.");
                p->prefix_length = p->delta ? UPDATE_DELTA_HEADER_BYTES : 8u;
                if (!p->delta) {
                    memcpy(manifest, p->prelude, 8u);
                    p->manifest_used = 8u;
                }
            }
            continue;
        }
        if (p->manifest_used < UPDATE_MANIFEST_BYTES) {
            unsigned take = UPDATE_MANIFEST_BYTES - p->manifest_used;
            if (take > length) take = (unsigned)length;
            memcpy(manifest + p->manifest_used, data, take);
            p->manifest_used += take;
            p->downloaded += take;
            data += take; length -= take;
            if (p->manifest_used != UPDATE_MANIFEST_BYTES) continue;
            if (!update_manifest_valid(manifest, UPDATE_MANIFEST_BYTES, p->expected))
                return package_error(p, "Manifest identity/layout validation failed.");
            if (p->delta && (LUNA_BUILD_DIRTY ||
                             memcmp(p->prelude + 8, LUNA_BUILD_FULL_COMMIT, 40) ||
                             p->prelude[79] != 0))
                return package_error(p, "Differential base revision is incompatible.");
            uint64_t payload = 0;
            for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
                p->selected[i] = !p->delta ||
                    !!(p->prelude[48 + i / 8] & (1u << (i % 8)));
                if (p->selected[i])
                    payload += word(manifest + 64 + i * 40 + 4);
            }
            if (!p->selected[0]) return package_error(p, "Differential MBR block missing.");
            p->total = (p->delta ? UPDATE_DELTA_HEADER_BYTES : 0u) +
                       UPDATE_MANIFEST_BYTES + payload;
            p->manifest_ok = true;
            p->rate_at = frame_clock;
            p->rate_bytes = p->downloaded;
            if (!update_skip_empty(p)) return false;
            update_show_download(p);
            continue;
        }
        if (p->record >= UPDATE_RECORDS)
            return package_error(p, "Unexpected trailing bytes in update package.");
        unsigned i = p->record;
        unsigned size = word(manifest + 64 + i * 40 + 4);
        if (!size || !p->selected[i]) return package_error(p, "Invalid chunk stream state.");
        if (!packed[i]) {
            packed[i] = heap_allocate_owned(size + 1u, 1);
            if (!packed[i]) return package_error(p, "Insufficient RAM for compressed chunk.");
        }
        unsigned take = size - p->received;
        if (take > length) take = (unsigned)length;
        memcpy(packed[i] + p->received, data, take);
        p->received += take;
        p->downloaded += take;
        data += take; length -= take;
        if (p->received == size) {
            if (!decode(i, p->scratch))
                return package_error(p, "Downloaded chunk failed SHA-256 validation.");
            p->received = 0;
            p->record++;
            if (!update_skip_empty(p)) return false;
        }
        update_show_download(p);
    }
    return true;
}
static bool download_package(const char *url, const char *target, bool expect_delta,
                             uint8_t *scratch) {
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        cleanup();
        memset(manifest, 0, sizeof(manifest));
        UpdateStream p = {.expected = target, .scratch = scratch};
        uint64_t body_bytes = 0;
        detail[0] = 0;
        state("Downloading and verifying the update...",
              "Baixando e verificando a atualização...", 1);
        bool ok = net_https_stream(url, update_receive, &p, &body_bytes,
                                   detail, sizeof(detail));
        if (ok && p.manifest_ok && p.delta == expect_delta &&
            p.record == UPDATE_RECORDS && p.received == 0 &&
            body_bytes == p.total && p.downloaded == p.total) {
            memcpy(included, p.selected, sizeof(included));
            progress = 50;
            serial_write("LunaUpdate v2: bundle downloaded, all included SHA-256 checks passed.\\n");
            return true;
        }
        if (p.malformed) return false;  /* Never retry invalid authenticated content. */
        if (ok) {
            detail[0] = 0;
            append(detail, "Incomplete or wrong update package.", sizeof(detail));
        }
        if (strstr(detail, "HTTP status 404.")) return false;
        if (attempt < 2) task_sleep(250u * (attempt + 1u));
    }
    return false;
}
static void update_show_blocks(const char *prefix, unsigned done_blocks, unsigned total_blocks) {
    detail[0] = 0;
    append(detail, prefix, sizeof(detail));
    char b[24];
    number(b, done_blocks); append(detail, b, sizeof(detail));
    append(detail, " / ", sizeof(detail));
    number(b, total_blocks); append(detail, b, sizeof(detail));
    append(detail, " blocks", sizeof(detail));
}

void update_run(void) {
    if (running) return;
    running = true;
    owner = task_current()->pid;
    detail[0] = 0; progress = 0;
    serial_write("LunaUpdate v2: checking latest main revision.\\n");
    uint8_t mbr[512], *raw = 0, *verify = 0;
    char commit[41], url[256], base[192];
    unsigned disk = disk_boot_index();
    if (!net_ready) {
        state("Connect to the internet to check for updates.",
              "Conecte-se à internet para verificar atualizações.", 0);
        goto done;
    }
    if (setup_active || !compatible_disk(disk, mbr)) {
        state("Install LunaOS on a supported disk before updating.",
              "Instale o LunaOS em um disco compatível antes de atualizar.", 0);
        goto done;
    }
    state("Checking the official repository...", "Consultando o repositório oficial...", 0);
    char response[4096];
    int n = net_https("https://api.github.com/repos/" UPDATE_REPOSITORY "/git/ref/heads/main",
                      response, sizeof(response), detail, sizeof(detail));
    if (n < 0 || !head_commit(response, commit)) {
        state("Unable to check for updates. Try again.",
              "Não foi possível verificar atualizações. Tente novamente.", 0);
        goto done;
    }
    if (!LUNA_BUILD_DIRTY && !strcmp(commit, LUNA_BUILD_FULL_COMMIT)) {
        state("LunaOS is up to date.", "O LunaOS está atualizado.", 100);
        goto done;
    }
    if (memory_mb < 256) {
        state("Automatic updates require at least 256 MiB of RAM.",
              "Atualizações automáticas exigem pelo menos 256 MiB de RAM.", 0);
        goto done;
    }
    raw = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
    verify = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
    if (!raw || !verify) goto memory_error;
    strcpy(base, "https://github.com/" UPDATE_REPOSITORY "/releases/download/update-");
    append(base, commit, sizeof(base)); append(base, "/", sizeof(base));
    bool downloaded = false, got_delta = false;
    if (!LUNA_BUILD_DIRTY) {
        strcpy(url, base);
        append(url, "update-from-", sizeof(url));
        append(url, LUNA_BUILD_FULL_COMMIT, sizeof(url));
        append(url, ".pack", sizeof(url));
        serial_write("LunaUpdate v2: checking an exact-base differential bundle.\\n");
        downloaded = download_package(url, commit, true, raw);
        got_delta = downloaded;
        /* Only absent differentials may fall back automatically; do not hide
         * TLS, integrity, or transport errors by switching packages. */
        if (!downloaded && !strstr(detail, "HTTP status 404.")) {
            state("Differential download failed. The system was not changed.",
                  "Falha no download diferencial. O sistema não foi alterado.", 0);
            goto done;
        }
    }
full_download:
    if (!downloaded) {
        strcpy(url, base); append(url, "update.pack", sizeof(url));
        serial_write("LunaUpdate v2: downloading single full update bundle.\\n");
        if (!download_package(url, commit, false, raw)) {
            state("Update download failed. The installed system was not changed.",
                  "Falha no download. O sistema instalado não foi alterado.", 0);
            goto done;
        }
        got_delta = false;
    }
    /* Disk identities must survive updates. An incomplete delta is rejected
     * before saving, backing up or writing any OS sector. */
    unsigned to_write = 0;
    bool wrong_base = false;
    memset(changed, 0, sizeof(changed));
    for (unsigned i = 0; i < UPDATE_RECORDS; ++i) {
        unsigned sector = word(manifest + 64 + i * 40);
        if (!included[i]) {
            if (!transfer(disk, sector, raw, false)) {
                state("Disk read failed. Installation cancelled.",
                      "Falha na leitura do disco. Instalação cancelada.", 0);
                goto done;
            }
            if (!update_digest_valid(raw, UPDATE_CHUNK_BYTES, manifest + 64 + i * 40 + 8)) {
                wrong_base = true;
                break;
            }
            continue;
        }
        if (!decode(i, raw)) {
            state("Update integrity check failed. Installation cancelled.",
                  "Falha na integridade da atualização. Instalação cancelada.", 0);
            goto done;
        }
        if (!i) memcpy(raw + 432, mbr + 432, 78);
        if (!transfer(disk, sector, verify, false)) {
            state("Disk read failed. Installation cancelled.",
                  "Falha na leitura do disco. Instalação cancelada.", 0);
            goto done;
        }
        if (memcmp(raw, verify, UPDATE_CHUNK_BYTES)) {
            changed[i] = true;
            to_write++;
        } else if (packed[i]) {
            heap_release(packed[i]);
            packed[i] = 0;
        }
    }
    if (wrong_base && got_delta) {
        serial_write("LunaUpdate v2: differential baseline mismatch, selecting full bundle.\\n");
        downloaded = false;
        cleanup();
        goto full_download;
    }
    if (wrong_base) {
        state("Local disk differs from the signed update baseline.",
              "O disco local difere da base de atualização verificada.", 0);
        goto done;
    }
    if (!to_write) {
        state("LunaOS is up to date.", "O LunaOS está atualizado.", 100);
        goto done;
    }
    if (!settings_write() || !fs_save()) {
        state("Unable to save user data. Installation cancelled.",
              "Não foi possível salvar os dados. Instalação cancelada.", 0);
        goto done;
    }
    serial_write("LunaUpdate v2: backing up changed sectors in RAM.\\n");
    unsigned prepared = 0;
    for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
        if (!changed[i]) continue;
        state("Preparing recovery data...", "Preparando dados de recuperação...",
              50 + prepared * 20 / to_write);
        update_show_blocks("Backup: ", prepared, to_write);
        backup[i] = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
        if (!backup[i]) goto memory_error;
        if (!transfer(disk, word(manifest + 64 + i * 40), backup[i], false)) {
            state("Disk read failed. Installation cancelled.",
                  "Falha na leitura do disco. Instalação cancelada.", 0);
            goto done;
        }
        prepared++;
        task_sleep(1);
    }
    /* Write at most eight 256 KiB blocks, flush once, read back each block,
     * compare exact bytes, and commit the boot loader's first block last. */
    unsigned walked = 0, written = 0;
    while (walked < UPDATE_RECORDS) {
        unsigned batch[8], count = 0;
        while (walked < UPDATE_RECORDS && count < 8) {
            unsigned i = walked + 1 == UPDATE_RECORDS ? 0 : walked + 1;
            walked++;
            if (!changed[i]) continue;
            if (!decode(i, raw)) goto rollback;
            if (!i) memcpy(raw + 432, mbr + 432, 78);
            touched[i] = true;
            if (!transfer(disk, word(manifest + 64 + i * 40), raw, true)) goto rollback;
            batch[count++] = i;
        }
        if (!count) break;
        if (!disk_flush(disk)) goto rollback;
        for (unsigned slot = 0; slot < count; slot++) {
            unsigned i = batch[slot];
            if (!decode(i, raw)) goto rollback;
            if (!i) memcpy(raw + 432, mbr + 432, 78);
            if (!transfer(disk, word(manifest + 64 + i * 40), verify, false) ||
                memcmp(raw, verify, UPDATE_CHUNK_BYTES)) goto rollback;
        }
        written += count;
        state("Installing update. Keep the device powered on.",
              "Instalando atualização. Mantenha o dispositivo ligado.",
              70 + written * 29 / to_write);
        update_show_blocks("Installed and verified: ", written, to_write);
        task_sleep(1);
    }
    serial_write("LunaUpdate v2: verified installation complete. Rebooting.\\n");
    cleanup();
    state("Update installed. Restarting...", "Atualização instalada. Reiniciando...", 100);
    task_sleep(2000);
    reboot();
    goto done;
rollback: {
        serial_write("LunaUpdate v2: install error, rolling back changed sectors.\\n");
        state("Installation failed. Restoring the previous system...",
              "Instalação falhou. Restaurando o sistema anterior...", 0);
        bool restored = true;
        for (unsigned step = 0; step < UPDATE_RECORDS; step++) {
            unsigned i = step + 1 == UPDATE_RECORDS ? 0 : step + 1;
            if (touched[i] && !write_verify(disk, i, backup[i], verify)) restored = false;
            task_sleep(1);
        }
        state(restored ? "Update failed. Previous system restored; no restart." :
                         "Disk recovery failed. Use LunaOS installation media.",
              restored ? "Atualização falhou. Sistema anterior restaurado; sem reinício." :
                         "Recuperação do disco falhou. Use a mídia de instalação.", 0);
        goto done;
    }
memory_error:
    serial_write("LunaUpdate v2: memory allocation failure.\\n");
    state("Not enough free memory. Installation cancelled.",
          "Memória livre insuficiente. Instalação cancelada.", 0);
done:
    cleanup();
    if (raw) heap_release(raw);
    if (verify) heap_release(verify);
    running = false; owner = 0;
    gui_invalidate_app(APP_SETTINGS);
}
