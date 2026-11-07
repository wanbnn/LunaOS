#include "../include/net/time_dns.h"
#include "luna.h"
static unsigned u16(const uint8_t *p) {
    return (unsigned)p[0] * 256 + p[1];
}
static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
bool time_hostname_valid(const char *s) {
    if (!s || !*s)
        return false;
    unsigned label = 0, total = 0;
    for (const char *p = s; *p; p++) {
        if (++total > 127)
            return false;
        if (*p == '.') {
            if (!label || p[-1] == '-')
                return false;
            label = 0;
        } else {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-'))
                return false;
            if ((!label && *p == '-') || ++label > 63)
                return false;
        }
    }
    return label && s[total - 1] != '-';
}
int time_dns_question(const char *host, uint16_t id, uint8_t *out, size_t cap, unsigned *written) {
    if (!out || !written || !time_hostname_valid(host))
        return -22;
    *written = 0;
    size_t len = strlen(host);
    if (cap < len + 18)
        return -28;
    memset(out, 0, 12);
    out[0] = id >> 8;
    out[1] = id;
    out[2] = 1;
    out[5] = 1;
    unsigned n = 12;
    for (const char *p = host; *p;) {
        const char *end = p;
        while (*end && *end != '.')
            end++;
        out[n++] = end - p;
        while (p < end)
            out[n++] = lower(*p++);
        if (*p)
            p++;
    }
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = 0;
    out[n++] = 1;
    *written = n;
    return 0;
}
static int name(const uint8_t *p, size_t n, unsigned *cursor, char out[128]) {
    unsigned pos = *cursor, used = 0, steps = 0;
    bool jumped = false;
    for (;;) {
        if (pos >= n || ++steps > 128)
            return -22;
        unsigned len = p[pos++];
        if ((len & 192) == 192) {
            if (pos >= n)
                return -22;
            unsigned target = ((len & 63) << 8) | p[pos++];
            if (target >= n)
                return -22;
            if (!jumped)
                *cursor = pos;
            jumped = true;
            pos = target;
            continue;
        }
        if (len & 192 || len > 63 || len > n - pos)
            return -22;
        if (!len) {
            if (!jumped)
                *cursor = pos;
            out[used] = 0;
            return 0;
        }
        if (used) {
            if (used + 1 >= 128)
                return -22;
            out[used++] = '.';
        }
        if (used + len >= 128)
            return -22;
        for (unsigned i = 0; i < len; i++)
            out[used++] = lower((char)p[pos++]);
    }
}
int time_dns_answer(const char *host, uint16_t id, const uint8_t *p, size_t n, uint8_t address[4]) {
    if (!host || !p || !address || n < 12 || n > 512 || u16(p) != id || !(p[2] & 128) ||
        (p[2] & 0x7a) || (p[3] & 15) || u16(p + 4) != 1 || u16(p + 6) > 16)
        return -22;
    unsigned pos = 12;
    char question[128], expected[128];
    if (!time_hostname_valid(host))
        return -22;
    for (unsigned i = 0; i <= strlen(host); i++)
        expected[i] = lower(host[i]);
    if (name(p, n, &pos, question) || strcmp(question, expected) || n - pos < 4 ||
        u16(p + pos) != 1 || u16(p + pos + 2) != 1)
        return -13;
    pos += 4;
    struct {
        char owner[128], target[128];
        uint8_t ip[4];
        unsigned type;
    } records[16];
    unsigned records_used = 0;
    for (unsigned i = 0; i < u16(p + 6); i++) {
        if (name(p, n, &pos, question) || n - pos < 10)
            return -22;
        unsigned type = u16(p + pos), cls = u16(p + pos + 2), len = u16(p + pos + 8);
        pos += 10;
        if (len > n - pos)
            return -22;
        if (cls == 1 && (type == 1 || type == 5)) {
            unsigned k = records_used++;
            strcpy(records[k].owner, question);
            records[k].type = type;
            if (type == 1) {
                if (len != 4)
                    return -22;
                memcpy(records[k].ip, p + pos, 4);
            } else {
                unsigned cursor = pos;
                if (name(p, n, &cursor, records[k].target) || cursor != pos + len ||
                    !time_hostname_valid(records[k].target))
                    return -22;
            }
        }
        pos += len;
    }
    for (unsigned chain = 0; chain < 8; chain++) {
        bool alias = false;
        for (unsigned i = 0; i < records_used; i++)
            if (!strcmp(records[i].owner, expected)) {
                if (records[i].type == 1) {
                    const uint8_t *ip = records[i].ip;
                    if (!ip[0] || ip[0] == 127 || ip[0] >= 224)
                        return -13;
                    memcpy(address, ip, 4);
                    return 0;
                }
                strcpy(question, records[i].target);
                alias = true;
            }
        if (!alias)
            return -2;
        if (!strcmp(question, expected))
            return -22;
        strcpy(expected, question);
    }
    return -22;
}
