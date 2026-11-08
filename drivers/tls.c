/* BearSSL TLS 1.2 client, verified chains/hostnames/time. No insecure bypass. */
#include "bearssl.h"
#include "trust_anchors.h"
#include "web.h"
static br_ssl_client_context client;
static br_x509_minimal_context verifier;
static br_sslio_context channel;
static unsigned char io_buffer[BR_SSL_BUFSIZE_BIDI];
static br_x509_trust_anchor roots[TAs_NUM + 1];
static br_x509_decoder_context custom_decoder;
static unsigned char custom_der[8192], custom_dn[4096];
static size_t der_used, dn_used;
static bool decode_overflow;
static void pem_output(void *ctx, const void *data, size_t n) {
    (void)ctx;
    if (n > sizeof(custom_der) - der_used) {
        decode_overflow = true;
        return;
    }
    memcpy(custom_der + der_used, data, n);
    der_used += n;
}
static void dn_output(void *ctx, const void *data, size_t n) {
    (void)ctx;
    if (n > sizeof(custom_dn) - dn_used) {
        decode_overflow = true;
        return;
    }
    memcpy(custom_dn + dn_used, data, n);
    dn_used += n;
}
static size_t trust_roots(void) {
    memcpy(roots, TAs, sizeof(TAs));
    File *f = fs_find("/etc/tls/custom.pem");
    if (!f || f->dir || f->size > 65536 || !fs_materialize(f))
        return TAs_NUM;
    br_pem_decoder_context pem;
    br_pem_decoder_init(&pem);
    br_pem_decoder_setdest(&pem, pem_output, 0);
    der_used = dn_used = 0;
    decode_overflow = false;
    const char *p = f->data;
    size_t remaining = f->size;
    bool complete = false;
    while (remaining) {
        size_t consumed = br_pem_decoder_push(&pem, p, remaining);
        p += consumed;
        remaining -= consumed;
        int event = br_pem_decoder_event(&pem);
        if (event == BR_PEM_ERROR)
            return TAs_NUM;
        if (event == BR_PEM_END_OBJ) {
            complete = true;
            break;
        }
        if (!consumed && !event)
            break;
    }
    if (!complete || decode_overflow || !der_used)
        return TAs_NUM;
    br_x509_decoder_init(&custom_decoder, dn_output, 0);
    br_x509_decoder_push(&custom_decoder, custom_der, der_used);
    br_x509_pkey *key = br_x509_decoder_get_pkey(&custom_decoder);
    if (!key || decode_overflow || !br_x509_decoder_isCA(&custom_decoder))
        return TAs_NUM;
    roots[TAs_NUM].dn.data = custom_dn;
    roots[TAs_NUM].dn.len = dn_used;
    roots[TAs_NUM].flags = BR_X509_TA_CA;
    roots[TAs_NUM].pkey = *key;
    return TAs_NUM + 1;
}
static int read_transport(void *ctx, unsigned char *data, size_t len) {
    (void)ctx;
    int n = stream_read(data, len);
    return n > 0 ? n : -1;
}
static int write_transport(void *ctx, const unsigned char *data, size_t len) {
    (void)ctx;
    return stream_write(data, len);
}
bool tls_connect(const char *host, char *error, size_t cap) {
    unsigned char entropy[32];
    if (!random_bytes(entropy, sizeof(entropy))) {
        append(error, "TLS requires a working hardware random source (RDRAND).", cap);
        return false;
    }
    size_t count = trust_roots();
    br_ssl_client_init_full(&client, &verifier, roots, count);
    br_ssl_engine_set_versions(&client.eng, BR_TLS12, BR_TLS12);
    static const uint16_t suites[] = {BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                                      BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
                                      BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                                      BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                                      BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
                                      BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384};
    br_ssl_engine_set_suites(&client.eng, suites, sizeof(suites) / sizeof(suites[0]));
    uint32_t epoch = rtc_epoch_seconds();
    br_x509_minimal_set_time(&verifier, epoch / 86400 + 719528, epoch % 86400);
    br_x509_minimal_set_minrsa(&verifier, 256);
    br_ssl_engine_set_buffer(&client.eng, io_buffer, sizeof(io_buffer), 1);
    br_ssl_engine_inject_entropy(&client.eng, entropy, sizeof(entropy));
    memset(entropy, 0, sizeof(entropy));
    if (!br_ssl_client_reset(&client, host, 0)) {
        append(error, "TLS client initialization failed.", cap);
        return false;
    }
    br_sslio_init(&channel, &client.eng, read_transport, 0, write_transport, 0);
    return true;
}
int tls_read(void *data, unsigned n) {
    return br_sslio_read(&channel, data, n);
}
int tls_write(const void *data, unsigned n) {
    if (br_sslio_write_all(&channel, data, n) < 0 || br_sslio_flush(&channel) < 0)
        return -1;
    return n;
}
void tls_close(void) { /* stream_close releases a possibly failed or cancelled connection. */
    stream_close();
}
unsigned tls_error(void) {
    return br_ssl_engine_last_error(&client.eng);
}
