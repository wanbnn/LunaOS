/* Real update engine + package + production inflate/SHA, with failure-injected disk/HTTPS. */
#include "update.h"
#include "install.h"
#include "settings.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool net_ready = true, setup_active;
uint32_t memory_mb = 256;
volatile uint32_t frame_clock;
static Task current = {.pid = 9};
static DiskInfo info = {.present = true, .supported = true, .sectors = LUNA_IMAGE_BYTES / 512};
static uint8_t *disk, *original, *wanted;
static uint8_t source_manifest[UPDATE_MANIFEST_BYTES];
static unsigned requests, writes, reads, restarts, allocations;
static int fail_download, corrupt_chunk, fail_write, fail_read, fail_flush, corrupt_readback, fail_alloc, bad_manifest;
static bool save_ok = true, same_commit, enable_delta;
static unsigned transient_failures;
Task *task_current(void) { return &current; }
const char *system_text(const char *en, const char *pt) { (void)pt; return en; }
void gui_invalidate_app(int app) { assert(app == APP_SETTINGS); }
void task_sleep(uint32_t ms) { (void)ms; assert(update_busy() && update_owner() == 9); }
void reboot(void) { restarts++; }
bool settings_write(void) { return save_ok; }
bool fs_save(void) { return save_ok; }
unsigned disk_boot_index(void) { return 0; }
const DiskInfo *disk_info(unsigned index) { assert(!index); return &info; }
void *heap_allocate_owned(size_t n, uint32_t owner) {
    assert(owner == 1);
    if (fail_alloc && ++allocations == (unsigned)fail_alloc) return NULL;
    return malloc(n);
}
int heap_release(void *p) { free(p); return 0; }
bool disk_read_blocks(unsigned index, uint32_t sector, unsigned count, void *out) {
    assert(!index && count <= 64 && sector + count <= info.sectors);
    reads++;
    if (fail_read && reads == (unsigned)fail_read) return false;
    memcpy(out, disk + sector * 512, count * 512);
    if (corrupt_readback && writes) { ((uint8_t *)out)[0] ^= 1; corrupt_readback = 0; }
    return true;
}
bool disk_read_on(unsigned index, uint32_t sector, void *out) { return disk_read_blocks(index, sector, 1, out); }
bool disk_write_blocks(unsigned index, uint32_t sector, unsigned count, const void *data) {
    assert(!index && count <= 64 && sector + count <= info.sectors);
    /* No write may touch either live filesystem snapshot slot. */
    assert(sector + count <= 8192 || sector >= 16384);
    memcpy(disk + sector * 512, data, count * 512);
    writes++;
    if (fail_write && writes == (unsigned)fail_write) return false;
    return true;
}
bool disk_flush(unsigned index) {
    assert(!index);
    if (fail_flush) { fail_flush = 0; return false; }
    return true;
}
void append(char *out, const char *s, size_t cap) {
    size_t n = strlen(out), len = strlen(s);
    if (n + len >= cap) len = cap - n - 1;
    memcpy(out + n, s, len); out[n + len] = 0;
}
int net_https(const char *url, char *out, size_t cap, char *error, size_t ecap) {
    (void)ecap;
    requests++;
    error[0] = 0;
    assert(strstr(url, "/git/ref/heads/main") && cap > 80);
    return snprintf(out, cap, "{\"object\":{\"sha\":\"%.*s\"}}", 40,
                    same_commit ? (const uint8_t *)"0000000000000000000000000000000000000000" :
                    source_manifest + 24);
}
/* Simulated HTTPS streams exercise the real v2 parser, SHA decoder, and
 * disk installer, using on-disk packages produced by the Python release tool. */
bool net_https_stream(const char *url, NetHttpsSink sink, void *context, uint64_t *written,
                      char *error, size_t ecap) {
    requests++;
    error[0] = 0;
    *written = 0;
    bool delta = strstr(url, "/update-from-") != 0;
    if (delta && !enable_delta) {
        snprintf(error, ecap, "HTTP status 404.");
        return false;
    }
    if (transient_failures) {
        transient_failures--;
        snprintf(error, ecap, "Transport interrupted.");
        return false;
    }
    if (fail_download) {
        snprintf(error, ecap, "HTTP status 503.");
        return false;
    }
    char path[256];
    if (delta)
        snprintf(path, sizeof(path), "build/update-test/update-from-%040d.pack", 0);
    else
        strcpy(path, "build/update-test/update.pack");
    FILE *f = fopen(path, "rb");
    assert(f);
    unsigned char data[4096];
    size_t n;
    uint64_t offset = 0;
    bool accepted = true;
    while ((n = fread(data, 1, sizeof(data), f))) {
        if (bad_manifest && offset <= 64 && offset + n > 64)
            data[64 - offset] ^= 1;
        if (corrupt_chunk && !delta && offset <= UPDATE_MANIFEST_BYTES &&
            offset + n > UPDATE_MANIFEST_BYTES) {
            data[UPDATE_MANIFEST_BYTES - offset] ^= 1;
            corrupt_chunk = 0;
        }
        if (!sink(context, data, n)) {
            accepted = false;
            snprintf(error, ecap, "Bundle stream rejected.");
            break;
        }
        *written += n;
        offset += n;
    }
    assert(!ferror(f));
    fclose(f);
    return accepted;
}
static void reset(void) {
    memcpy(disk, original, LUNA_IMAGE_BYTES);
    requests = writes = reads = restarts = allocations = 0;
    transient_failures = 0;
    fail_download = corrupt_chunk = fail_write = fail_read = fail_flush = corrupt_readback = fail_alloc = bad_manifest = 0;
    net_ready = save_ok = true; same_commit = setup_active = enable_delta = false; memory_mb = 256;
}
static void unchanged(void) {
    assert(!restarts && !memcmp(disk, original, LUNA_IMAGE_BYTES));
    assert(!update_busy() && !update_owner());
}
int main(void) {
    disk = malloc(LUNA_IMAGE_BYTES); original = malloc(LUNA_IMAGE_BYTES); wanted = malloc(LUNA_IMAGE_BYTES);
    assert(disk && original && wanted);
    FILE *f = fopen("dist/LunaOS.img", "rb"); assert(f);
    assert(fread(wanted, 1, LUNA_IMAGE_BYTES, f) == LUNA_IMAGE_BYTES); fclose(f);
    f = fopen("build/update-test/update.manifest", "rb"); assert(f);
    assert(fread(source_manifest, 1, sizeof(source_manifest), f) == sizeof(source_manifest)); fclose(f);
    char revision[41]; memcpy(revision, source_manifest + 24, 40); revision[40] = 0;
    assert(update_manifest_valid(source_manifest, sizeof(source_manifest), revision));
    assert(!update_manifest_valid(source_manifest, sizeof(source_manifest) - 1, revision));
    uint8_t malformed[UPDATE_MANIFEST_BYTES]; memcpy(malformed, source_manifest, sizeof(malformed));
    malformed[64] = 0; malformed[65] = 32; /* live LunaFS sector 8192 */
    assert(!update_manifest_valid(malformed, sizeof(malformed), revision));
    memcpy(malformed, source_manifest, sizeof(malformed)); malformed[20]--;
    assert(!update_manifest_valid(malformed, sizeof(malformed), revision));
    revision[0] = revision[0] == 'a' ? 'b' : 'a';
    assert(!update_manifest_valid(source_manifest, sizeof(source_manifest), revision));
    memcpy(original, wanted, LUNA_IMAGE_BYTES);
    /* Installed ID, user data, and old kernel bytes must survive failed updates. */
    memcpy(original + 440, "TEST", 4);
    memset(original + 8192 * 512, 0xa5, 4096 * 1024);
    original[100000] ^= 0x77;
    reset(); net_ready = false; update_run(); unchanged(); assert(!requests && !writes);
    reset(); same_commit = true; update_run(); unchanged(); assert(requests == 1 && !writes && update_progress() == 100);
    reset(); setup_active = true; update_run(); unchanged(); assert(!requests && !writes);
    reset(); memcpy(disk + 440, "ANUL", 4); update_run(); assert(!requests && !writes && !restarts);
    reset(); memory_mb = 64; update_run(); unchanged(); assert(requests == 1 && !writes);
    reset(); bad_manifest = 1; update_run(); unchanged(); assert(!writes);
    reset(); fail_download = 1; update_run(); unchanged(); assert(!writes);
    reset(); corrupt_chunk = 1; update_run(); unchanged(); assert(!writes);
    reset(); fail_alloc = 4; update_run(); unchanged(); assert(!writes);
    reset(); save_ok = false; update_run(); unchanged(); assert(!writes);
    reset(); fail_read = 2; update_run(); unchanged(); assert(!writes);
    reset(); fail_write = 11; update_run(); unchanged(); assert(writes > 11);
    reset(); fail_flush = 1; update_run(); unchanged(); assert(writes);
    reset(); corrupt_readback = 1; update_run(); unchanged(); assert(writes);
    reset();
    transient_failures = 2;
    update_run();
    assert(!transient_failures);
    memcpy(wanted + 432, original + 432, 78);
    memcpy(wanted + 8192 * 512, original + 8192 * 512, 4096 * 1024);
    assert(restarts == 1 && !memcmp(wanted, disk, LUNA_IMAGE_BYTES));
    assert(!update_busy() && !update_owner() && update_progress() == 100);
    /* Exact-base differential: only changed blocks are in one streamed pack. */
    f = fopen("build/update-test/base.img", "rb"); assert(f);
    assert(fread(original, 1, LUNA_IMAGE_BYTES, f) == LUNA_IMAGE_BYTES); fclose(f);
    memcpy(original + 440, "TEST", 4);
    memset(original + 8192 * 512, 0xa5, 4096 * 1024);
    reset(); enable_delta = true;
    update_run();
    assert(requests == 2 && restarts == 1 && !memcmp(wanted, disk, LUNA_IMAGE_BYTES));
    /* A modified unselected sector must trigger full-package fallback BEFORE writing. */
    original[3 * UPDATE_CHUNK_BYTES + 17] ^= 0x2c;
    reset(); enable_delta = true;
    update_run();
    assert(requests == 3 && restarts == 1 && !memcmp(wanted, disk, LUNA_IMAGE_BYTES));
    free(disk); free(original); free(wanted);
    puts("PASS: automatic install/restart, user-data preservation, corrupt/missing downloads, allocation failure, write/flush/readback rollback");
    return 0;
}
