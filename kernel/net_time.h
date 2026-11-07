#ifndef LUNA_NET_TIME_H
#define LUNA_NET_TIME_H
#include <stddef.h>
#include <stdint.h>
/* Nonblocking dedicated UDP/DNS transaction, independent of foreground HTTP.
 * Responses are accepted only from the selected address/port and local port. */
int net_time_begin(const char *host, uint16_t port, const uint8_t request[48]);
int net_time_result(uint8_t *out, unsigned cap, unsigned *bytes, uint32_t *rtt_ms);
void net_time_cancel(void);
#endif
