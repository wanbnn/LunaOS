/* Native RTL8139 + Ethernet/ARP/IPv4/ICMP/UDP DNS/TCP HTTP.
 * One synchronous TCP connection, static QEMU user-network profile. */
#include "luna.h"
#include "heap.h"
#include "task.h"
#include "web.h"
#include "netif.h"
#include "wifi.h"
#include "net_time.h"
#include "../include/net/time_dns.h"
bool net_ready;
static uint16_t io;
static uint8_t mac[6], gateway_mac[6];
static bool gateway_known;
static NetInterface external_nic;
static uint8_t netmask[4]={255,255,255,0};
static uint8_t peer_ip[4], peer_mac[6], pending_hop[4], pending_frame[1514];
static unsigned pending_bytes;
static bool peer_known;
static uint8_t local_ip[4] = {10, 0, 2, 15}, gateway[4] = {10, 0, 2, 2},
                     dns_ip[4] = {10, 0, 2, 3};
static uint8_t manual_dns[4];
static bool dns_manual;
static void apply_dns(const uint8_t automatic[4]);
static uint8_t rx[65536 + 2048] __attribute__((aligned(256))),
    tx[4][1536] __attribute__((aligned(256)));
static int txslot;
static uint32_t rxpos, rxcount, txcount;
static uint16_t ipid, dnsid = 0x1234;
static bool ping_done, dns_done;
static const char *connect_error = "No connection attempted.";
static char failed_http_url[256],failed_http_error[192];
static char final_http_url[4096];
static uint32_t arp_requests, arp_received, dns_requests, dns_answers, tcp_syns;
/* The task that currently owns the network service; nobody else may poll it. */
static uint32_t network_owner;
static uint8_t resolved[4];
static uint32_t resolved_ttl;
static struct {
    char host[201];
    uint8_t address[4], dns[4], local[4];
    uint32_t expires, hits;
    bool valid;
} dns_cache;
static void apply_dns(const uint8_t automatic[4]) {
    const uint8_t *selected=dns_manual ? manual_dns : automatic;
    if(memcmp(dns_ip,selected,4)) dns_cache.valid=false;
    memcpy(dns_ip,selected,4);
}
static struct {
    unsigned phase; /* 0=idle, 1=resolve, 2=send request, 3=wait response, 4=done */
    uint8_t address[4],request[48],response[512];
    char host[128];
    uint16_t port,local_port,id;
    uint32_t deadline,sent,rtt;
    unsigned bytes;
    int error;
} time_query;
static void time_network_poll(void);
static void time_network_input(const uint8_t [4],const uint8_t *,unsigned);
#define TCP_MSS 1200u
#define TCP_REORDER 4u
static const char *transport_error="No transfer attempted.";
static uint32_t tcp_gaps,tcp_duplicates,tcp_queue_drops,tcp_resets,tcp_timeouts;
static struct {
    int state;
    uint8_t ip[4];
    uint16_t port, remote_port;
    uint32_t seq, ack, remote_ack;
    char data[65536];
    unsigned used;
    unsigned peer_mss;
    struct {
        bool valid, fin;
        uint32_t seq;
        unsigned bytes;
        uint8_t data[1460];
    } pending[TCP_REORDER];
} tcp;
static uint16_t be16(const void *p) {
    const uint8_t *b = p;
    return ((uint16_t)b[0] << 8) | b[1];
}
static uint32_t be32(const void *p) {
    const uint8_t *b = p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static void put16(void *p, uint16_t v) {
    uint8_t *b = p;
    b[0] = v >> 8;
    b[1] = v;
}
static void put32(void *p, uint32_t v) {
    uint8_t *b = p;
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}
static bool equal(const uint8_t *a, const uint8_t *b, int n) {
    while (n--)
        if (*a++ != *b++)
            return false;
    return true;
}
static uint16_t sum(const uint8_t *b, unsigned n, uint32_t s) {
    while (n > 1) {
        s += be16(b);
        b += 2;
        n -= 2;
    }
    if (n)
        s += *b << 8;
    while (s >> 16)
        s = (s & 65535) + (s >> 16);
    return ~s;
}
static bool send_frame(const uint8_t *p, unsigned n) {
    if (!net_ready || n > 1514)
        return false;
    if (external_nic.started) {
        int rc=netif_transmit(&external_nic,p,n);
        if (!rc) txcount++;
        return !rc;
    }
    int slot = txslot++ & 3;
    uint32_t start = ticks;
    while (!(inl(io + 0x10 + slot * 4) & (1 << 13)))
        if (ticks - start > 100)
            return false;
    memcpy(tx[slot], p, n);
    if (n < 60) {
        memset(tx[slot] + n, 0, 60 - n);
        n = 60;
    }
    outl(io + 0x20 + slot * 4, (uint32_t)tx[slot]);
    outl(io + 0x10 + slot * 4, n);
    txcount++;
    return true;
}
static void arp_request_to(const uint8_t target[4]) {
    arp_requests++;
    uint8_t p[60];
    memset(p, 0, sizeof(p));
    memset(p, 255, 6);
    memcpy(p + 6, mac, 6);
    put16(p + 12, 0x806);
    put16(p + 14, 1);
    put16(p + 16, 0x800);
    p[18] = 6;
    p[19] = 4;
    put16(p + 20, 1);
    memcpy(p + 22, mac, 6);
    memcpy(p + 28, local_ip, 4);
    memcpy(p + 38, target, 4);
    send_frame(p, 42);
}
static void arp_request(void) { arp_request_to(gateway); }
static bool ensure_gateway(void) {
    if (gateway_known)
        return true;
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_request();
        uint32_t start = ticks;
        while (ticks - start < 100) {
            net_poll();
            if (gateway_known)
                return true;
            task_yield();
        }
    }
    return false;
}
static void send_ip(const uint8_t *dest, int proto, const uint8_t *payload, unsigned n) {
    if (n > 1480 || !net_ready) return;
    bool on_link=true;
    for (unsigned i=0;i<4;i++)
        if ((dest[i]&netmask[i])!=(local_ip[i]&netmask[i])) on_link=false;
    const uint8_t *hop=on_link ? dest : gateway;
    const uint8_t *destination=0;
    if (equal(hop,gateway,4) && gateway_known) destination=gateway_mac;
    else if (peer_known && equal(hop,peer_ip,4)) destination=peer_mac;
    uint8_t p[1514];
    if (destination) memcpy(p,destination,6); else memset(p,0,6);
    memcpy(p + 6, mac, 6);
    put16(p + 12, 0x800);
    uint8_t *h = p + 14;
    memset(h, 0, 20);
    h[0] = 0x45;
    put16(h + 2, n + 20);
    put16(h + 4, ++ipid);
    put16(h + 6, 0x4000);
    h[8] = 64;
    h[9] = proto;
    memcpy(h + 12, local_ip, 4);
    memcpy(h + 16, dest, 4);
    put16(h + 10, sum(h, 20, 0));
    memcpy(h + 20, payload, n);
    if (destination) send_frame(p,n+34);
    else {
        /* Bounded single pending packet. ARP replies release it; protocol
         * callers own retries if a peer never responds. */
        memcpy(pending_frame,p,n+34); pending_bytes=n+34;
        memcpy(pending_hop,hop,4); arp_request_to(hop);
    }
}
static void send_tcp(int flags, const char *data, unsigned n) {
    uint8_t p[1460];
    unsigned header=(flags&2) ? 24 : 20;
    memset(p, 0, header);
    put16(p, tcp.port);
    put16(p + 2, tcp.remote_port);
    put32(p + 4, tcp.seq);
    put32(p + 8, tcp.ack);
    p[12] = (header/4)<<4;
    p[13] = flags;
    put16(p + 14, sizeof(tcp.data) - tcp.used - 1);
    if(flags&2) { p[20]=2;p[21]=4;put16(p+22,TCP_MSS); }
    if (n)
        memcpy(p + header, data, n);
    uint32_t s = be16(local_ip) + be16(local_ip + 2) + be16(tcp.ip) + be16(tcp.ip + 2) + 6 + header + n;
    put16(p + 16, sum(p, header + n, s));
    send_ip(tcp.ip, 6, p, header + n);
}
/* Consume only the contiguous suffix of a segment, trimming retransmitted
 * prefixes. Comparisons use signed differences to survive sequence wrap. */
static bool tcp_contiguous(uint32_t seq,const uint8_t *data,unsigned bytes,bool fin) {
    if(tcp.state!=2) return true;
    if((int32_t)(seq-tcp.ack)>0) return false;
    uint32_t end=seq+bytes;
    unsigned skip=tcp.ack-seq;
    unsigned fresh=skip<bytes ? bytes-skip : 0;
    if(fresh>sizeof(tcp.data)-tcp.used-1) return false;
    if(fresh) {
        memcpy(tcp.data+tcp.used,data+skip,fresh);
        tcp.used+=fresh;tcp.ack+=fresh;tcp.data[tcp.used]=0;
    } else if(bytes) tcp_duplicates++;
    if(fin && end==tcp.ack) {
        tcp.ack++;send_tcp(0x11,NULL,0);tcp.seq++;tcp.state=3;
    }
    return true;
}
static void tcp_drain_pending(void) {
    for(unsigned pass=0;pass<TCP_REORDER;pass++) {
        bool progress=false;
        for(unsigned i=0;i<TCP_REORDER;i++) {
            if(!tcp.pending[i].valid) continue;
            if(tcp_contiguous(tcp.pending[i].seq,tcp.pending[i].data,
                              tcp.pending[i].bytes,tcp.pending[i].fin)) {
                tcp.pending[i].valid=false;progress=true;
            }
        }
        if(!progress) break;
    }
}
static void tcp_payload(uint32_t seq,const uint8_t *data,unsigned bytes,bool fin) {
    if(tcp_contiguous(seq,data,bytes,fin)) {
        tcp_drain_pending();
    } else if((int32_t)(seq-tcp.ack)>0) {
        tcp_gaps++;
        unsigned gap=seq-tcp.ack,window=sizeof(tcp.data)-tcp.used-1;
        if(bytes<=1460 && gap<window && bytes<=window-gap) {
            for(unsigned i=0;i<TCP_REORDER;i++) {
                if(tcp.pending[i].valid) {
                    if(tcp.pending[i].seq==seq && tcp.pending[i].bytes==bytes &&
                       tcp.pending[i].fin==fin) { tcp_duplicates++;send_tcp(0x10,NULL,0);return; }
                    continue;
                }
                tcp.pending[i].seq=seq;tcp.pending[i].bytes=bytes;tcp.pending[i].fin=fin;
                if(bytes) memcpy(tcp.pending[i].data,data,bytes);
                tcp.pending[i].valid=true;send_tcp(0x10,NULL,0);return;
            }
        }
        tcp_queue_drops++;
    }
    /* Cumulative ACK also advertises the gap when the bounded queue fills. */
    if(bytes || fin) send_tcp(0x10,NULL,0);
}
static int skip_name(const uint8_t *p, int n, int pos) {
    for (int k = 0; k < 128 && pos < n; k++) {
        int c = p[pos++];
        if (!c)
            return pos;
        if ((c & 0xc0) == 0xc0)
            return pos < n ? pos + 1 : -1;
        if (c > 63 || pos + c > n)
            return -1;
        pos += c;
    }
    return -1;
}
static void dns_receive(const uint8_t *p, int n) {
    if (n < 12 || be16(p) != dnsid || !(p[2] & 0x80) || (p[3] & 15))
        return;
    int pos = 12;
    int q = be16(p + 4), a = be16(p + 6);
    if (q > 8 || a > 64)
        return;
    while (q--) {
        pos = skip_name(p, n, pos);
        if (pos < 0 || pos + 4 > n)
            return;
        pos += 4;
    }
    uint32_t ttl_limit=UINT32_MAX;
    while (a--) {
        pos = skip_name(p, n, pos);
        if (pos < 0 || pos + 10 > n)
            return;
        int type = be16(p + pos), cls = be16(p + pos + 2), len = be16(p + pos + 8);
        pos += 10;
        if (pos + len > n)
            return;
        uint32_t ttl=be32(p+pos-6);
        if(cls==1 && type==5 && ttl<ttl_limit) ttl_limit=ttl;
        if (type == 1 && cls == 1 && len == 4) {
            memcpy(resolved, p + pos, 4);
            resolved_ttl=ttl<ttl_limit ? ttl : ttl_limit;
            dns_done = true;
            return;
        }
        pos += len;
    }
}
static void receive(uint8_t *p, unsigned n) {
    if (n < 14)
        return;
    int type = be16(p + 12);
    if (type == 0x806 && n >= 42) {
        uint8_t *a = p + 14;
        if (be16(a) != 1 || be16(a + 2) != 0x800 || a[4] != 6 || a[5] != 4)
            return;
        unsigned opcode=be16(a+6);
        if ((opcode!=1 && opcode!=2) || (a[8]&1) || !equal(a+8,p+6,6)) return;
        arp_received++;
        if (equal(a + 14, gateway, 4)) {
            memcpy(gateway_mac, a + 8, 6);
            gateway_known = true;
        }
        if (equal(a+24,local_ip,4)) {
            memcpy(peer_ip,a+14,4); memcpy(peer_mac,a+8,6); peer_known=true;
            if (pending_bytes && equal(a+14,pending_hop,4)) {
                unsigned bytes=pending_bytes; pending_bytes=0;
                memcpy(pending_frame,a+8,6); send_frame(pending_frame,bytes);
            }
        }
        if (be16(a + 6) == 1 && equal(a + 24, local_ip, 4)) {
            uint8_t reply[42];
            memcpy(reply, p, 42);
            memcpy(reply, p + 6, 6);
            memcpy(reply + 6, mac, 6);
            put16(reply + 20, 2);
            memcpy(reply + 32, p + 22, 6);
            memcpy(reply + 38, p + 28, 4);
            memcpy(reply + 22, mac, 6);
            memcpy(reply + 28, local_ip, 4);
            send_frame(reply, 42);
        }
        return;
    }
    if (type != 0x800 || n < 34)
        return;
    uint8_t *h = p + 14;
    unsigned ih = (h[0] & 15) * 4, total = be16(h + 2);
    if ((h[0] >> 4) != 4 || ih < 20 || total < ih || total > n - 14 || sum(h, ih, 0) ||
        !equal(h + 16, local_ip, 4) || (be16(h + 6) & 0x3fff))
        return;
    uint8_t *d = h + ih;
    unsigned len = total - ih;
    if (h[9] == 1 && len >= 8 && !sum(d, len, 0) && d[0] == 0 && be16(d + 4) == 0x4c55)
        ping_done = true;
    if (h[9] == 1 && len >= 8 && !sum(d, len, 0) && d[0] == 8) {
        d[0] = 0;
        d[2] = d[3] = 0;
        put16(d + 2, sum(d, len, 0));
        send_ip(h + 12, 1, d, len);
    }
    if (h[9]==17 && len>=8 && time_query.phase &&
        (be16(d+2)==49152 || be16(d+2)==time_query.local_port)) {
        unsigned bytes=be16(d+4);
        uint32_t pseudo=be16(h+12)+be16(h+14)+be16(h+16)+be16(h+18)+17+bytes;
        if (bytes>=8 && bytes==len && (!be16(d+6) || !sum(d,bytes,pseudo)))
            time_network_input(h+12,d,bytes);
        return;
    }
    if (h[9] == 17 && len >= 8 && be16(d) == 53 && be16(d + 2) == 53000 &&
        equal(h + 12, dns_ip, 4)) {
        unsigned u = be16(d + 4);
        if (u >= 8 && u <= len) {
            dns_answers++;
            dns_receive(d + 8, u - 8);
        }
    }
    if (h[9] != 6 || len < 20 || !tcp.state || !equal(h + 12, tcp.ip, 4) ||
        be16(d) != tcp.remote_port || be16(d + 2) != tcp.port)
        return;
    unsigned th = (d[12] >> 4) * 4;
    if (th < 20 || th > len)
        return;
    uint32_t pseudo = be16(h + 12) + be16(h + 14) + be16(h + 16) + be16(h + 18) + 6 + len;
    if (sum(d, len, pseudo))
        return;
    uint8_t flags = d[13];
    uint32_t seq = be32(d + 4), ack=be32(d+8);
    if (flags & 4) {
        if(tcp.state==1 ? (!(flags&0x10) || ack!=tcp.seq+1) : seq!=tcp.ack) {
            if(tcp.state>=2) send_tcp(0x10,NULL,0);
            return;
        }
        tcp_resets++;transport_error="TCP reset by peer.";
        tcp.state = -1;
        return;
    }
    if (tcp.state == 1 && (flags & 0x12) == 0x12 && ack == tcp.seq + 1) {
        tcp.peer_mss=536; /* IPv4 default when the peer omits MSS. */
        for(unsigned i=20;i<th;) {
            unsigned kind=d[i++];if(!kind) break;if(kind==1) continue;
            if(i>=th || d[i]<2 || d[i]>th-i+1) return;
            unsigned count=d[i++];
            if(kind==2 && count==4) { unsigned mss=be16(d+i);if(!mss) return;tcp.peer_mss=mss; }
            i+=count-2;
        }
        tcp.seq++;
        tcp.remote_ack=ack;
        tcp.ack = seq + 1;
        tcp.state = 2;
        send_tcp(0x10, 0, 0);
        return;
    }
    if (tcp.state < 2)
        return;
    if(!(flags&0x10) || (int32_t)(ack-tcp.seq)>0) {send_tcp(0x10,NULL,0);return;}
    if((int32_t)(ack-tcp.remote_ack)>0) tcp.remote_ack=ack;
    unsigned bytes = len - th;
    tcp_payload(seq,d+th,bytes,(flags&1)!=0);
}
static void external_input(void *ctx, uint8_t *p, unsigned n) {
    (void)ctx;
    if (!external_nic.dhcp.configured) return;
    const NetIpv4Config *cfg=&external_nic.dhcp.config;
    if (!net_ready || memcmp(local_ip,cfg->address,4) || memcmp(gateway,cfg->gateway,4)) {
        gateway_known=peer_known=false;
        dns_cache.valid=false;
    }
    memcpy(local_ip,cfg->address,4); memcpy(netmask,cfg->mask,4);
    memcpy(gateway,cfg->gateway,4); apply_dns(cfg->dns);
    net_ready=true;
    receive(p,n); rxcount++;
}
int net_attach_driver(const NetDeviceOps *ops, void *ctx, const uint8_t address[6], uint32_t xid) {
    if (io || external_nic.started || network_owner) return -16;
    int rc=netif_start(&external_nic,ops,ctx,address,xid,frame_clock);
    if (rc) return rc;
    dns_cache.valid=false;
    memcpy(mac,address,6); gateway_known=peer_known=false; pending_bytes=0; net_ready=false;
    memset(&tcp,0,sizeof(tcp)); return 0;
}
void net_detach_driver(const NetDeviceOps *ops, void *ctx) {
    if (!external_nic.started || external_nic.ops!=ops || external_nic.context!=ctx) return;
    netif_stop(&external_nic); net_ready=false; gateway_known=peer_known=false; pending_bytes=0;
    dns_cache.valid=false;
    memset(&tcp,0,sizeof(tcp)); memset(mac,0,sizeof(mac));
    memset(local_ip,0,4); memset(gateway,0,4); memset(dns_ip,0,4);
}
bool net_driver_ipv4(const NetDeviceOps *ops, void *ctx, NetIpv4Config *out) {
    if (!external_nic.started || external_nic.ops!=ops || external_nic.context!=ctx ||
        !external_nic.dhcp.configured || !external_nic.ops->link_up(ctx)) return false;
    if (out) *out=external_nic.dhcp.config;
    return true;
}
void net_poll(void) {
    /* Synchronous DNS/TCP/ICMP callers may occupy the GUI loop that normally
     * services the radio. Drain/decrypt Wi-Fi RX before consuming its IP queue. */
    wifi_poll();
    if (external_nic.started) {
        netif_poll(&external_nic,frame_clock,external_input,0);
        if (external_nic.dhcp.configured) {
            const NetIpv4Config *cfg=&external_nic.dhcp.config;
            if (!net_ready || memcmp(local_ip,cfg->address,4) || memcmp(gateway,cfg->gateway,4)) {
                gateway_known=peer_known=false;
                dns_cache.valid=false;
            }
            memcpy(local_ip,cfg->address,4); memcpy(netmask,cfg->mask,4);
            memcpy(gateway,cfg->gateway,4); apply_dns(cfg->dns);
            net_ready=true;
        } else { net_ready=false; gateway_known=peer_known=false; pending_bytes=0; tcp.state=0; dns_cache.valid=false; }
        time_network_poll();
        return;
    }
    if (!net_ready) {
        time_network_poll();
        return;
    }
    for (int k = 0; k < 32 && !(inb(io + 0x37) & 1); k++) {
        uint8_t *h = rx + rxpos;
        uint16_t status = *(uint16_t *)h, len = *(uint16_t *)(h + 2);
        if (len < 4 || len > 1522) {
            outb(io + 0x37, 0);
            outl(io + 0x30, (uint32_t)rx);
            rxpos = 0;
            outw(io + 0x38, 0xfff0);
            outb(io + 0x37, 0x0c);
            break;
        }
        if (status & 1) {
            uint8_t packet[1536];
            memcpy(packet, h + 4, len - 4);
            receive(packet, len - 4);
            rxcount++;
        }
        rxpos = (rxpos + len + 4 + 3) & ~3u;
        rxpos %= 65536;
        outw(io + 0x38, rxpos - 16);
    }
    outw(io + 0x3e, 0xffff);
    time_network_poll();
}
void net_init(void) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++)
                if (pci_read(b, d, f, 0) == 0x813910ec) {
                    uint32_t bar = pci_read(b, d, f, 0x10);
                    if (!(bar & 1))
                        continue;
                    io = bar & ~3u;
                    uint32_t c = pci_read(b, d, f, 4);
                    pci_write(b, d, f, 4, (c & 65535) | 5);
                    outb(io + 0x52, 0);
                    outb(io + 0x37, 0x10);
                    for (int k = 0; k < 1000000 && (inb(io + 0x37) & 0x10); k++)
                        ;
                    if (inb(io + 0x37) & 0x10)
                        return;
                    for (int k = 0; k < 6; k++)
                        mac[k] = inb(io + k);
                    outl(io + 0x30, (uint32_t)rx);
                    outw(io + 0x3c, 0);
                    outl(io + 0x44, 0x188a);
                    outl(io + 0x40, 0x03000700);
                    outw(io + 0x38, 0xfff0);
                    outb(io + 0x37, 0x0c);
                    net_ready = true;
                    serial_write("RTL8139: initialized, static 10.0.2.15/24\n");
                    return;
                }
}
void net_status(char *out, size_t cap) {
    out[0] = 0;
    if (external_nic.started) {
        append(out, net_ready ? "Wi-Fi/generic interface: IPv4 ready (DHCP)\n" :
                              "Wi-Fi/generic interface: waiting for DHCP\n", cap);
        const uint8_t *ips[]={local_ip,gateway,dns_ip};
        const char *labels[]={"IPv4: ","Gateway: ","DNS: "};
        for (unsigned i=0; i<3 && net_ready; i++) {
            append(out,labels[i],cap);
            for (unsigned j=0;j<4;j++) {
                char value[16]; number(value,ips[i][j]); append(out,value,cap);
                append(out,j==3 ? "\n" : ".",cap);
            }
        }
    } else append(out,
           net_ready ? "Ethernet: RTL8139 ready\nIPv4: 10.0.2.15/24 (static)\nGateway: 10.0.2.2\n"
                     : "Ethernet: no supported RTL8139 found\n",
           cap);
    if (net_ready) {
        append(out,dns_manual ? "DNS mode: manual\n" : "DNS mode: automatic (DHCP)\n",cap);
        if(!external_nic.started) {
            append(out,"DNS: ",cap);
            for(unsigned i=0;i<4;i++) {
                char value[16]; number(value,dns_ip[i]); append(out,value,cap);
                append(out,i==3 ? "\n" : ".",cap);
            }
        }
        append(out, gateway_known ? "Gateway ARP: resolved\n" : "Gateway ARP: unresolved\n", cap);
        const uint32_t values[]={arp_requests,arp_received,dns_requests,dns_answers,tcp_syns};
        const char *names[]={"ARP requests: ","ARP received: ","DNS requests: ","DNS responses: ","TCP SYN attempts: "};
        for(unsigned i=0;i<5;i++) {
            char value[16]; number(value,values[i]);
            append(out,names[i],cap); append(out,value,cap); append(out,"\n",cap);
        }
        append(out,"Last connection: ",cap); append(out,connect_error,cap); append(out,"\n",cap);
        char hits[16]; number(hits,dns_cache.hits);
        append(out,"DNS cache hits: ",cap); append(out,hits,cap); append(out,"\n",cap);
        append(out,"Last transport: ",cap);append(out,transport_error,cap);append(out,"\n",cap);
        const uint32_t counts[]={tcp_gaps,tcp_duplicates,tcp_queue_drops,tcp_resets,tcp_timeouts};
        const char *fields[]={"TCP gaps: ","TCP duplicates: ","TCP reorder drops: ","TCP resets: ","TCP timeouts: "};
        for(unsigned i=0;i<5;i++) {
            char value[16];number(value,counts[i]);append(out,fields[i],cap);
            append(out,value,cap);append(out,"\n",cap);
        }
        if(failed_http_url[0]) {
            append(out,"Last HTTP failure: ",cap);append(out,failed_http_url,cap);append(out,"\n",cap);
            append(out,failed_http_error,cap);append(out,"\n",cap);
        }
        append(out, "MAC: ", cap);
        for (int i = 0; i < 6; i++) {
            char b[4] = {"0123456789abcdef"[mac[i] >> 4], "0123456789abcdef"[mac[i] & 15],
                         i == 5 ? '\n' : ':', 0};
            append(out, b, cap);
        }
        char b[16];
        append(out, "RX ", cap);
        number(b, rxcount);
        append(out, b, cap);
        append(out, " | TX ", cap);
        number(b, txcount);
        append(out, b, cap);
        append(out, "\n", cap);
    }
    append(out, "Intel Wi-Fi: transport preparation only; radio backend pending\nBluetooth: driver not implemented\n", cap);
}
static bool parse_ip(const char *s, uint8_t *out) {
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        int n = 0, k = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + *s++ - '0';
            if (++k > 3 || n > 255)
                return false;
        }
        out[i] = n;
        if (i < 3) {
            if (*s++ != '.')
                return false;
        } else if (*s)
            return false;
    }
    return true;
}
bool net_set_dns(const char *server) {
    if(!server) return false;
    bool automatic=!strcmp(server,"auto");
    uint8_t address[4];
    if(!automatic && (!parse_ip(server,address) || !address[0] ||
       address[0]==127 || address[0]>=224 || equal(address,(const uint8_t[]){255,255,255,255},4)))
        return false;
    dns_manual=!automatic;
    if(dns_manual) memcpy(manual_dns,address,4);
    const uint8_t fallback[4]={10,0,2,3};
    apply_dns(external_nic.started && external_nic.dhcp.configured ? external_nic.dhcp.config.dns : fallback);
    return true;
}
static void time_udp(const uint8_t dest[4], uint16_t local, uint16_t remote, const uint8_t *body,
                     unsigned n) {
    uint8_t packet[520] = {0};
    put16(packet, local);
    put16(packet + 2, remote);
    put16(packet + 4, n + 8);
    memcpy(packet + 8, body, n);
    uint32_t pseudo =
        be16(local_ip) + be16(local_ip + 2) + be16(dest) + be16(dest + 2) + 17 + n + 8;
    uint16_t check = sum(packet, n + 8, pseudo);
    put16(packet + 6, check ? check : 65535);
    send_ip(dest, 17, packet, n + 8);
}
void net_time_cancel(void) {
    memset(&time_query, 0, sizeof(time_query));
}
int net_time_begin(const char *host, uint16_t port, const uint8_t request[48]) {
    if (!host || !request || !port || !time_hostname_valid(host))
        return -22;
    if (!net_ready)
        return -100;
    if (network_owner || time_query.phase)
        return -16;
    uint16_t random[2];
    if (!random_bytes(random, sizeof(random)))
        return -5;
    memset(&time_query, 0, sizeof(time_query));
    strcpy(time_query.host, host);
    time_query.port = port;
    time_query.local_port = 49153 + random[0] % 15000;
    if (time_query.local_port == 53000)
        time_query.local_port++;
    time_query.id = random[1];
    memcpy(time_query.request, request, 48);
    if (parse_ip(host, time_query.address))
        time_query.phase = 2;
    else {
        /* Resolve once per query. Keep the dedicated DNS ID/port independent
         * of the foreground resolver and validate the complete question. */
        uint8_t question[160];
        unsigned bytes;
        if (time_dns_question(host, time_query.id, question, sizeof(question), &bytes))
            return -22;
        time_query.phase = 1;
        time_query.deadline = frame_clock + 5000;
        time_udp(dns_ip, 49152, 53, question, bytes);
    }
    return 0;
}
static void time_network_input(const uint8_t source[4], const uint8_t *udp, unsigned bytes) {
    if (time_query.phase == 1 && be16(udp) == 53 && be16(udp + 2) == 49152 &&
        equal(source, dns_ip, 4)) {
        if (!time_dns_answer(time_query.host, time_query.id, udp + 8, bytes - 8,
                             time_query.address))
            time_query.phase = 2;
    } else if (time_query.phase == 3 && be16(udp) == time_query.port &&
               be16(udp + 2) == time_query.local_port && equal(source, time_query.address, 4) &&
               bytes - 8 <= sizeof(time_query.response)) {
        time_query.bytes = bytes - 8;
        memcpy(time_query.response, udp + 8, time_query.bytes);
        time_query.rtt = frame_clock - time_query.sent;
        time_query.phase = 4;
    }
}
static void time_network_poll(void) {
    if (!time_query.phase || time_query.phase == 4)
        return;
    if (!net_ready) {
        time_query.error = -100;
        time_query.phase = 4;
        return;
    }
    if (time_query.phase == 2 && !network_owner) {
        time_query.phase = 3;
        time_query.sent = frame_clock;
        time_query.deadline = frame_clock + 5000;
        time_udp(time_query.address, time_query.local_port, time_query.port, time_query.request,
                 48);
    } else if ((time_query.phase == 1 || time_query.phase == 3) &&
               (int32_t)(frame_clock - time_query.deadline) >= 0) {
        time_query.error = -110;
        time_query.phase = 4;
    }
}
int net_time_result(uint8_t *out, unsigned cap, unsigned *bytes, uint32_t *rtt) {
    if (!out || !bytes || !rtt)
        return -22;
    *bytes = 0;
    if (time_query.phase != 4)
        return -11;
    int rc = time_query.error;
    if (!rc && time_query.bytes > cap)
        rc = -28;
    if (!rc) {
        *bytes = time_query.bytes;
        *rtt = time_query.rtt;
        memcpy(out, time_query.response, *bytes);
    }
    net_time_cancel();
    return rc;
}
static bool resolve(const char *host, uint8_t *out) {
    if (parse_ip(host, out))
        return true;
    File *hosts = fs_find("/etc/hosts");
    if (hosts && !hosts->dir) {
        const char *p = hosts->data;
        while (*p) {
            char address[32], name[224];
            int n = 0;
            while (*p == ' ' || *p == '\t')
                p++;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n < 31)
                address[n++] = *p++;
            address[n] = 0;
            while (*p == ' ' || *p == '\t')
                p++;
            n = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n < 223)
                name[n++] = *p++;
            name[n] = 0;
            while (*p && *p != '\n')
                p++;
            if (*p)
                p++;
            if (!strcmp(name, host) && parse_ip(address, out))
                return true;
        }
    }
    if (!*host || strlen(host) > 200)
        return false;
    if(dns_cache.valid && !strcmp(host,dns_cache.host) &&
       equal(dns_cache.dns,dns_ip,4) && equal(dns_cache.local,local_ip,4) &&
       (int32_t)(frame_clock-dns_cache.expires)<0) {
        memcpy(out,dns_cache.address,4);
        dns_cache.hits++;
        return true;
    }
    uint8_t p[512];
    memset(p, 0, sizeof(p));
    put16(p, 53000);
    put16(p + 2, 53);
    put16(p + 8, ++dnsid);
    p[10] = 1;
    put16(p + 12, 1);
    int pos = 20;
    const char *s = host;
    while (*s) {
        const char *start = s;
        int n = 0;
        while (*s && *s != '.') {
            s++;
            n++;
        }
        if (!n || n > 63)
            return false;
        p[pos++] = n;
        memcpy(p + pos, start, n);
        pos += n;
        if (*s)
            s++;
    }
    p[pos++] = 0;
    put16(p + pos, 1);
    put16(p + pos + 2, 1);
    pos += 4;
    put16(p + 4, pos);
    dns_done = false;
    for (int a = 0; a < 3; a++) {
        dns_requests++;
        send_ip(dns_ip, 17, p, pos);
        uint32_t t = ticks;
        while (ticks - t < 100) {
            net_poll();
            if (dns_done) {
                memcpy(out, resolved, 4);
                /* Cache only successful A answers, bounded to five minutes.
                 * A zero TTL must always cause a new query. */
                dns_cache.valid=resolved_ttl!=0;
                if(dns_cache.valid) {
                    strcpy(dns_cache.host,host);
                    memcpy(dns_cache.address,resolved,4);
                    memcpy(dns_cache.dns,dns_ip,4);
                    memcpy(dns_cache.local,local_ip,4);
                    uint32_t ttl=resolved_ttl>300 ? 300 : resolved_ttl;
                    dns_cache.expires=frame_clock+ttl*1000;
                }
                return true;
            }
            task_yield();
        }
    }
    return false;
}
static bool ping_impl(const char *host, char *out, size_t cap) {
    out[0] = 0;
    if (!net_ready || !ensure_gateway()) {
        append(out, "ping: network/gateway unavailable\n", cap);
        return false;
    }
    uint8_t ip[4];
    if (!resolve(host, ip)) {
        append(out, "ping: name resolution failed\n", cap);
        return false;
    }
    uint8_t p[32];
    memset(p, 0, sizeof(p));
    p[0] = 8;
    put16(p + 4, 0x4c55);
    put16(p + 6, ++ipid);
    memcpy(p + 8, "LunaOS native ICMP", 18);
    put16(p + 2, sum(p, sizeof(p), 0));
    ping_done = false;
    uint32_t start = ticks;
    send_ip(ip, 1, p, sizeof(p));
    while (ticks - start < 300) {
        net_poll();
        if (ping_done) {
            char b[16];
            append(out, "Reply from ", cap);
            append(out, host, cap);
            append(out, ": time <= ", cap);
            number(b, (ticks - start) * 10 + 10);
            append(out, b, cap);
            append(out, " ms\n", cap);
            return true;
        }
        task_yield();
    }
    append(out, "ping: timed out (ICMP may be filtered)\n", cap);
    return false;
}
int stream_connect(const char *host, unsigned port) {
    static uint32_t connections;
    net_poll();
    connect_error="Network interface has no IPv4 configuration.";
    if (!net_ready)
        return -1;
    connect_error="Gateway did not answer ARP. Check Wi-Fi data traffic.";
    if (!ensure_gateway())
        return -1;
    uint8_t ip[4];
    connect_error="DNS resolution failed. Check the configured DNS server.";
    if (!resolve(host, ip))
        return -1;
    memset(&tcp, 0, sizeof(tcp));
    memcpy(tcp.ip, ip, 4);
    tcp.port = 40000 + (++connections % 20000);
    tcp.remote_port = port;
    tcp.seq = ticks * 7919 + connections*65537u + 1000;
    tcp.peer_mss=536;
    transport_error="TCP connecting.";
    tcp.state = 1;
    for (int attempt = 0; attempt < 3 && tcp.state == 1; attempt++) {
        tcp_syns++;
        send_tcp(2, 0, 0);
        uint32_t start = ticks;
        while (tcp.state == 1 && ticks - start < 100) {
            net_poll();
            task_yield();
        }
    }
    if (tcp.state != 2) {
        connect_error=tcp.state<0 ? "TCP connection reset by peer." : "TCP connection timed out after DNS resolution.";
        tcp.state = 0;
        return -1;
    }
    connect_error="TCP connected.";
    transport_error="TCP connected.";
    return 0;
}
int stream_write(const void *data, unsigned n) {
    const char *p = data;
    unsigned total = 0;
    while (total < n) {
        unsigned chunk = n - total;
        if (chunk > TCP_MSS) chunk=TCP_MSS;
        if (chunk > tcp.peer_mss) chunk=tcp.peer_mss;
        uint32_t seq = tcp.seq, next = seq + chunk;
        bool acked = false;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            if (tcp.state != 2)
                return -1;
            tcp.seq = seq;
            send_tcp(0x18, p + total, chunk);
            tcp.seq = next;
            uint32_t start = ticks;
            while (ticks - start < 80) {
                net_poll();
                if ((int32_t)(tcp.remote_ack - next) >= 0) {
                    acked = true;
                    break;
                }
                if (tcp.state < 0)
                    return -1;
                task_yield();
            }
        }
        if (!acked) {
            tcp_timeouts++;transport_error="TCP request ACK timed out.";
            return -1;
        }
        total += chunk;
    }
    return total;
}
int stream_read(void *out, unsigned n) {
    uint32_t start = ticks;
    while (!tcp.used) {
        net_poll();
        /* The peer may close the connection in the same packet that carries
         * the last data: deliver what is buffered before honouring the close. */
        if (tcp.used)
            break;
        if (tcp.state == 3) {
            transport_error="TCP peer closed the connection.";
            return 0;
        }
        if (tcp.state < 2) {
            if(tcp.state!=-1) transport_error="Network link or IPv4 configuration lost.";
            return -1;
        }
        if(ticks-start>1500) {
            tcp_timeouts++;transport_error="TCP receive timed out waiting for missing data.";
            return -1;
        }
        task_yield();
    }
    if (n > tcp.used)
        n = tcp.used;
    memcpy(out, tcp.data, n);
    tcp.used -= n;
    memmove(tcp.data, tcp.data + n, tcp.used);
    tcp_drain_pending();
    send_tcp(0x10, 0, 0);
    return n;
}
void stream_close(void) {
    if (tcp.state == 2)
        send_tcp(0x11, 0, 0);
    tcp.state = 0;
    tcp.used = 0;
}
static bool equals_ci(const char *a, const char *b, size_t n) {
    while (n--) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
    }
    return true;
}
static int http_response(char *response, unsigned used, char *out, size_t cap,
                         char *error, size_t ecap, int *status_out, char *location,
                         size_t locap, bool secure, unsigned secure_error, int n) {
    char *body = 0;
    for (unsigned i = 0; i + 3 < used; i++)
        if (!strncmp(response + i, "\r\n\r\n", 4)) {
            body = response + i + 4;
            break;
        }
    if (!body || strncmp(response, "HTTP/1.", 7) || used < 12) {
        append(error, "Invalid HTTP response.", ecap);
        return -1;
    }
    unsigned code = (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
    /* The redirect target is read from the headers before the status is judged. */
    for (char *h = response; h < body;) {
        char *end = h;
        while (end < body && *end != '\r')
            end++;
        if (equals_ci(h, "Location:", 9)) {
            char *p = h + 9;
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            size_t n = (size_t)(end - p);
            while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
                n--;
            if (location && n && n < locap) {
                memcpy(location, p, n);
                location[n] = 0;
            }
        }
        h = end + 2;
    }
    if (status_out)
        *status_out = (int)code;
    if (code >= 300 && code < 400) {
        if (!location || !location[0]) {
            append(error, "The server sent a redirect without a target address.", ecap);
            return -1;
        }
        /* The body of a redirect is not the document. */
        unsigned redirect_body = used - (unsigned)(body - response);
        if (redirect_body >= cap)
            redirect_body = cap - 1;
        memmove(out, body, redirect_body);
        out[redirect_body] = 0;
        return (int)redirect_body;
    }
    if (code < 200 || code >= 300) {
        append(error, "HTTP status ", ecap);
        char b[16];
        number(b, code);
        append(error, b, ecap);
        append(error, ".", ecap);
        return -1;
    }
    bool chunked = false;
    int content_length = -1;
    for (char *h = response; h < body;) {
        char *end = h;
        while (end < body && *end != '\r')
            end++;
        if (equals_ci(h, "Transfer-Encoding:", 18)) {
            for (char *p = h + 18; p + 7 <= end; p++)
                if (equals_ci(p, "chunked", 7))
                    chunked = true;
        }
        if (equals_ci(h, "Content-Length:", 15)) {
            const char *p = h + 15;
            while (*p == ' ')
                p++;
            content_length = atoi(p);
        }
        if (equals_ci(h, "Content-Encoding:", 17)) {
            char *p = h + 17;
            while (*p == ' ')
                p++;
            if (p < end && !equals_ci(p, "identity", 8)) {
                append(error, "Compressed HTTP responses are not supported.", ecap);
                return -1;
            }
        }
        h = end + 2;
    }
    unsigned available = used - (body - response), written = 0;
    /* Complete, authenticated HTTP framing can finish before TLS close_notify. */
    if (secure && secure_error && secure_error != 31) {
        append(error, "TLS response authentication failed (code ", ecap);
        char b[16];
        number(b, secure_error);
        append(error, b, ecap);
        append(error, ").", ecap);
        return -1;
    }
    if ((secure && secure_error == 31) || (!secure && n < 0)) {
        if (!chunked && (content_length < 0 || (unsigned)content_length > available)) {
            append(error, "Transport interrupted; incomplete response rejected.", ecap);
            append(error," ",ecap);append(error,transport_error,ecap);
            return -1;
        }
    }

    if (chunked) {
        unsigned pos = 0;
        for (;;) {
            unsigned size = 0, digits = 0;
            while (pos < available) {
                char c = body[pos];
                int v = c >= '0' && c <= '9'   ? c - '0'
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                               : -1;
                if (v < 0)
                    break;
                if (++digits > 8 || size > (UINT32_MAX - (unsigned)v) / 16u) {
                    append(error, "Malformed chunked response size.", ecap);
                    return -1;
                }
                size = size * 16 + v;
                pos++;
            }
            if (!digits) {
                append(error, "Malformed chunked response size.", ecap);
                return -1;
            }
            while (pos + 1 < available && !(body[pos] == '\r' && body[pos + 1] == '\n'))
                pos++;
            if (pos + 1 >= available) {
                append(error, "Incomplete chunked response.", ecap);
                return -1;
            }
            pos += 2;
            if (!size) {
                /* A complete zero chunk includes the final empty trailer
                 * line. Preserve truncation detection even after all data. */
                for (;;) {
                    unsigned end = pos;
                    while (end + 1 < available && !(body[end] == '\r' && body[end + 1] == '\n'))
                        end++;
                    if (end + 1 >= available) {
                        append(error, "Incomplete chunked response trailers.", ecap);
                        return -1;
                    }
                    bool last = end == pos;
                    pos = end + 2;
                    if (last) break;
                }
                break;
            }
            if (size > available - pos) {
                append(error, "Incomplete chunked response.", ecap);
                return -1;
            }
            if (size >= cap - written) {
                append(error, "Chunked document exceeds the destination size limit.", ecap);
                return -1;
            }
            memmove(out + written, body + pos, size);
            written += size;
            pos += size;
            if (pos + 1 >= available) {
                append(error, "Incomplete chunked response.", ecap);
                return -1;
            }
            if (body[pos] != '\r' || body[pos + 1] != '\n') {
                append(error, "Malformed chunked response delimiter.", ecap);
                return -1;
            }
            pos += 2;
        }
    } else {
        if (content_length >= 0 && (unsigned)content_length > available) {
            append(error, "Incomplete HTTP document.", ecap);
            return -1;
        }
        written = content_length >= 0 ? (unsigned)content_length : available;
        if (written >= cap) {
            append(error, "Document exceeds the destination size limit.", ecap);
            return -1;
        }
        memmove(out, body, written);
    }
    out[written] = 0;
    return written;
}

static int http_once(const char *url, char *out, size_t cap, char *error, size_t ecap,
                     int *status_out, char *location, size_t locap, char **download) {
    out[0] = error[0] = 0;
    if (status_out)
        *status_out = 0;
    if (location && locap)
        location[0] = 0;
    bool secure = !strncmp(url, "https://", 8);
    if (!secure && strncmp(url, "http://", 7)) {
        append(error, "Expected an http:// or https:// address.", ecap);
        return -1;
    }
    char host[224], path[4096];
    const char *s = url + (secure ? 8 : 7);
    unsigned k = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#') {
        if (k + 1 >= sizeof(host) || *s == '@' || (unsigned char)*s <= 32) {
            append(error, "Invalid host.", ecap);
            return -1;
        }
        host[k++] = *s++;
    }
    host[k] = 0;
    if (!k)
        return -1;
    unsigned port = secure ? 443 : 80;
    if (*s == ':') {
        s++;
        port = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            port = port * 10 + *s++ - '0';
            if (++digits > 5 || port > 65535)
                break;
        }
        if (!digits || !port || port > 65535) {
            append(error, "Invalid port.", ecap);
            return -1;
        }
    }
    if (strlen(s) > sizeof(path) - 3) {
        append(error, "URL too long.", ecap);
        return -1;
    }
    path[0] = 0;
    if (*s != '/')
        strcpy(path, "/");
    strcat(path, s);
    for (char *p = path; *p; p++) {
        if ((unsigned char)*p <= 32) {
            append(error, "Invalid URL characters.", ecap);
            return -1;
        }
        if (*p == '#') {
            *p = 0;
            break;
        }
    }
    if (stream_connect(host, port) < 0) {
        append(error, connect_error, ecap);
        return -1;
    }
    if (secure && !tls_connect(host, error, ecap)) {
        stream_close();
        return -1;
    }
    char request[4608] = "GET ";
    append(request, path, sizeof(request));
    append(request, " HTTP/1.0\r\nHost: ", sizeof(request));
    append(request, host, sizeof(request));
    if (port != (secure ? 443u : 80u)) {
        char p[16];
        number(p, port);
        append(request, ":", sizeof(request));
        append(request, p, sizeof(request));
    }
    append(
        request,
        "\r\nUser-Agent: LunaBrowser/0.3\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        sizeof(request));
    if ((secure ? tls_write(request, strlen(request)) : stream_write(request, strlen(request))) <
        0) {
        append(error,
               secure ? "TLS handshake / certificate validation failed." : "HTTP request failed.",
               ecap);
        stream_close();
        return -1;
    }
    size_t capacity = download ? 16384u : WEB_CAP + 8192u;
    char *response = heap_allocate(capacity);
    if (!response) {
        stream_close();
        append(error, "Insufficient memory for HTTP response.", ecap);
        return -1;
    }
    unsigned used = 0;
    int n = 0;
    bool complete = true;
    for (;;) {
        if (used + 1u == capacity) {
            if (!download || capacity > 0x7fffffffu / 2u) {
                append(error, "Response exceeds the destination size limit.", ecap);
                complete = false;
                break;
            }
            char *next = heap_resize(response, capacity * 2u);
            if (!next) {
                append(error, "Insufficient memory for HTTP response.", ecap);
                complete = false;
                break;
            }
            response = next;
            capacity *= 2u;
        }
        n = secure ? tls_read(response + used, capacity - used - 1u)
                   : stream_read(response + used, capacity - used - 1u);
        if (n <= 0) break;
        used += (unsigned)n;
    }
    response[used] = 0;
    unsigned secure_error = secure ? tls_error() : 0;
    stream_close();
    int result = -1;
    if (complete) {
        if (download) { out = response; cap = capacity; }
        result = http_response(response, used, out, cap, error, ecap, status_out,
                               location, locap, secure, secure_error, n);
    }
    if (download && result >= 0) *download = response;
    else heap_release(response);
    return result;
}

/* Builds an absolute address from a base document and a reference that may
 * be absolute, protocol relative, rooted or relative to the base directory. */
static bool url_resolve(const char *base, const char *reference, char *out, size_t cap) {
    return net_url_resolve(base,reference,out,cap);
}

static int http_impl(const char *url, char *out, size_t cap, char *error, size_t ecap, bool https_only, char **download) {
    final_http_url[0]=0;
    char current[4096];
    size_t n = strlen(url);
    if (n >= sizeof(current)) {
        out[0] = 0;
        error[0] = 0;
        append(error, "The address is too long.", ecap);
        return -1;
    }
    memcpy(current, url, n + 1);
    for (int hop = 0; hop < 8; hop++) {
        if (https_only && strncmp(current, "https://", 8)) {
            append(error, "HTTPS downgrade refused.", ecap);
            return -1;
        }
        int status = 0;
        char location[4096];
        location[0] = 0;
        int result = http_once(current, out, cap, error, ecap, &status, location, sizeof(location), download);
        if (result < 0)
            return -1;
        if (status < 300 || status >= 400) {
            strcpy(final_http_url,current);
            return result;
        }
        if (download && *download) {
            heap_release(*download);
            *download = 0;
        }
        char next[4096];
        if (!url_resolve(current, location, next, sizeof(next))) {
            append(error, "The redirect target could not be resolved.", ecap);
            return -1;
        }
        memcpy(current, next, strlen(next) + 1);
    }
    append(error, "The server redirected too many times.", ecap);
    return -1;
}

bool net_http_final_url(char *out,size_t cap) {
    if(!out || !cap) return false;
    out[0]=0;
    size_t n=strlen(final_http_url);
    if(!n || n>=cap) return false;
    memcpy(out,final_http_url,n+1);return true;
}
void net_cancel_owner(uint32_t pid) {
    if (network_owner == pid) {
        network_owner = 0;
        tcp.state = 0;
    }
}
bool net_ping(const char *host, char *out, size_t cap) {
    if (network_owner) {
        out[0] = 0;
        append(out, "ping: network service busy\n", cap);
        return false;
    }
    network_owner = task_current()->pid;
    bool ok = ping_impl(host, out, cap);
    network_owner = 0;
    return ok;
}
int net_http(const char *url, char *out, size_t cap, char *error, size_t ecap) {
    if (network_owner) {
        out[0] = error[0] = 0;
        append(error, "Network service busy; try again shortly.", ecap);
        return -1;
    }
    network_owner = task_current()->pid;
    int result = http_impl(url, out, cap, error, ecap, false, 0);
    if(result<0) {
        failed_http_url[0]=failed_http_error[0]=0;
        append(failed_http_url,url,sizeof(failed_http_url));
        append(failed_http_error,error,sizeof(failed_http_error));
    }
    network_owner = 0;
    return result;
}

/* Updater transport authenticates every redirect and never permits HTTP. */
int net_https(const char *url, char *out, size_t cap, char *error, size_t ecap) {
    if (!cap || !ecap) return -1;
    out[0] = error[0] = 0;
    if (network_owner) { append(error, "Network service busy.", ecap); return -1; }
    network_owner = task_current()->pid;
    int result = http_impl(url, out, cap, error, ecap, true, 0);
    network_owner = 0;
    return result;
}

/* Caller owns the complete decoded document and releases it with heap_release. */
int net_download(const char *url, char **out, char *error, size_t ecap) {
    if (!out || !error || !ecap) return -1;
    *out = 0;
    error[0] = 0;
    if (network_owner) {
        append(error, "Network service busy.", ecap);
        return -1;
    }
    network_owner = task_current()->pid;
    char unused[1] = {0};
    int result = http_impl(url, unused, sizeof(unused), error, ecap, false, out);
    network_owner = 0;
    return result;
}
