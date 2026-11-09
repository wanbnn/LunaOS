#ifndef LUNA_WEB_H
#define LUNA_WEB_H
#include "luna.h"
#define WEB_CAP 524288
bool net_url_resolve(const char *,const char *,char *,size_t);
bool net_http_final_url(char *,size_t);
int stream_connect(const char *, unsigned);
int stream_read(void *, unsigned);
int stream_write(const void *, unsigned);
void stream_close(void);
void stream_set_read_policy(uint32_t, uint32_t);
bool tls_connect(const char *, char *, size_t);
int tls_read(void *, unsigned);
int tls_write(const void *, unsigned);
void tls_close(void);
unsigned tls_error(void);
uint32_t rtc_epoch_seconds(void);
bool random_bytes(void *, size_t);

int javascript_eval(const char *, char *, size_t);
int javascript_eval_sdk(const char *, const char *, char *, size_t);
void gui_benchmark(char *, size_t);
#endif
