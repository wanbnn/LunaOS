#ifndef LUNA_HTTP_STREAM_H
#define LUNA_HTTP_STREAM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*HttpRead)(void *, void *, unsigned);
typedef bool (*HttpSink)(void *, const void *, size_t);
typedef struct {
    HttpRead read;
    void *context;
    uint8_t buffer[16384];
    unsigned position, length;
    int last;
} HttpInput;
typedef struct {
    unsigned status;
    bool chunked, have_length;
    uint32_t length;
    char location[4096];
} HttpReply;
bool http_stream_headers(HttpInput *, HttpReply *, char *, size_t);
bool http_stream_body(HttpInput *, const HttpReply *, HttpSink, void *, uint64_t *, char *, size_t);
#endif
