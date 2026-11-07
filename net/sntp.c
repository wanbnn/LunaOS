#include "../include/net/sntp.h"
#include "datetime.h"
#include "luna.h"
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
void sntp_request(const uint8_t token[8], uint8_t out[48]) {
    memset(out, 0, 48);
    out[0] = 0x23;
    out[2] = 12;
    out[3] = (uint8_t)-20;
    memcpy(out + 40, token, 8);
}
static uint64_t stamp_ms(const uint8_t *p) {
    uint64_t sec = get32(p);
    /* Bootstrap window 2000..2099: select era 0/1 across the 2036
     * rollover. This window is shorter than a full 136-year NTP era. */
    if (sec < UINT64_C(3155673600))
        sec += UINT64_C(0x100000000);
    return (sec - UINT64_C(2208988800)) * 1000 + ((uint64_t)get32(p + 4) * 1000 >> 32);
}
int sntp_response(const uint8_t *p, size_t n, const uint8_t token[8], uint32_t rtt, uint64_t *utc) {
    if (!p || !token || !utc || n < 48 || n > 512 || n % 4 || rtt > 5000)
        return -22;
    unsigned version = (p[0] >> 3) & 7;
    if ((p[0] & 7) != 4 || (version != 3 && version != 4) || (p[0] >> 6) == 3 ||
        memcmp(p + 24, token, 8))
        return -13;
    if (!p[1])
        return -11; /* Kiss-o'-Death: caller backs off, never sets the clock. */
    if (p[1] > 15)
        return -13;
    uint8_t zeros[8] = {0};
    if (!memcmp(p + 32, zeros, 8) || !memcmp(p + 40, zeros, 8))
        return -13;
    uint64_t receive = stamp_ms(p + 32), send = stamp_ms(p + 40);
    if (send < UINT64_C(946684800000) || send < receive || send - receive > rtt + 1000u ||
        send > UINT64_C(4102444799999))
        return -13;
    /* Reject grossly unsynchronized servers (16.16 fixed-point root distance). */
    if (get32(p + 8) > 16u * 65536u || (int32_t)get32(p + 4) > 16 * 65536 ||
        (int32_t)get32(p + 4) < -16 * 65536)
        return -13;
    uint64_t flight = rtt > send - receive ? rtt - (send - receive) : 0;
    *utc = send + flight / 2;
    if (*utc > UINT64_C(4102444799999))
        return -13;
    return 0;
}
