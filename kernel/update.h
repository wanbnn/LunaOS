#ifndef LUNA_UPDATE_H
#define LUNA_UPDATE_H
#include "luna.h"
#define UPDATE_CHUNK_BYTES (256u * 1024u)
#define UPDATE_RECORDS 248u
#define UPDATE_MANIFEST_BYTES (64u + UPDATE_RECORDS * 40u)
#define UPDATE_REPOSITORY "wanbnn/LunaOS"
/* Runs on the Settings task; network waits yield to the desktop. */
void update_run(void);
bool update_busy(void);
uint32_t update_owner(void);
unsigned update_progress(void);
const char *update_status(void);
const char *update_detail(void);
const char *update_revision(void);
/* Shared format validation, also used by the host contract tests. */
bool update_manifest_valid(const uint8_t *, size_t, const char *);
bool update_digest_valid(const void *, size_t, const uint8_t *);
int update_inflate(char *, int, const char *, int);
#endif
