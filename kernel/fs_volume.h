#ifndef LUNA_FS_VOLUME_H
#define LUNA_FS_VOLUME_H
#include "install.h"

typedef struct {
    uint32_t start, slot_sectors;
    bool extended, exists;
} FsVolume;

/* Discover an existing data partition or reserve a free extent after the
 * system image. Publishing a new partition requires create=true. */
bool fs_volume(unsigned disk, bool create, FsVolume *out);
size_t fs_volume_bytes(const FsVolume *volume);
#endif
