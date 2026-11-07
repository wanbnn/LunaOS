#ifndef LUNA_DHCP_H
#define LUNA_DHCP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint8_t address[4], mask[4], gateway[4], dns[4]; } NetIpv4Config;
typedef enum { DHCP_OFF, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND,
               DHCP_RENEWING, DHCP_REBINDING, DHCP_FAILED } DhcpPhase;
typedef struct {
    void *context;
    /* Send BOOTP payload, UDP 68 -> 67. dest_mac NULL means broadcast. */
    int (*send)(void *, const uint8_t source[4], const uint8_t dest[4],
                 const uint8_t *dest_mac, const uint8_t *, size_t);
} DhcpOps;
typedef struct {
    DhcpOps ops;
    DhcpPhase phase;
    NetIpv4Config config;
    uint8_t mac[6], offered[4], server[4], server_mac[6];
    uint32_t xid, deadline, lease_start, lease_ms, t1_ms, t2_ms;
    unsigned attempts;
    int error;
    bool configured;
} DhcpClient;
int dhcp_start(DhcpClient *, const DhcpOps *, const uint8_t mac[6], uint32_t xid, uint32_t now);
int dhcp_receive(DhcpClient *, const uint8_t source_mac[6], const uint8_t *, size_t, uint32_t now);
void dhcp_poll(DhcpClient *, uint32_t now);
void dhcp_stop(DhcpClient *);
#endif
