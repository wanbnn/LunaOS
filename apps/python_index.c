#include "python_wheel.h"
#include "heap.h"
#include <string.h>
#include "quickjs.h"

#define PYPI_API "https://pypi.org/pypi"
#define PYPI_FILES "https://files.pythonhosted.org/"

static bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool python_package_normalize(const char *name, size_t len, char *out, size_t cap) {
    if (!len || len >= cap || !alnum(name[0]) || !alnum(name[len - 1]))
        return false;
    size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = name[i];
        if (alnum(c)) {
            out[used++] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        } else if (c == '_' || c == '.' || c == '-') {
            if (out[used - 1] != '-')
                out[used++] = '-';
        } else {
            return false;
        }
    }
    out[used] = 0;
    return true;
}

static bool valid_version(const char *version) {
    size_t len = strlen(version);
    if (!len || len > 64 || version[0] < '0' || version[0] > '9')
        return false;
    for (const char *p = version; *p; ++p)
        if (!alnum(*p) && *p != '.' && *p != '-' && *p != '_')
            return false;
    return true;
}

int python_fetch(const char *url, char **data, size_t *size, char *out, size_t cap) {
    char error[192] = "";
    int result;
    if (!strncmp(url, "https://", 8)) {
        result = net_https_alloc(url, data, size, error, sizeof(error));
    } else if (!strncmp(url, "http://", 7)) {
        result = net_http_alloc(url, data, size, error, sizeof(error));
    } else {
        append(out, "pip: expected an HTTP or HTTPS URL\n", cap);
        return -1;
    }
    if (result >= 0)
        return 0;
    append(out, "pip: ", cap);
    append(out, *error ? error : "request failed", cap);
    append(out, "\n", cap);
    return -1;
}

static bool get_string(JSContext *ctx, JSValueConst obj, const char *key,
                       char *out, size_t cap) {
    JSValue field = JS_GetPropertyStr(ctx, obj, key);
    size_t len;
    const char *str = JS_IsString(field) ? JS_ToCStringLen(ctx, &len, field) : NULL;
    bool ok = str && len < cap && strlen(str) == len;
    if (ok)
        memcpy(out, str, len + 1);
    if (str)
        JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, field);
    return ok;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool get_digest(JSContext *ctx, JSValueConst file, uint8_t *digest) {
    char hash[2 * PYTHON_DIGEST_BYTES + 1];
    JSValue digests = JS_GetPropertyStr(ctx, file, "digests");
    bool ok = get_string(ctx, digests, "sha256", hash, sizeof(hash)) &&
              strlen(hash) == sizeof(hash) - 1;
    JS_FreeValue(ctx, digests);
    if (!ok)
        return false;
    for (unsigned i = 0; i < PYTHON_DIGEST_BYTES; ++i) {
        int high = nibble(hash[2 * i]), low = nibble(hash[2 * i + 1]);
        if (high < 0 || low < 0)
            return false;
        digest[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool read_candidate(JSContext *ctx, JSValueConst file, bool pypi, const char *project,
                           const char *version, char *url, size_t cap, uint8_t *digest) {
    char filename[PATH_CAP], info[PATH_CAP], candidate[PATH_CAP];
    JSValue yanked = JS_GetPropertyStr(ctx, file, "yanked");
    bool usable = !JS_ToBool(ctx, yanked);
    JS_FreeValue(ctx, yanked);
    if (!usable || !get_string(ctx, file, "filename", filename, sizeof(filename)) ||
        !python_wheel_name(filename, info, sizeof(info)))
        return false;
    const char *separator = strchr(info, '-');
    if (!python_package_normalize(info, separator - info, candidate, sizeof(candidate)) ||
        strcmp(candidate, project) || !get_string(ctx, file, "url", url, cap))
        return false;
    size_t version_len = strlen(version);
    if (version_len && (strncmp(separator + 1, version, version_len) ||
                        strcmp(separator + 1 + version_len, ".dist-info")))
        return false;
    if (pypi && strncmp(url, PYPI_FILES, sizeof(PYPI_FILES) - 1))
        return false;
    const char *leaf = strrchr(url, '/');
    return leaf && !strcmp(leaf + 1, filename) && get_digest(ctx, file, digest);
}

static bool select_wheel(const char *json, size_t bytes, const char *project, const char *version,
                         bool pypi, char *url, size_t cap, uint8_t *digest) {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt)
        return false;
    JS_SetMemoryLimit(rt, 8u * 1024 * 1024);
    JS_SetMaxStackSize(rt, 48u * 1024);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return false;
    }

    bool found = false;
    JSValue root = JS_ParseJSON(ctx, json, bytes, "package index");
    JSValue files = JS_GetPropertyStr(ctx, root, "urls");
    if (JS_IsArray(ctx, files)) {
        JSValue length = JS_GetPropertyStr(ctx, files, "length");
        uint32_t count = 0;
        JS_ToUint32(ctx, &count, length);
        JS_FreeValue(ctx, length);
        for (uint32_t i = 0; i < count && i < 512 && !found; ++i) {
            JSValue file = JS_GetPropertyUint32(ctx, files, i);
            found = read_candidate(ctx, file, pypi, project, version, url, cap, digest);
            JS_FreeValue(ctx, file);
        }
    }
    JS_FreeValue(ctx, files);
    JS_FreeValue(ctx, root);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return found;
}

bool python_index_find(const char *requirement, const char *index, char *url, size_t url_cap,
                       uint8_t digest[PYTHON_DIGEST_BYTES], char *out, size_t cap) {
    char project[96], endpoint[512];
    const char *equals = strchr(requirement, '=');
    if (!python_package_normalize(requirement, equals ? (size_t)(equals - requirement) :
                                  strlen(requirement), project, sizeof(project)) ||
        (equals && (equals[1] != '=' || !valid_version(equals + 2)))) {
        append(out, "pip: expected NAME or NAME==VERSION\n", cap);
        return false;
    }
    const char *version = equals ? equals + 2 : "";
    const char *base = index ? index : PYPI_API;
    size_t len = strlen(base);
    if (!len || len + strlen(project) + strlen(version) + sizeof("///json") > sizeof(endpoint)) {
        append(out, "pip: invalid index URL\n", cap);
        return false;
    }
    strcpy(endpoint, base);
    if (endpoint[len - 1] == '/')
        endpoint[len - 1] = 0;
    strcat(endpoint, "/");
    strcat(endpoint, project);
    if (*version) {
        strcat(endpoint, "/");
        strcat(endpoint, version);
    }
    strcat(endpoint, "/json");

    char *body = NULL;
    size_t bytes = 0;
    if (python_fetch(endpoint, &body, &bytes, out, cap))
        return false;
    bool ok = body && select_wheel(body, bytes, project, version, !index, url, url_cap, digest);
    heap_release(body);
    if (!ok)
        append(out, "pip: index did not provide a compatible wheel with a SHA-256 digest\n", cap);
    return ok;
}
