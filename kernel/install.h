#ifndef LUNA_INSTALL_H
#define LUNA_INSTALL_H
#include "luna.h"
#define LUNA_IMAGE_BYTES (66u * 1024 * 1024)
#define LUNA_IMAGE_INFO_SECTOR 7168u
#define LUNA_SOURCES_SECTOR 32768u
#define ACCOUNT_ROUNDS 210000u
typedef struct {
    bool present, supported;
    bool optical;
    uint8_t index, multiple;
    uint64_t sectors;
    char model[41];
} DiskInfo;
void disk_init(void);
const DiskInfo *disk_info(unsigned);
unsigned disk_boot_index(void);
void disk_resolve_native_boot(void);
bool disk_read_on(unsigned, uint32_t, void *);
bool disk_write_on(unsigned, uint32_t, const void *);
bool disk_read_blocks(unsigned, uint32_t, unsigned, void *);
bool disk_write_blocks(unsigned, uint32_t, unsigned, const void *);
bool disk_flush(unsigned);
bool fs_save_to(unsigned);
bool fs_verify_on(unsigned);

bool account_load(void);
const char *account_name(void);
const char *account_home(void);
bool account_begin(const char *, const char *, bool verify);
bool account_poll(unsigned budget);
unsigned account_progress(void);
bool account_success(void);
bool account_store(void);
void account_clear_secret(void);

extern bool setup_active;
extern int setup_page;
void setup_init(void);
void setup_draw(void);
void setup_key(int);
void setup_pointer(int, int, int);
void setup_poll(void);
void setup_live(void);
#endif
