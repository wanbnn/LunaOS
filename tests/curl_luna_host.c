/* Host-only HTTP regression harness. This does not simulate TLS or prove guest compatibility. */
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } \
} while (0)

struct result {
    char data[256];
    size_t used;
    int status_lines;
    int final_headers;
};
struct stream {
    const char *data;
    size_t length;
    size_t offset;
};
static size_t body_cb(char *ptr, size_t size, size_t count, void *opaque)
{
    struct result *r = opaque;
    size_t n = size * count;
    if (r->used + n >= sizeof(r->data))
        return 0;
    memcpy(r->data + r->used, ptr, n);
    r->used += n;
    r->data[r->used] = 0;
    return n;
}
static size_t header_cb(char *ptr, size_t size, size_t count, void *opaque)
{
    struct result *r = opaque;
    size_t n = size * count;
    if (n < 2 || ptr[n-2] != '\r' || ptr[n-1] != '\n')
        return 0;
    if (n >= 5 && !memcmp(ptr, "HTTP/", 5))
        r->status_lines++;
    if (n == 2)
        r->final_headers++;
    return n;
}
static size_t read_cb(char *buffer, size_t size, size_t count, void *opaque)
{
    struct stream *s = opaque;
    size_t n = size * count;
    if (n > s->length - s->offset)
        n = s->length - s->offset;
    memcpy(buffer, s->data + s->offset, n);
    s->offset += n;
    return n;
}
static CURL *prepare(const char *url, struct result *result)
{
    CURL *h = curl_easy_init();
    if (!h)
        return NULL;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, result);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, result);
    return h;
}
static void make_url(char *out, size_t length, int port, const char *path)
{
    snprintf(out, length, "http://127.0.0.1:%d%s", port, path);
}
int main(int argc, char **argv)
{
    int port;
    char url[512];
    long status = -1;
    CURL *h, *duplicate;
    CURLM *multi;
    CURLMsg *msg;
    struct result result = {{0},0,0,0};
    const char binary[] = {'a', '\0', 'b', 'c'};
    struct stream stream = {"stream-data",11,0};
    int running = -1, queued = 0;
    if (argc != 2)
        return 2;
    port = atoi(argv[1]);

    make_url(url, sizeof(url), port, "/ok?value=1");
    h = prepare(url, &result);
    CHECK(h && curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "OK") && result.status_lines == 1);
    CHECK(result.final_headers == 1);
    CHECK(curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200);
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/nested/redirect");
    h = prepare(url, &result);
    CHECK(h);
    CHECK(curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "OK")); /* relative Location preserves the port */
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/binary");
    h = prepare(url, &result);
    CHECK(h);
    CHECK(curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, 4L) == CURLE_OK);
    CHECK(curl_easy_setopt(h, CURLOPT_POSTFIELDS, binary) == CURLE_OK);
    duplicate = curl_easy_duphandle(h);
    CHECK(duplicate);
    CHECK(curl_easy_perform(duplicate) == CURLE_OK);
    CHECK(!strcmp(result.data, "BINARY-OK"));
    curl_easy_cleanup(duplicate); /* must NOT free the caller's stack buffer */
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/sized");
    h = prepare(url, &result);
    CHECK(h);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(h, CURLOPT_READDATA, &stream);
    curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)stream.length);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "SIZED-OK"));
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    stream.offset = 0;
    make_url(url, sizeof(url), port, "/chunked");
    h = prepare(url, &result);
    CHECK(h);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(h, CURLOPT_READDATA, &stream);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "CHUNKED-OK"));
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/auth-redirect");
    h = prepare(url, &result);
    CHECK(h);
    curl_easy_setopt(h, CURLOPT_USERNAME, "tester");
    curl_easy_setopt(h, CURLOPT_PASSWORD, "secret");
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "NO-LEAK"));
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/userpwd");
    h = prepare(url, &result);
    CHECK(h);
    curl_easy_setopt(h, CURLOPT_USERPWD, "tester:secret");
    curl_easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    CHECK(curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "USERPWD-OK"));
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/informational");
    h = prepare(url, &result);
    CHECK(h && curl_easy_perform(h) == CURLE_OK);
    CHECK(!strcmp(result.data, "OK") && result.status_lines == 1);
    curl_easy_cleanup(h);

    memset(&result, 0, sizeof(result));
    make_url(url, sizeof(url), port, "/ok");
    h = prepare(url, &result);
    CHECK(h);
    CHECK(curl_easy_setopt(h, CURLOPT_PINNEDPUBLICKEY, "sha256//test") == CURLE_NOT_BUILT_IN);
    multi = curl_multi_init();
    CHECK(multi && curl_multi_add_handle(multi, h) == CURLM_OK);
    CHECK(curl_multi_perform(multi, &running) == CURLM_OK && running == 0);
    msg = curl_multi_info_read(multi, &queued);
    CHECK(msg && msg->data.result == CURLE_OK);
    CHECK(curl_multi_info_read(multi, &queued) == NULL && queued == 0);
    CHECK(!strcmp(result.data, "OK"));
    curl_multi_remove_handle(multi, h);
    curl_multi_cleanup(multi);
    curl_easy_cleanup(h);

    printf("PASS: curl-luna host HTTP framing/binary/multi/security regression tests\n");
    return 0;
}
