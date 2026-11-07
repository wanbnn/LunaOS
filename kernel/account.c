#include "bearssl.h"
#include "install.h"
#include "web.h"
static char username[25] = "luna", home[40] = "/home/luna";
static uint8_t salt[16], password_hash[32];
static unsigned rounds = ACCOUNT_ROUNDS;
static bool loaded, running, success, verifying;
static br_hmac_key_context key;
static uint8_t u[32], derived[32];
static unsigned iteration;
static void erase(void *ptr, size_t n) {
    volatile uint8_t *p = ptr;
    while (n--)
        *p++ = 0;
}
static bool valid_user(const char *s) {
    if (!s || *s < 'a' || *s > 'z' || strlen(s) > 24)
        return false;
    for (; *s; s++)
        if ((*s < 'a' || *s > 'z') && (*s < '0' || *s > '9') && *s != '_' && *s != '-')
            return false;
    return true;
}
static void hex_bytes(char *out, const uint8_t *data, unsigned n) {
    const char *digits = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    out[n * 2] = 0;
}
static int digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static bool unhex(const char *s, uint8_t *out, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int a = digit(s[i * 2]), b = digit(s[i * 2 + 1]);
        if (a < 0 || b < 0)
            return false;
        out[i] = (a << 4) | b;
    }
    return s[n * 2] == 0;
}
static bool field(const char *text, const char *name, char *out, unsigned capacity) {
    size_t len = strlen(name);
    for (const char *p = text; *p;) {
        const char *end = p;
        while (*end && *end != '\n')
            end++;
        if (!strncmp(p, name, len) && p[len] == '=') {
            unsigned n = end - p - len - 1;
            if (n >= capacity)
                return false;
            memcpy(out, p + len + 1, n);
            out[n] = 0;
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}
bool account_load(void) {
    loaded = false;
    File *f = fs_find("/etc/luna-user");
    if (!f || f->dir)
        return false;
    char user[25], s[65], hash[65], r[16], algorithm[32];
    if (!field(f->data, "algorithm", algorithm, sizeof(algorithm)) ||
        strcmp(algorithm, "pbkdf2-sha256") || !field(f->data, "username", user, sizeof(user)) ||
        !valid_user(user) || !field(f->data, "salt", s, sizeof(s)) || strlen(s) != 32 ||
        !unhex(s, salt, 16) || !field(f->data, "hash", hash, sizeof(hash)) || strlen(hash) != 64 ||
        !unhex(hash, password_hash, 32) || !field(f->data, "rounds", r, sizeof(r)))
        return false;
    rounds = atoi(r);
    if (rounds < 100000 || rounds > 1000000)
        return false;
    strcpy(username, user);
    strcpy(home, "/home/");
    strcat(home, username);
    loaded = true;
    return true;
}
const char *account_name(void) {
    return username;
}
const char *account_home(void) {
    return home;
}
bool account_begin(const char *user, const char *password, bool verify) {
    if (running || !password || strlen(password) > 64 || strlen(password) < 8)
        return false;
    if (verify) {
        if (!loaded)
            return false;
    } else {
        if (!valid_user(user) || !random_bytes(salt, sizeof(salt)))
            return false;
        strcpy(username, user);
        strcpy(home, "/home/");
        strcat(home, user);
        rounds = ACCOUNT_ROUNDS;
    }
    br_hmac_key_init(&key, &br_sha256_vtable, password, strlen(password));
    br_hmac_context ctx;
    br_hmac_init(&ctx, &key, 32);
    br_hmac_update(&ctx, salt, sizeof(salt));
    const uint8_t counter[4] = {0, 0, 0, 1};
    br_hmac_update(&ctx, counter, 4);
    br_hmac_out(&ctx, u);
    memcpy(derived, u, 32);
    iteration = 1;
    running = true;
    success = false;
    verifying = verify;
    return true;
}
bool account_poll(unsigned budget) {
    if (!running)
        return true;
    if (budget > 2048)
        budget = 2048;
    while (budget-- && iteration < rounds) {
        br_hmac_context ctx;
        br_hmac_init(&ctx, &key, 32);
        br_hmac_update(&ctx, u, 32);
        br_hmac_out(&ctx, u);
        for (unsigned i = 0; i < 32; i++)
            derived[i] ^= u[i];
        iteration++;
    }
    if (iteration < rounds)
        return false;
    if (verifying) {
        unsigned difference = 0;
        for (unsigned i = 0; i < 32; i++)
            difference |= derived[i] ^ password_hash[i];
        success = difference == 0;
    } else {
        memcpy(password_hash, derived, 32);
        success = true;
        loaded = true;
    }
    running = false;
    account_clear_secret();
    return true;
}
unsigned account_progress(void) {
    return rounds ? iteration * 100 / rounds : 0;
}
bool account_success(void) {
    return success;
}
void account_clear_secret(void) {
    erase(&key, sizeof(key));
    erase(u, sizeof(u));
    erase(derived, sizeof(derived));
}
bool account_store(void) {
    if (!loaded || !success || verifying)
        return false;
    char body[512] = "algorithm=pbkdf2-sha256\nusername=", n[16], encoded[65];
    strcat(body, username);
    strcat(body, "\nrounds=");
    number(n, rounds);
    strcat(body, n);
    strcat(body, "\nsalt=");
    hex_bytes(encoded, salt, 16);
    strcat(body, encoded);
    strcat(body, "\nhash=");
    hex_bytes(encoded, password_hash, 32);
    strcat(body, encoded);
    strcat(body, "\n");
    if (!fs_find(home) && fs_create(home, true))
        return false;
    return !fs_write("/etc/luna-user", body, false);
}
