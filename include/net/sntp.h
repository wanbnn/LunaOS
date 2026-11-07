#ifndef LUNA_SNTP_H
#define LUNA_SNTP_H
#include <stddef.h>
#include <stdint.h>
#define SNTP_PACKET_BYTES 48
void sntp_request(const uint8_t token[8], uint8_t out[48]);
/* Unauthenticated unicast SNTPv4. Validate identity/stratum/origin, reject KoD
 * and alarm clocks, unfold the NTP era in the supported network-time bootstrap window 2000..2099.
 */
int sntp_response(const uint8_t *, size_t, const uint8_t token[8], uint32_t roundtrip_ms,
                  uint64_t *utc_ms);
#endif
