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
static bool touched[UPDATE_RECORDS];
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
void update_run(void) {
    if (running) return;
    running = true; owner = task_current()->pid; detail[0] = 0; progress = 0;
    uint8_t mbr[512], *raw = 0, *verify = 0;
    char commit[41], url[256], base[192];
    unsigned disk = disk_boot_index();
    if (!net_ready) {
        state("Connect to the internet to check for updates.", "Conecte-se à internet para verificar atualizações.", 0); goto done;
    }
    if (setup_active || !compatible_disk(disk, mbr)) {
        state("Install LunaOS on a supported disk before updating.", "Instale o LunaOS em um disco compatível antes de atualizar.", 0); goto done;
    }
    state("Checking the official repository...", "Consultando o repositório oficial...", 0);
    char response[4096];
    int n = net_https("https://api.github.com/repos/" UPDATE_REPOSITORY "/git/ref/heads/main",
                      response, sizeof(response), detail, sizeof(detail));
    if (n < 0 || !head_commit(response, commit)) {
        state("Unable to check for updates. Try again.", "Não foi possível verificar atualizações. Tente novamente.", 0); goto done;
    }
    if (!LUNA_BUILD_DIRTY && !strcmp(commit, LUNA_BUILD_FULL_COMMIT)) {
        state("LunaOS is up to date.", "O LunaOS está atualizado.", 100); goto done;
    }
    if (memory_mb < 256) {
        state("Automatic updates require at least 256 MiB of RAM.", "Atualizações automáticas exigem pelo menos 256 MiB de RAM.", 0); goto done;
    }
    strcpy(base, "https://github.com/" UPDATE_REPOSITORY "/releases/download/update-");
    append(base, commit, sizeof(base)); append(base, "/", sizeof(base));
    strcpy(url, base); append(url, "update.manifest", sizeof(url));
    n = net_https(url, (char *)manifest, sizeof(manifest), detail, sizeof(detail));
    if (n < 0) {
        state("Update release unavailable. The build may still be running.", "Release indisponível. A compilação pode estar em andamento.", 0); goto done;
    }
    if (!update_manifest_valid(manifest, n, commit)) {
        state("Incompatible or invalid update package.", "Pacote de atualização inválido ou incompatível.", 0); goto done;
    }
    raw = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
    verify = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
    if (!raw || !verify) goto memory_error;
    for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
        state("Downloading and verifying the update...", "Baixando e verificando a atualização...", 1 + i * 49 / UPDATE_RECORDS);
        unsigned bytes = word(manifest + 64 + i * 40 + 4);
        if (bytes) {
            packed[i] = heap_allocate_owned(bytes + 1, 1);
            if (!packed[i]) goto memory_error;
            char index[] = "chunk-000.z";
            index[6] = '0' + i / 100; index[7] = '0' + i / 10 % 10; index[8] = '0' + i % 10;
            strcpy(url, base); append(url, index, sizeof(url));
            n = net_https(url, (char *)packed[i], bytes + 1, detail, sizeof(detail));
            if (n < 0 || (unsigned)n != bytes) {
                state("Download failed. The installed system was not changed.", "Download falhou. O sistema instalado não foi alterado.", 0); goto done;
            }
        }
        if (!decode(i, raw)) {
            state("Update integrity check failed. Installation cancelled.", "Falha na integridade da atualização. Instalação cancelada.", 0); goto done;
        }
        if (i == 0 && (raw[510] != 0x55 || raw[511] != 0xaa)) {
            state("Invalid boot image. Installation cancelled.", "Imagem de boot inválida. Instalação cancelada.", 0); goto done;
        }
        task_sleep(1);
    }
    if (!settings_write() || !fs_save()) {
        state("Unable to save user data. Installation cancelled.", "Não foi possível salvar os dados. Instalação cancelada.", 0); goto done;
    }
    for (unsigned i = 0; i < UPDATE_RECORDS; i++) {
        state("Preparing recovery data...", "Preparando dados de recuperação...", 50 + i * 20 / UPDATE_RECORDS);
        backup[i] = heap_allocate_owned(UPDATE_CHUNK_BYTES, 1);
        if (!backup[i]) goto memory_error;
        if (!transfer(disk, word(manifest + 64 + i * 40), backup[i], false)) {
            state("Disk read failed. Installation cancelled.", "Falha na leitura do disco. Instalação cancelada.", 0); goto done;
        }
        task_sleep(1);
    }
    /* Commit the MBR/first-stage loader last. Never change disk identity or partitions. */
    for (unsigned step = 0; step < UPDATE_RECORDS; step++) {
        unsigned i = step + 1 == UPDATE_RECORDS ? 0 : step + 1;
        state("Installing update. Keep the device powered on.", "Instalando atualização. Mantenha o dispositivo ligado.", 70 + step * 29 / UPDATE_RECORDS);
        if (!decode(i, raw)) goto rollback;
        if (!i) memcpy(raw + 432, mbr + 432, 78);
        touched[i] = true;
        if (!write_verify(disk, i, raw, verify)) goto rollback;
        task_sleep(1);
    }
    cleanup();
    state("Update installed. Restarting...", "Atualização instalada. Reiniciando...", 100);
    task_sleep(2000);
    reboot();
    goto done;
rollback: {
        state("Installation failed. Restoring the previous system...", "Instalação falhou. Restaurando o sistema anterior...", 0);
        bool restored = true;
        for (unsigned step = 0; step < UPDATE_RECORDS; step++) {
            unsigned i = step + 1 == UPDATE_RECORDS ? 0 : step + 1;
            if (touched[i] && !write_verify(disk, i, backup[i], verify)) restored = false;
            task_sleep(1);
        }
        state(restored ? "Update failed. Previous system restored; no restart." : "Disk recovery failed. Use LunaOS installation media.",
              restored ? "Atualização falhou. Sistema anterior restaurado; sem reinício." : "Recuperação do disco falhou. Use a mídia de instalação.", 0);
        goto done;
    }
memory_error:
    state("Not enough free memory. Installation cancelled.", "Memória livre insuficiente. Instalação cancelada.", 0);
done:
    cleanup();
    if (raw) heap_release(raw);
    if (verify) heap_release(verify);
    running = false; owner = 0;
    gui_invalidate_app(APP_SETTINGS);
}
