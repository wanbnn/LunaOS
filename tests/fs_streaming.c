#define _POSIX_C_SOURCE 200809L
#include "../include/net/http_stream.h"
#include "../kernel/fs_volume.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define DISK_BYTES (UINT64_C(3) * 1024 * 1024 * 1024)
#define FILE_BYTES (UINT32_C(1024) * 1024 * 1024 + 123u)
#define HEAP_BUDGET (2u * 1024 * 1024)
static FILE *medium;
static size_t heap_live, heap_peak;
static int writes_left = -1;
static bool corrupt_write;
static DiskInfo device = {.present = true, .supported = true, .sectors = DISK_BYTES / 512};
typedef struct {
    size_t bytes;
} Allocation;
void *heap_allocate_owned(size_t bytes, uint32_t owner) {
    assert(owner == 1);
    if (bytes > HEAP_BUDGET - heap_live)
        return 0;
    Allocation *allocation = malloc(sizeof(*allocation) + bytes);
    if (!allocation)
        return 0;
    allocation->bytes = bytes;
    heap_live += bytes;
    if (heap_live > heap_peak)
        heap_peak = heap_live;
    return allocation + 1;
}
int heap_release(void *pointer) {
    if (pointer) {
        Allocation *allocation = (Allocation *)pointer - 1;
        heap_live -= allocation->bytes;
        free(allocation);
    }
    return 0;
}
const DiskInfo *disk_info(unsigned disk) {
    return disk ? 0 : &device;
}
unsigned disk_boot_index(void) {
    return 0;
}
bool disk_read_blocks(unsigned disk, uint32_t sector, unsigned count, void *out) {
    if (disk || (uint64_t)sector + count > device.sectors)
        return false;
    return pread(fileno(medium), out, (size_t)count * 512u, (off_t)sector * 512u) ==
           (ssize_t)count * 512;
}
bool disk_write_blocks(unsigned disk, uint32_t sector, unsigned count, const void *data) {
    if (disk || (uint64_t)sector + count > device.sectors || writes_left == 0)
        return false;
    if (writes_left > 0)
        writes_left--;
    bool written = pwrite(fileno(medium), data, (size_t)count * 512u, (off_t)sector * 512u) ==
                   (ssize_t)count * 512;
    if (written && corrupt_write && count > 1) {
        uint8_t bad = ((const uint8_t *)data)[(size_t)count * 512u - 1u] ^ 0xffu;
        assert(pwrite(fileno(medium), &bad, 1, (off_t)sector * 512u + (off_t)count * 512u - 1) ==
               1);
        corrupt_write = false;
    }
    return written;
}
bool disk_read_on(unsigned d, uint32_t s, void *p) {
    return disk_read_blocks(d, s, 1, p);
}
bool disk_write_on(unsigned d, uint32_t s, const void *p) {
    return disk_write_blocks(d, s, 1, p);
}
bool disk_flush(unsigned disk) {
    return !disk && !fsync(fileno(medium));
}
void serial_write(const char *text) {
    (void)text;
}
static void store32(uint8_t *out, uint32_t value) {
    memcpy(out, &value, 4);
}
static void fill(uint8_t *out, uint32_t offset, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        uint32_t position = offset + (uint32_t)i;
        out[i] = (uint8_t)(position * 37u + (position >> 8) + (position >> 24));
    }
}
static void sample(const File *file, uint32_t offset) {
    uint8_t expected[4096], actual[4096];
    size_t bytes = file->size - offset;
    if (bytes > sizeof(actual))
        bytes = sizeof(actual);
    fill(expected, offset, bytes);
    assert(fs_read_bytes(file, offset, actual, bytes));
    assert(!memcmp(expected, actual, bytes));
}
static uint64_t response_position;
static char response_header[160];
static int response_read(void *context, void *out, unsigned capacity) {
    (void)context;
    size_t header_bytes = strlen(response_header);
    uint64_t total = header_bytes + (uint64_t)FILE_BYTES;
    if (response_position >= total)
        return 0;
    size_t n = total - response_position;
    if (n > capacity)
        n = capacity;
    uint8_t *data = out;
    size_t count = n;
    if (response_position < header_bytes) {
        size_t prefix = header_bytes - response_position;
        if (prefix > count)
            prefix = count;
        memcpy(data, response_header + response_position, prefix);
        response_position += prefix;
        data += prefix;
        count -= prefix;
    }
    if (count) {
        fill(data, (uint32_t)(response_position - header_bytes), count);
        response_position += count;
    }
    return (int)n;
}
static bool response_sink(void *context, const void *data, size_t bytes) {
    (void)context;
    return fs_write_part(data, bytes);
}
int main(void) {
    struct rlimit memory_limit = {128u * 1024 * 1024, 128u * 1024 * 1024};
    assert(!setrlimit(RLIMIT_AS, &memory_limit));
    medium = tmpfile();
    assert(medium);
    assert(!ftruncate(fileno(medium), (off_t)DISK_BYTES));
    uint8_t sector[512] = {0};
    sector[510] = 0x55;
    sector[511] = 0xaa;
    sector[450] = 0xda;
    store32(sector + 454, LUNA_VOLUME_START);
    store32(sector + 458, 65536 - LUNA_VOLUME_START);
    sector[466] = 0xef;
    store32(sector + 470, 65536);
    store32(sector + 474, 69632);
    assert(disk_write_on(0, 0, sector));
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "LUNAIMG1", 8);
    store32(sector + 8, 1);
    store32(sector + 12, LUNA_IMAGE_BYTES);
    assert(disk_write_on(0, LUNA_IMAGE_INFO_SECTOR, sector));
    fs_init();
    assert(fs_find("/home/luna"));
    assert(fs_begin_write("/home/luna/gigabyte.bin", false, 42));
    assert(fs_create("/home/luna/concurrent", false) < 0);
    snprintf(response_header, sizeof(response_header),
             "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", FILE_BYTES);
    HttpInput input = {.read = response_read};
    HttpReply reply;
    char error[256] = {0};
    uint64_t written = 0;
    assert(http_stream_headers(&input, &reply, error, sizeof(error)));
    assert(reply.status == 200 && reply.length == FILE_BYTES);
    assert(http_stream_body(&input, &reply, response_sink, 0, &written, error, sizeof(error)));
    assert(written == FILE_BYTES);
    assert(fs_finish_write());
    File *file = fs_find("/home/luna/gigabyte.bin");
    assert(file && file->size == FILE_BYTES && !file->data);
    assert(!fs_rename("/home/luna/gigabyte.bin", "/home/luna/renamed.bin"));
    assert(!fs_find("/home/luna/gigabyte.bin"));
    file = fs_find("/home/luna/renamed.bin");
    sample(file, 0);
    assert(!fs_rename("/home/luna/renamed.bin", "/home/luna/gigabyte.bin"));
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, 0);
    sample(file, 512u * 1024 * 1024);
    sample(file, FILE_BYTES - 4096u);
    uint8_t expected[16384], actual[16384];
    for (uint32_t offset = 0; offset < FILE_BYTES;) {
        size_t n = FILE_BYTES - offset;
        if (n > sizeof(expected))
            n = sizeof(expected);
        fill(expected, offset, n);
        assert(fs_read_bytes(file, offset, actual, n));
        assert(!memcmp(expected, actual, n));
        offset += (uint32_t)n;
    }
    assert(fs_load());
    file = fs_find("/home/luna/gigabyte.bin");
    assert(file && !file->data && file->size == FILE_BYTES);
    sample(file, FILE_BYTES - 4096u);
    /* A failed replacement keeps the old gigabyte and the snapshot readable. */
    assert(fs_begin_write("/home/luna/gigabyte.bin", false, 42));
    assert(fs_write_part("partial", 7));
    fs_abort_write(42);
    assert(fs_load());
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, FILE_BYTES - 4096u);
    assert(fs_begin_write("/home/luna/gigabyte.bin", false, 42));
    assert(!fs_write_part("x", UINT32_MAX));
    assert(!fs_finish_write());
    assert(fs_load());
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, 0);
    assert(fs_begin_write("/home/luna/gigabyte.bin", false, 42));
    uint8_t damaged[32768] = {0};
    corrupt_write = true;
    assert(fs_write_part(damaged, sizeof(damaged)));
    assert(!fs_finish_write());
    assert(fs_load());
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, FILE_BYTES - 4096u);
    /* A clone pins its disk-backed source until restored or discarded. */
    FileState backup = {0};
    assert(!fs_clone(&backup, files));
    assert(fs_save());
    assert(!fs_save());
    fs_restore_state(&backup);
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, 0);
    writes_left = 1;
    assert(!fs_save());
    writes_left = -1;
    assert(fs_load());
    file = fs_find("/home/luna/gigabyte.bin");
    sample(file, FILE_BYTES - 4096u);
    assert(heap_peak < HEAP_BUDGET);
    struct rusage usage;
    assert(!getrusage(RUSAGE_SELF, &usage));
    assert(usage.ru_maxrss < 64 * 1024);
    printf("PASS: %u-byte HTTP-streamed disk-backed file, 128 MiB address-space limit, heap peak "
           "%zu bytes, RSS %ld KiB, restart and interrupted writes\n",
           FILE_BYTES, heap_peak, usage.ru_maxrss);
    fclose(medium);
    return 0;
}
