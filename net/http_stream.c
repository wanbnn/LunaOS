#include "../include/net/http_stream.h"
#include "luna.h"
static void error_text(char *out, const char *message, size_t capacity) {
    if (!capacity)
        return;
    size_t n = strlen(message);
    if (n >= capacity)
        n = capacity - 1u;
    memcpy(out, message, n);
    out[n] = 0;
}
static bool equals_ci(const char *a, const char *b, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            return false;
    }
    return true;
}
static int download_fill(HttpInput *input) {
    if (input->position < input->length)
        return 1;
    int n = input->read(input->context, input->buffer, sizeof(input->buffer));
    input->last = n;
    input->position = 0;
    input->length = n > 0 ? (unsigned)n : 0;
    return n > 0 ? 1 : n;
}
static bool download_line(HttpInput *input, char *line, size_t capacity) {
    size_t used = 0;
    for (;;) {
        if (download_fill(input) <= 0)
            return false;
        uint8_t byte = input->buffer[input->position++];
        if (byte == '\n') {
            if (!used || line[used - 1] != '\r')
                return false;
            line[used - 1] = 0;
            return true;
        }
        if (!byte || used + 1u >= capacity)
            return false;
        line[used++] = (char)byte;
    }
}
static bool download_bytes(HttpInput *input, uint32_t bytes, HttpSink sink, void *context,
                           uint64_t *written) {
    while (bytes) {
        if (download_fill(input) <= 0)
            return false;
        unsigned n = input->length - input->position;
        if (n > bytes)
            n = bytes;
        if (!sink(context, input->buffer + input->position, n))
            return false;
        input->position += n;
        bytes -= n;
        *written += n;
    }
    return true;
}
static bool download_length(const char *text, uint32_t *value, unsigned base) {
    uint32_t result = 0;
    unsigned digits = 0;
    while (*text == ' ' || *text == '\t')
        text++;
    while (*text) {
        int digit = *text >= '0' && *text <= '9'   ? *text - '0'
                    : *text >= 'a' && *text <= 'f' ? *text - 'a' + 10
                    : *text >= 'A' && *text <= 'F' ? *text - 'A' + 10
                                                   : -1;
        if (digit < 0 || (unsigned)digit >= base)
            break;
        if (result > (UINT32_MAX - (unsigned)digit) / base)
            return false;
        result = result * base + (unsigned)digit;
        text++;
        digits++;
    }
    if (!digits)
        return false;
    while (*text == ' ' || *text == '\t')
        text++;
    if (*text && !(base == 16 && *text == ';'))
        return false;
    *value = result;
    return true;
}

bool http_stream_headers(HttpInput *input, HttpReply *reply, char *error, size_t ecap) {
    *reply = (HttpReply){0};
    char line[8192];
    if (!download_line(input, line, sizeof(line)) || strlen(line) < 12 ||
        strncmp(line, "HTTP/1.", 7) || line[9] < '0' || line[9] > '9' || line[10] < '0' ||
        line[10] > '9' || line[11] < '0' || line[11] > '9') {
        error_text(error, "Invalid HTTP response.", ecap);
        return false;
    }
    reply->status = (line[9] - '0') * 100u + (line[10] - '0') * 10u + line[11] - '0';
    size_t header_bytes = 0;
    reply->location[0] = 0;
    for (;;) {
        if (!download_line(input, line, sizeof(line))) {
            error_text(error, "Incomplete HTTP headers.", ecap);
            return false;
        }
        header_bytes += strlen(line) + 2u;
        if (header_bytes > 32768u) {
            error_text(error, "HTTP headers too large.", ecap);
            return false;
        }
        if (!line[0])
            break;
        char *value = strchr(line, ':');
        if (!value) {
            error_text(error, "Malformed HTTP header.", ecap);
            return false;
        }
        *value++ = 0;
        while (*value == ' ' || *value == '\t')
            value++;
        if (strlen(line) == 14 && equals_ci(line, "Content-Length", 14)) {
            uint32_t parsed;
            if (!download_length(value, &parsed, 10) ||
                (reply->have_length && parsed != reply->length)) {
                error_text(error, "Invalid Content-Length or unsupported file size.", ecap);
                return false;
            }
            reply->length = parsed;
            reply->have_length = true;
        } else if (strlen(line) == 17 && equals_ci(line, "Transfer-Encoding", 17)) {
            if (strlen(value) != 7 || !equals_ci(value, "chunked", 7)) {
                error_text(error, "Unsupported HTTP transfer encoding.", ecap);
                return false;
            }
            reply->chunked = true;
        } else if (strlen(line) == 16 && equals_ci(line, "Content-Encoding", 16)) {
            if (strlen(value) != 8 || !equals_ci(value, "identity", 8)) {
                error_text(error, "Compressed HTTP responses are not supported.", ecap);
                return false;
            }
        } else if (strlen(line) == 8 && equals_ci(line, "Location", 8)) {
            if (strlen(value) >= sizeof(reply->location)) {
                error_text(error, "Redirect URL too long.", ecap);
                return false;
            }
            strcpy(reply->location, value);
        }
    }

    if (reply->chunked && reply->have_length) {
        error_text(error, "Conflicting HTTP framing.", ecap);
        return false;
    }
    return true;
}
bool http_stream_body(HttpInput *input, const HttpReply *reply, HttpSink sink, void *context,
                      uint64_t *written, char *error, size_t ecap) {
    char line[8192];
    if (reply->chunked) {
        for (;;) {
            uint32_t size;
            if (!download_line(input, line, sizeof(line)) || !download_length(line, &size, 16)) {
                error_text(error, "Incomplete chunked response.", ecap);
                return false;
            }
            if (!size) {
                size_t trailers = 0;
                do {
                    if (!download_line(input, line, sizeof(line))) {
                        error_text(error, "Incomplete chunked response trailers.", ecap);
                        return false;
                    }
                    trailers += strlen(line) + 2u;
                    if (trailers > 32768u) {
                        error_text(error, "HTTP trailers too large.", ecap);
                        return false;
                    }
                } while (line[0]);
                break;
            }
            if (!download_bytes(input, size, sink, context, written)) {
                error_text(error, "Incomplete chunked response or disk write failed.", ecap);
                return false;
            }
            if (!download_line(input, line, sizeof(line)) || line[0]) {
                error_text(error, "Malformed chunked response delimiter.", ecap);
                return false;
            }
        }
    } else if (reply->have_length) {
        if (!download_bytes(input, reply->length, sink, context, written)) {
            error_text(error,
                       "Transport interrupted; incomplete HTTP document or disk write failed.",
                       ecap);
            return false;
        }
    } else {
        while (download_fill(input) > 0) {
            unsigned n = input->length - input->position;
            if (!download_bytes(input, n, sink, context, written)) {
                error_text(error, "Download disk write failed.", ecap);
                return false;
            }
        }
    }

    return true;
}
