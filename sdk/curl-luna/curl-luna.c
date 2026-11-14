/* libcurl-compatible HTTP/1.1 client for LunaOS guest programs.
 *
 * Scope: the upstream Git HTTP transport (http.c, remote-curl.c,
 * http-walker.c) drives this through the libcurl API. The transport itself
 * is a verified TLS/TCP stream provided by the LunaOS kernel
 * (SYS_EXEC_TCP_CONNECT), so this file owns the HTTP framing only:
 * request building, response parsing, chunked transfer in both
 * directions, redirects, basic authentication and connection reuse.
 *
 * There is no insecure TLS mode here: the kernel verifies the certificate
 * chain, the hostname and the validity period before a byte of HTTP is
 * exchanged, and the options libcurl would use to weaken that are ignored.
 */
#define _GNU_SOURCE 1
#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <zlib.h>
#ifdef LUNA_CURL_HOST_TEST
#include <netdb.h>
#include <sys/socket.h>
#endif

#ifdef LUNA_CURL_HOST_TEST
/* Host regression harness: HTTP-only loopback sockets, NEVER a TLS fallback. */
static int luna_stream_connect(const char *host, unsigned port, int secure)
{
    struct addrinfo hints, *addresses = NULL, *cursor;
    char service[8];
    int fd = -1;
    if (secure) {
        errno = EOPNOTSUPP;
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &addresses))
        return -1;
    for (cursor = addresses; cursor; cursor = cursor->ai_next) {
        fd = socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol);
        if (fd < 0)
            continue;
        if (!connect(fd, cursor->ai_addr, cursor->ai_addrlen))
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}
#else
/* LunaOS guest syscall: TCP or verified TLS, including peer hostname checks. */
#define LUNA_SYS_TCP_CONNECT 25

static long luna_syscall3(unsigned number, unsigned a, unsigned b, unsigned c)
{
    long result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "b"(a), "c"(b), "d"(c)
                     : "memory", "cc");
    return result;
}

static int luna_stream_connect(const char *host, unsigned port, int secure)
{
    long fd = luna_syscall3(LUNA_SYS_TCP_CONNECT, (unsigned)(uintptr_t)host,
                           port, secure ? 1u : 0u);
    if (fd < 3)
        return -1;
    return (int)fd;
}
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *dup_string(const char *value)
{
    size_t length;
    char *copy;
    if (!value)
        return NULL;
    length = strlen(value) + 1;
    copy = malloc(length);
    if (!copy)
        return NULL;
    memcpy(copy, value, length);
    return copy;
}

static char *trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t')
        text++;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    *end = 0;
    return text;
}

static int header_is(const char *line, const char *name)
{
    size_t length = strlen(name);
    if (strncasecmp(line, name, length))
        return 0;
    return line[length] == ':';
}

static void base64_encode(const char *data, char *out, size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = strlen(data);
    size_t input = 0, output = 0;
    size_t groups = (length + 2) / 3;
    if (groups * 4 + 1 > capacity)
        return;
    while (input < length) {
        unsigned char first = (unsigned char)data[input++];
        unsigned char second = input < length ? (unsigned char)data[input++] : 0;
        unsigned char third = input < length ? (unsigned char)data[input++] : 0;
        unsigned value = ((unsigned)first << 16) | ((unsigned)second << 8) | third;
        out[output++] = alphabet[(value >> 18) & 0x3f];
        out[output++] = alphabet[(value >> 12) & 0x3f];
        out[output++] = alphabet[(value >> 6) & 0x3f];
        out[output++] = alphabet[value & 0x3f];
    }
    switch (length % 3) {
    case 1:
        out[output - 2] = '=';
        out[output - 1] = '=';
        break;
    case 2:
        out[output - 1] = '=';
        break;
    default:
        break;
    }
    out[output] = 0;
}

/* ------------------------------------------------------------------ */
/* URL handling                                                         */
/* ------------------------------------------------------------------ */

struct luna_url {
    char scheme[8];
    char host[256];
    unsigned port;
    char path[2048];
};

static int url_parse(const char *url, struct luna_url *out)
{
    const char *p, *host_start, *host_end, *colon;

    if (!url)
        return -1;
    p = url;
    memset(out, 0, sizeof(*out));
    if (!strncasecmp(p, "http://", 7)) {
        strcpy(out->scheme, "http");
        out->port = 80;
        p += 7;
    } else if (!strncasecmp(p, "https://", 8)) {
        strcpy(out->scheme, "https");
        out->port = 443;
        p += 8;
    } else {
        return -1;
    }

    host_start = p;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    host_end = p;
    if (host_end == host_start || memchr(host_start, '@', (size_t)(host_end-host_start)))
        return -1; /* Userinfo belongs in CURLOPT_USERNAME / CURLOPT_PASSWORD. */
    if (*host_start == '[')
        return -1; /* IPv6 literals are not yet supported by this backend. */
    colon = memchr(host_start, ':', (size_t)(host_end - host_start));
    if (colon) {
        const char *digit;
        unsigned port = 0;
        if (colon == host_start || colon + 1 == host_end)
            return -1;
        for (digit = colon + 1; digit < host_end; digit++) {
            if (*digit < '0' || *digit > '9')
                return -1;
            port = port * 10u + (unsigned)(*digit - '0');
            if (port > 65535u)
                return -1;
        }
        if (!port)
            return -1;
        out->port = port;
        host_end = colon;
    }
    {
        size_t length = (size_t)(host_end - host_start);
        if (!length || length >= sizeof(out->host))
            return -1;
        memcpy(out->host, host_start, length);
        out->host[length] = 0;
    }
    {
        const char *fragment = strchr(p, '#');
        size_t length = fragment ? (size_t)(fragment - p) : strlen(p);
        if (*p == '?' ) {
            if (length + 1 >= sizeof(out->path))
                return -1;
            out->path[0] = '/';
            memcpy(out->path + 1, p, length);
            out->path[length + 1] = 0;
        } else if (!length) {
            strcpy(out->path, "/");
        } else {
            if (length >= sizeof(out->path))
                return -1;
            memcpy(out->path, p, length);
            out->path[length] = 0;
        }
    }
    return 0;
}

static unsigned url_default_port(const char *scheme)
{
    return !strcmp(scheme, "https") ? 443u : 80u;
}

/* Resolve a Location header against the URL it came from. */
static int url_resolve(const struct luna_url *base, const char *location,
                       char *out, size_t capacity)
{
    int written;
    if (!location || !*location)
        return -1;
    if (!strncasecmp(location, "http://", 7) ||
        !strncasecmp(location, "https://", 8)) {
        if (strlen(location) >= capacity)
            return -1;
        strcpy(out, location);
        return 0;
    }
    written = snprintf(out, capacity, "%s://%s", base->scheme, base->host);
    if (written < 0 || (size_t)written >= capacity)
        return -1;
    if (base->port != url_default_port(base->scheme)) {
        int more = snprintf(out + written, capacity - (size_t)written,
                            ":%u", base->port);
        if (more < 0 || (size_t)more >= capacity - (size_t)written)
            return -1;
        written += more;
    }
    if (location[0] == '/') {
        int more = snprintf(out + written, capacity - (size_t)written,
                            "%s", location);
        return (more < 0 || (size_t)more >= capacity - (size_t)written) ? -1 : 0;
    }
    {
        char base_path[2048];
        char *slash;
        size_t path_length;
        snprintf(base_path, sizeof(base_path), "%s", base->path);
        if ((slash = strchr(base_path, '?')))
            *slash = 0;
        slash = strrchr(base_path, '/');
        if (slash)
            slash[1] = 0;
        else
            strcpy(base_path, "/");
        path_length = strlen(base_path);
        if (path_length + strlen(location) >= sizeof(base_path))
            return -1;
        written = snprintf(out + written, capacity - (size_t)written,
                           "%s%s", base_path, location);
        if (written < 0 || (size_t)written >= capacity - (size_t)written)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Buffered reader over the guest stream                               */
/* ------------------------------------------------------------------ */

struct luna_reader {
    int fd;
    unsigned char buffer[8192];
    size_t position;
    size_t length;
    int eof;
    int error;
};

static void reader_init(struct luna_reader *reader, int fd)
{
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
}

static int reader_fill(struct luna_reader *reader)
{
    long got;
    if (reader->position < reader->length)
        return 1;
    if (reader->eof)
        return 0;
    got = read(reader->fd, reader->buffer, sizeof(reader->buffer));
    if (got < 0) {
        reader->error = 1;
        return -1;
    }
    if (got == 0) {
        reader->eof = 1;
        return 0;
    }
    reader->position = 0;
    reader->length = (size_t)got;
    return 1;
}

/* Read one CRLF terminated line. Returns the length without the line
 * ending, -1 on error and -2 when the peer closed without any data. */
static long reader_line(struct luna_reader *reader, char *out, size_t capacity)
{
    size_t used = 0;
    int any = 0;
    for (;;) {
        int filled = reader_fill(reader);
        if (filled < 0)
            return -1;
        if (filled == 0)
            return any ? (long)used : -2;
        while (reader->position < reader->length) {
            unsigned char byte = reader->buffer[reader->position++];
            any = 1;
            if (byte == '\n') {
                if (used && out[used - 1] == '\r')
                    used--;
                out[used] = 0;
                return (long)used;
            }
            if (used + 1 >= capacity)
                return -1;
            out[used++] = (char)byte;
        }
    }
}

static long reader_read(struct luna_reader *reader, void *out, size_t count)
{
    unsigned char *target = out;
    size_t done = 0;
    while (done < count) {
        int filled = reader_fill(reader);
        size_t available;
        if (filled < 0)
            return -1;
        if (filled == 0)
            break;
        available = reader->length - reader->position;
        if (available > count - done)
            available = count - done;
        memcpy(target + done, reader->buffer + reader->position, available);
        reader->position += available;
        done += available;
    }
    return (long)done;
}

/* ------------------------------------------------------------------ */
/* Request and response state                                          */
/* ------------------------------------------------------------------ */

struct luna_headers {
    char *location;
    char *content_type;
    char *content_encoding;
    char *www_authenticate;
    long content_length;
    int chunked;
    int has_content_length;
};

static void headers_init(struct luna_headers *headers)
{
    memset(headers, 0, sizeof(*headers));
    headers->content_length = -1;
}

static void headers_free(struct luna_headers *headers)
{
    free(headers->location);
    free(headers->content_type);
    free(headers->content_encoding);
    free(headers->www_authenticate);
    headers_init(headers);
}

struct luna_handle {
    char *url;
    char *method;
    struct curl_slist *headers;
    char *postfields;
    size_t postfieldsize;
    int postfieldsize_set;
    int post;
    int nobody;
    int upload;
    int followlocation;
    long maxredirs;
    char *username;
    char *password;
    char *userpwd;
    long httpauth;
    char *useragent;
    char *errorbuffer;
    size_t errorbuffersize;
    int failonerror;
    int verbose;
    char *proxy;
    curl_write_callback write_cb;
    void *write_data;
    curl_write_callback header_cb;
    void *header_data;
    curl_read_callback read_cb;
    void *read_data;
    curl_seek_callback seek_cb;
    void *seek_data;
    curl_off_t infilesize;
    int infilesize_set;
    long http_version;
    /* Result state. */
    long http_code;
    long connect_code;
    long auth_avail;
    long retry_after;
    char *content_type;
    char *effective_url;
    /* Authentication retry bookkeeping. */
    int auth_sent;
    int auth_challenged;
    /* Multi interface bookkeeping. */
    int in_use;
    int finished;
    CURLcode result;
};

struct luna_multi {
    struct luna_handle *handles[16];
    unsigned count;
};

static void set_error(struct luna_handle *handle, CURLcode code, const char *message)
{
    (void)code;
    if (handle->errorbuffer && handle->errorbuffersize) {
        strncpy(handle->errorbuffer, message, handle->errorbuffersize - 1);
        handle->errorbuffer[handle->errorbuffersize - 1] = 0;
    }
    if (handle->verbose)
        fprintf(stderr, "curl-luna: %s\n", message);
}

static int deliver_header(struct luna_handle *handle, const char *line)
{
    char header[4098];
    size_t length = strlen(line);
    if (length + 2 > sizeof(header))
        return -1;
    if (!handle->header_cb)
        return 0;
    memcpy(header, line, length);
    header[length++] = '\r';
    header[length++] = '\n';
    return handle->header_cb(header, 1, length, handle->header_data) == length
           ? 0 : -1;
}

static int deliver(struct luna_handle *handle, const void *data, size_t count)
{
    if (!handle->write_cb)
        return 0;
    if (handle->write_cb((char *)data, 1, count, handle->write_data) != count)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* gzip response decoding                                              */
/* ------------------------------------------------------------------ */

struct luna_gzip {
    z_stream stream;
    int started;
    int done;
};

static int gzip_init(struct luna_gzip *gzip)
{
    memset(gzip, 0, sizeof(*gzip));
    if (inflateInit2(&gzip->stream, 16 + MAX_WBITS) != Z_OK)
        return -1;
    gzip->started = 1;
    return 0;
}

static void gzip_free(struct luna_gzip *gzip)
{
    if (gzip->started) {
        inflateEnd(&gzip->stream);
        gzip->started = 0;
    }
}

static int gzip_push(struct luna_gzip *gzip, const unsigned char *data, size_t count,
                     struct luna_handle *handle)
{
    unsigned char output[8192];
    gzip->stream.next_in = (unsigned char *)data;
    gzip->stream.avail_in = (unsigned)count;
    while (gzip->stream.avail_in && !gzip->done) {
        int status;
        gzip->stream.next_out = output;
        gzip->stream.avail_out = (unsigned)sizeof(output);
        status = inflate(&gzip->stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR)
            return -1;
        if (gzip->stream.avail_out < sizeof(output)) {
            size_t produced = sizeof(output) - gzip->stream.avail_out;
            if (deliver(handle, output, produced))
                return -1;
        }
        if (status == Z_STREAM_END) {
            gzip->done = 1;
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* One HTTP exchange                                                   */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const void *data, size_t count)
{
    const char *cursor = data;
    size_t done = 0;
    while (done < count) {
        long wrote = write(fd, cursor + done, count - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0)
            return -1;
        done += (size_t)wrote;
    }
    return 0;
}

/* Build the credentials string for Basic authentication. */
static int handle_credentials(struct luna_handle *handle, char *out, size_t capacity)
{
    const char *user = handle->username ? handle->username : "";
    const char *password = handle->password ? handle->password : "";
    if (!handle->username && handle->userpwd) {
        /* The colon must be preserved: Basic encodes username:password. */
        size_t length = strlen(handle->userpwd);
        if (length >= capacity || !strchr(handle->userpwd, ':'))
            return -1;
        memcpy(out, handle->userpwd, length + 1);
        return 0;
    }
    {
        int length = snprintf(out, capacity, "%s:%s", user, password);
        return (length < 0 || (size_t)length >= capacity) ? -1 : 0;
    }
}

/* Perform one request/response exchange on a fresh connection. */
static CURLcode transfer_once(struct luna_handle *handle, const char *url,
                              struct luna_headers *headers, long *status_out)
{
    struct luna_url parsed;
    int fd;
    struct luna_reader reader;
    char line[4096];
    char method[16];
    long status = 0;
    CURLcode code = CURLE_OK;
    const char *body = NULL;
    size_t body_length = 0;
    int chunked_upload = 0;
    int sized_upload = 0;
    char request[16384];
    int length;
    int secure;

    if (url_parse(url, &parsed))
        return CURLE_URL_MALFORMAT;
    if (handle->proxy) {
        set_error(handle, CURLE_UNSUPPORTED_PROTOCOL,
                  "proxies are not supported by the LunaOS HTTP client");
        return CURLE_UNSUPPORTED_PROTOCOL;
    }

    /* Method selection. */
    if (handle->method && *handle->method)
        snprintf(method, sizeof(method), "%s", handle->method);
    else if (handle->post)
        snprintf(method, sizeof(method), "POST");
    else if (handle->upload)
        snprintf(method, sizeof(method), "PUT");
    else
        snprintf(method, sizeof(method), "GET");
    if (handle->nobody)
        snprintf(method, sizeof(method), "HEAD");

    /* Body selection. */
    if (handle->post && handle->postfields) {
        body = handle->postfields;
        body_length = handle->postfieldsize;
    } else if (handle->read_cb) {
        if (handle->infilesize_set && handle->infilesize >= 0)
            sized_upload = 1;
        else
            chunked_upload = 1;
    }
    /* No Expect handshake: send the request body without a speculative wait. */

    secure = !strcmp(parsed.scheme, "https");
    fd = luna_stream_connect(parsed.host, parsed.port, secure);
    if (fd < 0) {
        set_error(handle, CURLE_COULDNT_CONNECT,
                  "the LunaOS network stack could not open the connection");
        return CURLE_COULDNT_CONNECT;
    }
    handle->connect_code = 0; /* No HTTP proxy CONNECT tunnel. */
    reader_init(&reader, fd);
    handle->retry_after = -1;

    /* Request head. */
    length = snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\n", method,
                      parsed.path);
    if (parsed.port == url_default_port(parsed.scheme))
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "Host: %s\r\n", parsed.host);
    else
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "Host: %s:%u\r\n", parsed.host, parsed.port);
    length += snprintf(request + length, sizeof(request) - (size_t)length,
                       "User-Agent: %s\r\n",
                       handle->useragent ? handle->useragent : "git/curl-luna");
    length += snprintf(request + length, sizeof(request) - (size_t)length,
                       "Accept: */*\r\n");
    length += snprintf(request + length, sizeof(request) - (size_t)length,
                       "Connection: close\r\n");
    if (handle->auth_sent) {
        char credentials[512];
        char encoded[700];
        if (!handle_credentials(handle, credentials, sizeof(credentials))) {
            base64_encode(credentials, encoded, sizeof(encoded));
            length += snprintf(request + length, sizeof(request) - (size_t)length,
                               "Authorization: Basic %s\r\n", encoded);
        }
    }
    if (body) {
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "Content-Length: %lu\r\n", (unsigned long)body_length);
    } else if (sized_upload) {
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "Content-Length: %llu\r\n",
                           (unsigned long long)handle->infilesize);
    } else if (chunked_upload) {
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "Transfer-Encoding: chunked\r\n");
    }
    {
        struct curl_slist *entry;
        for (entry = handle->headers; entry; entry = entry->next) {
            const char *value = entry->data;
            if (!strncasecmp(value, "Expect:", 7))
                continue; /* Never advertise 100-continue on this backend. */
            length += snprintf(request + length, sizeof(request) - (size_t)length,
                               "%s\r\n", value);
        }
    }
    length += snprintf(request + length, sizeof(request) - (size_t)length, "\r\n");
    if (length <= 0 || (size_t)length >= sizeof(request)) {
        close(fd);
        return CURLE_OUT_OF_MEMORY;
    }
    if (write_all(fd, request, (size_t)length)) {
        close(fd);
        set_error(handle, CURLE_SEND_ERROR, "the request could not be sent");
        return CURLE_SEND_ERROR;
    }

    /* Request body. */
    if (body) {
        if (write_all(fd, body, body_length)) {
            close(fd);
            set_error(handle, CURLE_SEND_ERROR, "the request body could not be sent");
            return CURLE_SEND_ERROR;
        }
    } else if (sized_upload) {
        curl_off_t left = handle->infilesize;
        while (left > 0) {
            char buffer[4096];
            size_t want = left < (curl_off_t)sizeof(buffer) ? (size_t)left : sizeof(buffer);
            size_t produced = handle->read_cb(buffer, 1, want, handle->read_data);
            if (!produced || produced > want || write_all(fd, buffer, produced)) {
                close(fd);
                set_error(handle, CURLE_READ_ERROR, "sized upload aborted or truncated");
                return CURLE_READ_ERROR;
            }
            left -= (curl_off_t)produced;
        }
    } else if (chunked_upload) {
        char chunk[4096];
        for (;;) {
            size_t produced = handle->read_cb(chunk, 1, sizeof(chunk),
                                              handle->read_data);
            char header[32];
            if (!produced)
                break;
            if (produced > sizeof(chunk)) {
                close(fd);
                return CURLE_READ_ERROR;
            }
            snprintf(header, sizeof(header), "%lx\r\n", (unsigned long)produced);
            if (write_all(fd, header, strlen(header)) ||
                write_all(fd, chunk, produced) ||
                write_all(fd, "\r\n", 2)) {
                close(fd);
                set_error(handle, CURLE_SEND_ERROR,
                          "the request body could not be sent");
                return CURLE_SEND_ERROR;
            }
        }
        if (write_all(fd, "0\r\n\r\n", 5)) {
            close(fd);
            set_error(handle, CURLE_SEND_ERROR, "the request body could not be sent");
            return CURLE_SEND_ERROR;
        }
    }

    /* Consume informational responses before the final HTTP status. */
    for (;;) {
        long got = reader_line(&reader, line, sizeof(line));
        const char *space;
        if (got <= 0 || strncmp(line, "HTTP/1.", 7) ||
            !(space = strchr(line, ' '))) {
            close(fd);
            set_error(handle, CURLE_RECV_ERROR, "the server did not send a valid HTTP status");
            return CURLE_RECV_ERROR;
        }
        status = strtol(space + 1, NULL, 10);
        if (status < 100 || status > 599) {
            close(fd);
            return CURLE_RECV_ERROR;
        }
        if (status == 101) {
            close(fd);
            return CURLE_UNSUPPORTED_PROTOCOL;
        }
        if (status >= 200)
            break;
        /* 100 Continue / 103 Early Hints: skip their headers. */
        do {
            got = reader_line(&reader, line, sizeof(line));
        } while (got > 0);
        if (got < 0) {
            close(fd);
            return CURLE_RECV_ERROR;
        }
    }
    if (deliver_header(handle, line)) {
        close(fd);
        return CURLE_WRITE_ERROR;
    }

    /* Response headers. */
    for (;;) {
        long got = reader_line(&reader, line, sizeof(line));
        if (got < 0) {
            close(fd);
            return CURLE_RECV_ERROR;
        }
        if (!got) {
            if (deliver_header(handle, "")) {
                close(fd);
                return CURLE_WRITE_ERROR;
            }
            break;
        }
        if (deliver_header(handle, line)) {
            close(fd);
            return CURLE_WRITE_ERROR;
        }
        if (header_is(line, "Location")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                free(headers->location);
                headers->location = dup_string(trim((char *)colon + 1));
            }
        } else if (header_is(line, "Content-Length")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                headers->content_length = strtol(trim((char *)colon + 1), NULL, 10);
                headers->has_content_length = 1;
            }
        } else if (header_is(line, "Transfer-Encoding")) {
            const char *colon = strchr(line, ':');
            if (colon && strstr(trim((char *)colon + 1), "chunked"))
                headers->chunked = 1;
        } else if (header_is(line, "Content-Type")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                free(headers->content_type);
                headers->content_type = dup_string(trim((char *)colon + 1));
            }
        } else if (header_is(line, "Content-Encoding")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                free(headers->content_encoding);
                headers->content_encoding = dup_string(trim((char *)colon + 1));
            }
        } else if (header_is(line, "Retry-After")) {
            const char *colon = strchr(line, ':');
            if (colon)
                handle->retry_after = strtol(trim((char *)colon + 1), NULL, 10);
        } else if (header_is(line, "WWW-Authenticate")) {
            const char *colon = strchr(line, ':');
            if (colon) {
                free(headers->www_authenticate);
                headers->www_authenticate = dup_string(trim((char *)colon + 1));
            }
        }
    }
    *status_out = status;
    handle->http_code = status;
    free(handle->content_type);
    handle->content_type = headers->content_type ? dup_string(headers->content_type) : NULL;
    handle->auth_avail = CURLAUTH_BASIC;

    /* Response body. */
    if (status == 204 || status == 304 || handle->nobody ||
        !strcasecmp(method, "HEAD")) {
        close(fd);
        return CURLE_OK;
    }
    {
        int gzipped = headers->content_encoding &&
                      strstr(headers->content_encoding, "gzip");
        struct luna_gzip gzip;
        int gzip_ready = 0;
        if (gzipped && gzip_init(&gzip)) {
            close(fd);
            return CURLE_RECV_ERROR;
        }
        if (gzipped)
            gzip_ready = 1;
        if (headers->chunked) {
            for (;;) {
                long size;
                unsigned long chunk;
                char *end;
                size = reader_line(&reader, line, sizeof(line));
                if (size < 0) {
                    if (gzip_ready)
                        gzip_free(&gzip);
                    close(fd);
                    return CURLE_RECV_ERROR;
                }
                chunk = strtoul(line, &end, 16);
                if (end == line) {
                    if (gzip_ready)
                        gzip_free(&gzip);
                    close(fd);
                    return CURLE_RECV_ERROR;
                }
                while (chunk) {
                    char buffer[4096];
                    size_t want = chunk > sizeof(buffer) ? sizeof(buffer)
                                                        : (size_t)chunk;
                    long got = reader_read(&reader, buffer, want);
                    if (got <= 0) {
                        if (gzip_ready)
                            gzip_free(&gzip);
                        close(fd);
                        return CURLE_RECV_ERROR;
                    }
                    if (gzip_ready) {
                        if (gzip_push(&gzip, (unsigned char *)buffer, (size_t)got,
                                      handle)) {
                            gzip_free(&gzip);
                            close(fd);
                            return CURLE_WRITE_ERROR;
                        }
                    } else if (deliver(handle, buffer, (size_t)got)) {
                        close(fd);
                        return CURLE_WRITE_ERROR;
                    }
                    chunk -= (unsigned long)got;
                }
                if (reader_line(&reader, line, sizeof(line)) < 0) {
                    if (gzip_ready)
                        gzip_free(&gzip);
                    close(fd);
                    return CURLE_RECV_ERROR;
                }
                if (!chunk)
                    break;
            }
            /* Optional trailer section. */
            for (;;) {
                long size = reader_line(&reader, line, sizeof(line));
                if (size <= 0)
                    break;
            }
            if (gzip_ready)
                gzip_free(&gzip);
            close(fd);
            return CURLE_OK;
        }
        if (headers->has_content_length && headers->content_length >= 0) {
            long remaining = headers->content_length;
            char buffer[8192];
            while (remaining > 0) {
                size_t want = (size_t)remaining > sizeof(buffer) ? sizeof(buffer)
                                                                : (size_t)remaining;
                long got = reader_read(&reader, buffer, want);
                if (got <= 0) {
                    if (gzip_ready)
                        gzip_free(&gzip);
                    close(fd);
                    return CURLE_PARTIAL_FILE;
                }
                if (gzip_ready) {
                    if (gzip_push(&gzip, (unsigned char *)buffer, (size_t)got,
                                  handle)) {
                        gzip_free(&gzip);
                        close(fd);
                        return CURLE_WRITE_ERROR;
                    }
                } else if (deliver(handle, buffer, (size_t)got)) {
                    close(fd);
                    return CURLE_WRITE_ERROR;
                }
                remaining -= got;
            }
            if (gzip_ready)
                gzip_free(&gzip);
            close(fd);
            return CURLE_OK;
        }
        {
            /* The body ends when the peer closes the connection. */
            char buffer[8192];
            for (;;) {
                long got = reader_read(&reader, buffer, sizeof(buffer));
                if (got <= 0)
                    break;
                if (gzip_ready) {
                    if (gzip_push(&gzip, (unsigned char *)buffer, (size_t)got,
                                  handle)) {
                        gzip_free(&gzip);
                        close(fd);
                        return CURLE_WRITE_ERROR;
                    }
                } else if (deliver(handle, buffer, (size_t)got)) {
                    close(fd);
                    return CURLE_WRITE_ERROR;
                }
            }
        }
        if (gzip_ready)
            gzip_free(&gzip);
    }
    close(fd);
    return CURLE_OK;
}

/* Run the request, following redirects and retrying with credentials when
 * the server asks for authentication. */
static CURLcode perform_request(struct luna_handle *handle)
{
    char current[4096];
    struct luna_headers headers;
    long status = 0;
    int redirects = 0;
    int allow_credentials = 1;
    CURLcode code;

    if (!handle->url)
        return CURLE_URL_MALFORMAT;
    snprintf(current, sizeof(current), "%s", handle->url);
    headers_init(&headers);
    handle->auth_challenged = 0;
    handle->auth_sent = 0;
    /* curl only sends credentials up front when Basic was requested
     * explicitly; otherwise it waits for the 401 challenge. */
    if (handle->httpauth == CURLAUTH_BASIC && (handle->username || handle->userpwd))
        handle->auth_sent = 1;

    for (;;) {
        struct luna_url base;
        code = transfer_once(handle, current, &headers, &status);
        if (code != CURLE_OK)
            break;
        /* Authentication challenge: retry once with the credentials. */
        if (status == 401 && allow_credentials && !handle->auth_sent && !handle->auth_challenged &&
            (handle->username || handle->userpwd)) {
            handle->auth_sent = 1;
            handle->auth_challenged = 1;
            headers_free(&headers);
            continue;
        }
        if (!headers.location || !handle->followlocation)
            break;
        if (status != 301 && status != 302 && status != 303 && status != 307 &&
            status != 308)
            break;
        if (handle->maxredirs >= 0 && redirects >= handle->maxredirs) {
            code = CURLE_TOO_MANY_REDIRECTS;
            break;
        }
        if (url_parse(current, &base)) {
            code = CURLE_URL_MALFORMAT;
            break;
        }
        {
            char next[4096];
            struct luna_url redirected;
            if (url_resolve(&base, headers.location, next, sizeof(next)) ||
                url_parse(next, &redirected)) {
                code = CURLE_URL_MALFORMAT;
                break;
            }
            if (!strcmp(base.scheme, "https") && strcmp(redirected.scheme, "https")) {
                code = CURLE_UNSUPPORTED_PROTOCOL; /* Never downgrade TLS. */
                break;
            }
            if (strcmp(base.scheme, redirected.scheme) ||
                strcmp(base.host, redirected.host) || base.port != redirected.port) {
                allow_credentials = 0;
                handle->auth_sent = 0;
            }
            snprintf(current, sizeof(current), "%s", next);
        }
        if (status == 303)
            handle->post = 0;
        redirects++;
        headers_free(&headers);
    }
    headers_free(&headers);
    free(handle->effective_url);
    handle->effective_url = dup_string(current);
    handle->http_code = status;
    return code;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

CURL *curl_easy_init(void)
{
    struct luna_handle *handle = calloc(1, sizeof(*handle));
    if (!handle)
        return NULL;
    handle->maxredirs = -1;
    handle->httpauth = CURLAUTH_ANY;
    return handle;
}

CURLcode curl_easy_setopt(CURL *handle, int option, ...)
{
    va_list args;
    struct luna_handle *session = handle;
    if (!session)
        return CURLE_BAD_FUNCTION_ARGUMENT;
    va_start(args, option);
    switch (option) {
    case CURLOPT_URL:
        free(session->url);
        session->url = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_CUSTOMREQUEST:
        free(session->method);
        session->method = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_HTTPHEADER:
        session->headers = va_arg(args, struct curl_slist *);
        break;
    case CURLOPT_POSTFIELDS:
        /* libcurl borrows this buffer; Git may pass binary pack data. */
        session->postfields = va_arg(args, char *);
        if (!session->postfieldsize_set)
            session->postfieldsize = session->postfields ? strlen(session->postfields) : 0;
        session->post = 1;
        break;
    case CURLOPT_POSTFIELDSIZE: {
        long size = va_arg(args, long);
        session->postfieldsize_set = size >= 0;
        session->postfieldsize = size >= 0 ? (size_t)size :
                                 (session->postfields ? strlen(session->postfields) : 0);
        break;
    }
    case CURLOPT_POSTFIELDSIZE_LARGE: {
        curl_off_t size = va_arg(args, curl_off_t);
        if (size < -1 || (size > 0 &&
            (unsigned long long)size > (unsigned long long)SIZE_MAX)) {
            va_end(args);
            return CURLE_BAD_FUNCTION_ARGUMENT;
        }
        session->postfieldsize_set = size >= 0;
        session->postfieldsize = size >= 0 ? (size_t)size :
                                 (session->postfields ? strlen(session->postfields) : 0);
        break;
    }
    case CURLOPT_POST:
        session->post = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_HTTPGET:
        if (va_arg(args, long))
            session->post = 0;
        break;
    case CURLOPT_NOBODY:
        session->nobody = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_UPLOAD:
        session->upload = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_INFILESIZE:
        session->infilesize = va_arg(args, long);
        session->infilesize_set = 1;
        break;
    case CURLOPT_INFILESIZE_LARGE:
        session->infilesize = va_arg(args, curl_off_t);
        session->infilesize_set = 1;
        break;
    case CURLOPT_FOLLOWLOCATION:
        session->followlocation = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_MAXREDIRS:
        session->maxredirs = va_arg(args, long);
        break;
    case CURLOPT_USERNAME:
        free(session->username);
        session->username = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_PASSWORD:
        free(session->password);
        session->password = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_USERPWD:
        free(session->userpwd);
        session->userpwd = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_HTTPAUTH:
        session->httpauth = va_arg(args, long);
        break;
    case CURLOPT_USERAGENT:
        free(session->useragent);
        session->useragent = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_ERRORBUFFER:
        session->errorbuffer = va_arg(args, char *);
        session->errorbuffersize = CURL_ERROR_SIZE;
        break;
    case CURLOPT_FAILONERROR:
        session->failonerror = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_VERBOSE:
        session->verbose = va_arg(args, long) ? 1 : 0;
        break;
    case CURLOPT_PROXY:
        free(session->proxy);
        session->proxy = dup_string(va_arg(args, char *));
        break;
    case CURLOPT_WRITEFUNCTION:
        session->write_cb = va_arg(args, curl_write_callback);
        break;
    case CURLOPT_WRITEDATA:
        session->write_data = va_arg(args, void *);
        break;
    case CURLOPT_HEADERFUNCTION:
        session->header_cb = va_arg(args, curl_write_callback);
        break;
    case CURLOPT_HEADERDATA:
        session->header_data = va_arg(args, void *);
        break;
    case CURLOPT_READFUNCTION:
        session->read_cb = va_arg(args, curl_read_callback);
        break;
    case CURLOPT_READDATA:
        session->read_data = va_arg(args, void *);
        break;
    case CURLOPT_SEEKFUNCTION:
        session->seek_cb = va_arg(args, curl_seek_callback);
        break;
    case CURLOPT_SEEKDATA:
        session->seek_data = va_arg(args, void *);
        break;
    case CURLOPT_HTTP_VERSION:
        session->http_version = va_arg(args, long);
        break;
    case CURLOPT_PINNEDPUBLICKEY:
        /* Never pretend that requested certificate pinning was enforced. */
        if (va_arg(args, const char *)) {
            va_end(args);
            return CURLE_NOT_BUILT_IN;
        }
        break;
    case CURLOPT_ENCODING:
    case CURLOPT_CAINFO:
    case CURLOPT_CAPATH:
    case CURLOPT_SSLCERT:
    case CURLOPT_SSLCERTTYPE:
    case CURLOPT_SSLKEY:
    case CURLOPT_SSLKEYTYPE:
    case CURLOPT_KEYPASSWD:
    case CURLOPT_SSLVERSION:
    case CURLOPT_SSL_CIPHER_LIST:
    case CURLOPT_SSL_OPTIONS:
    case CURLOPT_SSL_VERIFYPEER:
    case CURLOPT_SSL_VERIFYHOST:
    case CURLOPT_USE_SSL:
    case CURLOPT_IPRESOLVE:
    case CURLOPT_PROTOCOLS:
    case CURLOPT_REDIR_PROTOCOLS:
    case CURLOPT_TCP_KEEPALIVE:
    case CURLOPT_TCP_KEEPIDLE:
    case CURLOPT_TCP_KEEPINTVL:
    case CURLOPT_LOW_SPEED_LIMIT:
    case CURLOPT_LOW_SPEED_TIME:
    case CURLOPT_COOKIEFILE:
    case CURLOPT_COOKIEJAR:
    case CURLOPT_NETRC:
    case CURLOPT_NOPROXY:
    case CURLOPT_PROXYHEADER:
    case CURLOPT_PROXY_CAINFO:
    case CURLOPT_PROXY_SSLCERT:
    case CURLOPT_PROXY_SSLKEY:
    case CURLOPT_PROXY_KEYPASSWD:
    case CURLOPT_PROXYUSERNAME:
    case CURLOPT_PROXYPASSWORD:
    case CURLOPT_PROXYAUTH:
    case CURLOPT_PROXYTYPE:
    case CURLOPT_RESOLVE:
    case CURLOPT_FTP_USE_EPSV:
    case CURLOPT_GSSAPI_DELEGATION:
    case CURLOPT_POSTREDIR:
    case CURLOPT_RANGE:
    case CURLOPT_REFERER:
    case CURLOPT_INFILE:
    case CURLOPT_DEBUGFUNCTION:
    case CURLOPT_DEBUGDATA:
    case CURLOPT_CONNECTTIMEOUT:
        /* Accepted and ignored: the LunaOS kernel owns the TLS policy and
         * the guest has neither proxies nor cookies. */
        (void)va_arg(args, long);
        break;
    default:
        va_end(args);
        return CURLE_UNKNOWN_OPTION;
    }
    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *handle)
{
    struct luna_handle *session = handle;
    CURLcode code;
    if (!session)
        return CURLE_BAD_FUNCTION_ARGUMENT;
    if (!session->url) {
        set_error(session, CURLE_URL_MALFORMAT, "no URL was given");
        return CURLE_URL_MALFORMAT;
    }
    code = perform_request(session);
    if (code == CURLE_OK && session->failonerror && session->http_code >= 400)
        code = CURLE_HTTP_RETURNED_ERROR;
    session->result = code;
    return code;
}

CURLcode curl_easy_getinfo(CURL *handle, int info, ...)
{
    struct luna_handle *session = handle;
    va_list args;
    if (!session)
        return CURLE_BAD_FUNCTION_ARGUMENT;
    va_start(args, info);
    switch (info) {
    case CURLINFO_RESPONSE_CODE:
        *(long *)va_arg(args, long *) = session->http_code;
        break;
    case CURLINFO_HTTP_CONNECTCODE:
        *(long *)va_arg(args, long *) = session->connect_code;
        break;
    case CURLINFO_HTTPAUTH_AVAIL:
        *(long *)va_arg(args, long *) = session->auth_avail;
        break;
    case CURLINFO_CONTENT_TYPE:
        *(char **)va_arg(args, char **) = session->content_type;
        break;
    case CURLINFO_EFFECTIVE_URL:
        *(char **)va_arg(args, char **) = session->effective_url;
        break;
    case CURLINFO_RETRY_AFTER:
        *(long *)va_arg(args, long *) = session->retry_after;
        break;
    default:
        va_end(args);
        return CURLE_UNKNOWN_OPTION;
    }
    va_end(args);
    return CURLE_OK;
}

void curl_easy_cleanup(CURL *handle)
{
    struct luna_handle *session = handle;
    if (!session)
        return;
    free(session->url);
    free(session->method);
    free(session->username);
    free(session->password);
    free(session->userpwd);
    free(session->useragent);
    free(session->proxy);
    free(session->content_type);
    free(session->effective_url);
    free(session);
}

CURL *curl_easy_duphandle(CURL *handle)
{
    struct luna_handle *source = handle;
    struct luna_handle *copy;
    if (!source)
        return NULL;
    copy = curl_easy_init();
    if (!copy)
        return NULL;
    copy->url = dup_string(source->url);
    copy->method = dup_string(source->method);
    copy->headers = source->headers;
    copy->postfields = source->postfields;
    copy->postfieldsize = source->postfieldsize;
    copy->postfieldsize_set = source->postfieldsize_set;
    copy->post = source->post;
    copy->nobody = source->nobody;
    copy->upload = source->upload;
    copy->followlocation = source->followlocation;
    copy->maxredirs = source->maxredirs;
    copy->username = dup_string(source->username);
    copy->password = dup_string(source->password);
    copy->userpwd = dup_string(source->userpwd);
    copy->httpauth = source->httpauth;
    copy->useragent = dup_string(source->useragent);
    copy->write_cb = source->write_cb;
    copy->write_data = source->write_data;
    copy->header_cb = source->header_cb;
    copy->header_data = source->header_data;
    copy->read_cb = source->read_cb;
    copy->read_data = source->read_data;
    copy->seek_cb = source->seek_cb;
    copy->seek_data = source->seek_data;
    copy->http_version = source->http_version;
    copy->auth_sent = source->auth_sent;
    return copy;
}

const char *curl_easy_strerror(CURLcode code)
{
    switch (code) {
    case CURLE_OK: return "no error";
    case CURLE_UNSUPPORTED_PROTOCOL: return "unsupported protocol";
    case CURLE_URL_MALFORMAT: return "malformed URL";
    case CURLE_COULDNT_RESOLVE_HOST: return "could not resolve host";
    case CURLE_COULDNT_CONNECT: return "could not connect";
    case CURLE_PARTIAL_FILE: return "partial file";
    case CURLE_WRITE_ERROR: return "write error";
    case CURLE_READ_ERROR: return "read error";
    case CURLE_OUT_OF_MEMORY: return "out of memory";
    case CURLE_OPERATION_TIMEDOUT: return "operation timed out";
    case CURLE_SSL_CONNECT_ERROR: return "SSL connect error";
    case CURLE_TOO_MANY_REDIRECTS: return "too many redirects";
    case CURLE_GOT_NOTHING: return "nothing was returned";
    case CURLE_RECV_ERROR: return "receive error";
    case CURLE_SEND_ERROR: return "send error";
    case CURLE_HTTP_RETURNED_ERROR: return "the server returned an error";
    case CURLE_BAD_FUNCTION_ARGUMENT: return "bad function argument";
    case CURLE_UNKNOWN_OPTION: return "unknown option";
    default: return "unknown error";
    }
}

CURLcode curl_global_init(long flags)
{
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void) { }

CURLMcode curl_global_sslset(long options, const char *name,
                             const curl_ssl_backend ***available)
{
    static const curl_ssl_backend backend = { "luna-bearssl", "1.0", 0x10000 };
    static const curl_ssl_backend *backends[] = { &backend, NULL };
    (void)options;
    (void)name;
    if (available)
        *available = backends;
    return CURLSSLSET_OK;
}

void curl_free(void *pointer)
{
    free(pointer);
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
    struct curl_slist *entry = calloc(1, sizeof(*entry));
    struct curl_slist *cursor;
    if (!entry)
        return list;
    entry->data = dup_string(data);
    entry->next = NULL;
    if (!list)
        return entry;
    for (cursor = list; cursor->next; cursor = cursor->next)
        ;
    cursor->next = entry;
    return entry;
}

void curl_slist_free_all(struct curl_slist *list)
{
    while (list) {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}

/* ------------------------------------------------------------------ */
/* Multi interface                                                      */
/* ------------------------------------------------------------------ */

CURLM *curl_multi_init(void)
{
    struct luna_multi *multi = calloc(1, sizeof(*multi));
    return multi;
}

CURLMcode curl_multi_add_handle(CURLM *multi, CURL *handle)
{
    struct luna_multi *session = multi;
    struct luna_handle *target = handle;
    unsigned index;
    if (!session || !target)
        return CURLM_BAD_EASY_HANDLE;
    for (index = 0; index < session->count; index++) {
        if (session->handles[index] == target)
            return CURLM_ADDED_ALREADY;
    }
    if (session->count >= sizeof(session->handles) / sizeof(session->handles[0]))
        return CURLM_OUT_OF_MEMORY;
    target->in_use = 1;
    target->finished = 0;
    target->result = CURLE_OK;
    session->handles[session->count++] = target;
    return CURLM_OK;
}

CURLMcode curl_multi_remove_handle(CURLM *multi, CURL *handle)
{
    struct luna_multi *session = multi;
    struct luna_handle *target = handle;
    unsigned index;
    if (!session || !target)
        return CURLM_BAD_EASY_HANDLE;
    for (index = 0; index < session->count; index++) {
        if (session->handles[index] == target) {
            unsigned move;
            for (move = index; move + 1 < session->count; move++)
                session->handles[move] = session->handles[move + 1];
            session->count--;
            target->in_use = 0;
            return CURLM_OK;
        }
    }
    return CURLM_BAD_EASY_HANDLE;
}

CURLMcode curl_multi_cleanup(CURLM *multi)
{
    free(multi);
    return CURLM_OK;
}

CURLMcode curl_multi_perform(CURLM *multi, int *running_handles)
{
    struct luna_multi *session = multi;
    unsigned index;
    if (!session)
        return CURLM_BAD_HANDLE;
    /* The LunaOS network backend owns a single stream, so transfers run one
     * after the other instead of concurrently. */
    for (index = 0; index < session->count; index++) {
        struct luna_handle *target = session->handles[index];
        if (target->finished)
            continue;
        target->result = curl_easy_perform(target);
        target->finished = 1;
    }
    /* Transfers are synchronous; completion messages remain queued. */
    if (running_handles)
        *running_handles = 0;
    return CURLM_OK;
}

CURLMcode curl_multi_timeout(CURLM *multi, long *milliseconds)
{
    (void)multi;
    if (milliseconds)
        *milliseconds = 0;
    return CURLM_OK;
}

CURLMcode curl_multi_fdset(CURLM *multi, fd_set *read_fd, fd_set *write_fd,
                            fd_set *exc_fd, int *max_fd)
{
    (void)multi;
    (void)read_fd;
    (void)write_fd;
    (void)exc_fd;
    if (max_fd)
        *max_fd = -1;
    return CURLM_OK;
}

CURLMsg *curl_multi_info_read(CURLM *multi, int *msgs_in_queue)
{
    struct luna_multi *session = multi;
    static CURLMsg message;
    unsigned index;
    if (!session)
        return NULL;
    for (index = 0; index < session->count; index++) {
        struct luna_handle *target = session->handles[index];
        if (target->finished == 1) {
            message.msg = CURLMSG_DONE;
            message.easy_handle = target;
            message.data.result = target->result;
            target->finished = 2;
            if (msgs_in_queue)
                *msgs_in_queue = 1;
            return &message;
        }
    }
    if (msgs_in_queue)
        *msgs_in_queue = 0;
    return NULL;
}

const char *curl_multi_strerror(CURLMcode code)
{
    switch (code) {
    case CURLM_OK: return "no error";
    case CURLM_BAD_HANDLE: return "invalid multi handle";
    case CURLM_BAD_EASY_HANDLE: return "invalid easy handle";
    case CURLM_OUT_OF_MEMORY: return "out of memory";
    case CURLM_INTERNAL_ERROR: return "internal error";
    case CURLM_ADDED_ALREADY: return "the handle is already added";
    default: return "unknown multi error";
    }
}

/* ------------------------------------------------------------------ */
/* URL API                                                              */
/* ------------------------------------------------------------------ */

struct luna_url_handle {
    char *url;
};

CURLU *curl_url(void)
{
    struct luna_url_handle *handle = calloc(1, sizeof(*handle));
    return handle;
}

CURLUcode curl_url_set(CURLU *handle, unsigned int part, const char *content,
                        unsigned int flags)
{
    struct luna_url_handle *session = handle;
    (void)flags;
    if (!session)
        return CURLUE_BAD_HANDLE;
    switch (part) {
    case CURLUPART_URL:
        free(session->url);
        session->url = dup_string(content);
        return content ? CURLUE_OK : CURLUE_BAD_PARTPOINTER;
    case CURLUPART_USER:
    case CURLUPART_PASSWORD:
        /* Clearing the credentials strips them from the stored URL. */
        if (!content && session->url) {
            char *at = strchr(session->url, '@');
            char *scheme = strstr(session->url, "://");
            if (at && scheme && at > scheme) {
                size_t prefix = (size_t)(scheme - session->url) + 3;
                char *clean = malloc(prefix + strlen(at + 1) + 1);
                if (clean) {
                    memcpy(clean, session->url, prefix);
                    strcpy(clean + prefix, at + 1);
                    free(session->url);
                    session->url = clean;
                }
            }
        }
        return CURLUE_OK;
    default:
        return CURLUE_OK;
    }
}

CURLUcode curl_url_get(CURLU *handle, unsigned int part, char **content,
                        unsigned int flags)
{
    struct luna_url_handle *session = handle;
    (void)flags;
    if (!session || !content)
        return CURLUE_BAD_HANDLE;
    if (part != CURLUPART_URL)
        return CURLUE_BAD_PARTPOINTER;
    *content = dup_string(session->url);
    return *content ? CURLUE_OK : CURLUE_OUT_OF_MEMORY;
}

void curl_url_cleanup(CURLU *handle)
{
    struct luna_url_handle *session = handle;
    if (!session)
        return;
    free(session->url);
    free(session);
}

curl_version_info_data *curl_version_info(int age)
{
    static curl_version_info_data info;
    (void)age;
    memset(&info, 0, sizeof(info));
    info.name = "curl-luna";
    info.version = LIBCURL_VERSION;
    info.version_num = LIBCURL_VERSION_NUM;
    info.host = "lunaos";
    info.features = CURL_VERSION_SSL;
    info.ssl_version = "bearssl";
    info.ssl_version_num = 0x10000;
    info.protocols = "http https";
    return &info;
}
