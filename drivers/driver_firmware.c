#include "driver.h"
#define FW_SECTOR 16384u
#define FW_LIMIT (2u * 1024 * 1024)
#define FW_MAX_FILE (512u * 1024)
#define FW_ENTRIES 16
#define FW_NAME 96
static uint32_t hash_bytes(const uint8_t *p, unsigned n, uint32_t hash) {
    while (n--) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}
static bool valid_name(const char *s) {
    if (!s || !*s || *s == '/' || strlen(s) >= FW_NAME)
        return false;
    const char *part = s;
    for (const char *p = s;; p++) {
        if (*p && ((*p < 'a' || *p > 'z') && (*p < 'A' || *p > 'Z') && (*p < '0' || *p > '9') &&
                   *p != '_' && *p != '-' && *p != '.' && *p != '/'))
            return false;
        if (!*p || *p == '/') {
            unsigned len = p - part;
            if (!len || (len == 1 && *part == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            if (!*p)
                return true;
            part = p + 1;
        }
    }
}
int drv_firmware_request(const char *name, DrvFirmware *out) {
    if (!out || !valid_name(name))
        return DRV_EINVAL;
    if (out->data || out->allocation.cookie)
        return DRV_EBUSY;
    if (drv_in_irq() || !drv_irqs_enabled())
        return DRV_EDEADLK;
    uint8_t sector[512], directory[FW_ENTRIES * 128];
    if (!disk_read(FW_SECTOR, sector))
        return DRV_EIO;
    if (memcmp(sector, "LUNAFW1\0", 8))
        return DRV_ENOENT;
    uint32_t version, count, total, dir_hash;
    memcpy(&version, sector + 8, 4);
    memcpy(&count, sector + 12, 4);
    memcpy(&total, sector + 16, 4);
    memcpy(&dir_hash, sector + 20, 4);
    if (version != 1 || count > FW_ENTRIES || total > FW_LIMIT || total < 512 || total % 512)
        return DRV_EIO;
    unsigned dir_bytes = count * 128, sectors = (dir_bytes + 511) / 512;
    if (total < (1 + sectors) * 512)
        return DRV_EIO;
    for (unsigned i = 0; i < sectors; i++)
        if (!disk_read(FW_SECTOR + 1 + i, directory + i * 512))
            return DRV_EIO;
    if (hash_bytes(directory, dir_bytes, 2166136261u) != dir_hash)
        return DRV_EIO;
    const uint8_t *selected = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *entry = directory + i * 128;
        bool terminated = false;
        for (unsigned j = 0; j < FW_NAME; j++)
            if (!entry[j]) {
                terminated = true;
                break;
            }
        if (!terminated || !valid_name((char *)entry))
            return DRV_EIO;
        for (unsigned j = 0; j < i; j++)
            if (!strcmp((char *)entry, (char *)(directory + j * 128)))
                return DRV_EIO;
        uint32_t offset, size;
        memcpy(&offset, entry + 96, 4);
        memcpy(&size, entry + 100, 4);
        if (offset % 512 || offset < (1 + sectors) * 512 || !size || size > FW_MAX_FILE ||
            offset > total || size > total - offset)
            return DRV_EIO;
        for (unsigned j = 0; j < i; j++) {
            uint32_t prev_off, prev_size;
            memcpy(&prev_off, directory + j * 128 + 96, 4);
            memcpy(&prev_size, directory + j * 128 + 100, 4);
            if (offset < prev_off + prev_size && prev_off < offset + size)
                return DRV_EIO;
        }
        if (!strcmp(name, (char *)entry))
            selected = entry;
    }
    if (!selected)
        return DRV_ENOENT;
    uint32_t offset, size, expected;
    memcpy(&offset, selected + 96, 4);
    memcpy(&size, selected + 100, 4);
    memcpy(&expected, selected + 104, 4);
    int r = drv_dma_alloc(size, 16, UINT32_MAX, 0, &out->allocation);
    if (r)
        return r;
    uint8_t *data = out->allocation.cpu;
    uint32_t hash = 2166136261u;
    for (unsigned done = 0; done < size;) {
        if (!disk_read(FW_SECTOR + (offset + done) / 512, sector)) {
            r = DRV_EIO;
            break;
        }
        unsigned n = size - done;
        if (n > 512)
            n = 512;
        memcpy(data + done, sector, n);
        hash = hash_bytes(sector, n, hash);
        done += n;
    }
    if (r || hash != expected) {
        drv_dma_free(&out->allocation);
        return DRV_EIO;
    }
    out->data = data;
    out->size = size;
    out->checksum = hash;
    return 0;
}
int drv_firmware_release(DrvFirmware *firmware) {
    if (!firmware)
        return DRV_EINVAL;
    int r = drv_dma_free(&firmware->allocation);
    if (!r)
        memset(firmware, 0, sizeof(*firmware));
    return r;
}
