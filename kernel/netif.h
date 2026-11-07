#ifndef LUNA_NETIF_H
#define LUNA_NETIF_H
#include "../include/net/dhcp.h"
/* Ethernet-frame interface: radios supply decrypted/replay-checked MSDUs only
 * after authentication. Receive buffers are owned by the caller. */
typedef struct {
    bool (*link_up)(void *);
    int (*transmit)(void *, const uint8_t *, unsigned);
    int (*receive)(void *, uint8_t *, unsigned, unsigned *); /* 1, 0, negative */
} NetDeviceOps;
typedef struct {
    const NetDeviceOps *ops;
    void *context;
    uint8_t mac[6];
    DhcpClient dhcp;
    bool started;
    int error;
} NetInterface;
int netif_start(NetInterface *, const NetDeviceOps *, void *, const uint8_t mac[6],
                  uint32_t xid, uint32_t now);
void netif_stop(NetInterface *);
void netif_poll(NetInterface *, uint32_t now, void (*input)(void *,uint8_t *,unsigned), void *);
int netif_transmit(NetInterface *, const uint8_t *, unsigned);
/* Current IP stack owns one selected interface. RTL8139 remains preferred;
 * attach returns EBUSY if an existing transport or network task owns it. */
int net_attach_driver(const NetDeviceOps *, void *, const uint8_t mac[6], uint32_t xid);
void net_detach_driver(const NetDeviceOps *, void *);
bool net_driver_ipv4(const NetDeviceOps *, void *, NetIpv4Config *);
#endif
