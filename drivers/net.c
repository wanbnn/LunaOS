/* Native RTL8139 + Ethernet/ARP/IPv4/ICMP/UDP DNS/TCP HTTP.
 * One synchronous TCP connection, static QEMU user-network profile. */
#include "../include/net/http_stream.h"
#include "../include/net/time_dns.h"
#include "heap.h"
#include "luna.h"
#include "net_time.h"
#include "netif.h"
#include "task.h"
#include "web.h"
#include "wifi.h"
/* A response body that grows as the document demands. A fixed destination
 * is used by callers that own a buffer; an owned one is allocated here and
 * released by the caller. */
#define NET_BODY_MAX (4u * 1024u * 1024u)
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool owned;
} net_body;

/* Makes room for `needed` bytes plus the terminator. */
static bool net_body_reserve(net_body *b, size_t needed) {
    if (needed + 1 <= b->cap)
        return true;
    if (!b->owned) {
        /* The destination belongs to the caller and cannot grow. */
        return false;
    }
    size_t cap = b->cap ? b->cap : 8192;
    while (cap < needed + 1) {
        if (cap >= NET_BODY_MAX)
            return false;
        cap *= 2;
    }
    if (cap > NET_BODY_MAX + 1)
        cap = NET_BODY_MAX + 1;
    char *data = heap_allocate(cap);
    if (!data)
        return false;
    if (b->data && b->len)
        memcpy(data, b->data, b->len);
    if (b->data)
        heap_release(b->data);
    b->data = data;
    b->cap = cap;
    return true;
}

/* Copies a run of bytes into the body, growing it when it owns the memory. */
static bool net_body_append(net_body *b, const char *src, size_t size) {
    if (!size)
        return true;
    if (!net_body_reserve(b, b->len + size))
        return false;
    memcpy(b->data + b->len, src, size);
    b->len += size;
    b->data[b->len] = 0;
    return true;
}

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
#define TCP_MSS 1460u
#define TCP_RECEIVE_MIN 16384u
/* The update stream warms up at 1 MiB, then the existing delivery-rate/RTT
 * auto-tuner grows towards 10 MiB if the peer and RAM budget permit. Do not
 * preallocate 10 MiB on machines at the 256 MiB installer minimum. */
#define TCP_BULK_RECEIVE_WARMUP (1u * 1024u * 1024u)
#define TCP_BULK_RECEIVE_TARGET (10u * 1024u * 1024u)
#define TCP_WINDOW_UPDATE_MIN (2u * TCP_MSS)
#define TCP_WINDOW_UPDATE_MAX (16u * TCP_MSS)
#define TCP_UNSCALED_MAX 65535u
#define TCP_RECEIVE_MAX (65535u << 14)
#define TCP_PAWS_AGE_MS (24u * 24u * 60u * 60u * 1000u)
#define TCP_RTO_MIN 1000u
#define TCP_RTO_MAX 60000u
#define HTTP_IDLE_TIMEOUT_MS 60000u
#define HTTP_OPERATION_TIMEOUT_MS 300000u
typedef struct TcpPending {
    struct TcpPending *next;
    uint32_t seq;
    unsigned bytes;
    bool fin;
    uint8_t data[];
} TcpPending;
static const char *transport_error="No transfer attempted.";
static uint32_t tcp_gaps,tcp_duplicates,tcp_queue_drops,tcp_resets,tcp_timeouts;
static struct {
    int state;
    uint8_t ip[4];
    uint16_t port, remote_port;
    uint32_t seq, ack, remote_ack;
    char *data;
    unsigned capacity, goal, used, head, peer_mss, limit, memory_limit;
    unsigned scale_offer, scale_rx, scale_tx;
    bool scaled, timestamps, ack_pending, peer_window_valid;
    uint32_t ts_recent, ts_recent_at, last_ack_sent;
    uint32_t peer_right, peer_window_seq, peer_window_ack;
    uint32_t rate_at;
    uint64_t rate_bytes;
    uint32_t received_bytes, receive_at, receive_last;
    bool receive_started;
    TcpPending *pending, *pending_tail;
    size_t pending_memory;
    uint32_t right_edge, pressure_at;
    bool window_valid;
    uint32_t srtt, rttvar, rto, sample_at, sample_end;
    bool measured, sample_active;
    uint32_t idle_min, idle_max;
} tcp;
/* HTTP request deadlines are application policy, separate from TCP's RTO.
 * Streaming downloads use an idle limit without a whole-file deadline. */
static struct {
    bool active;
    uint32_t start, limit;
} http_operation;
static uint32_t http_timeouts;
static uint32_t tcp_last_srtt, tcp_last_rto, tcp_peak_capacity, tcp_allocation_failures;
static uint32_t tcp_last_scale, tcp_last_limit, tcp_last_receive_bytes, tcp_last_receive_ms;
static uint32_t tcp_paws_drops, tcp_ack_frames;
static bool http_expired(void) {
    if (!http_operation.active || !http_operation.limit ||
        frame_clock - http_operation.start < http_operation.limit)
        return false;
    transport_error = "HTTP operation deadline exceeded.";
    return true;
}
static void http_begin(uint32_t limit) {
    http_operation.active = true;
    http_operation.start = frame_clock;
    http_operation.limit = limit;
}
static int http_end(int result, net_body *out, char *error, size_t capacity) {
    if (http_expired()) {
        http_timeouts++;
        if (out->data)
            out->data[0] = 0;
        error[0] = 0;
        append(error, transport_error, capacity);
        result = -1;
    }
    http_operation.active = false;
    return result;
}
static uint32_t tcp_rto(void) {
    return tcp.rto ? tcp.rto : TCP_RTO_MIN;
}
static void tcp_release(void) {
    if (tcp.receive_started) {
        tcp_last_receive_bytes = tcp.received_bytes;
        tcp_last_receive_ms = tcp.receive_last - tcp.receive_at;
    }
    while (tcp.pending) {
        TcpPending *next = tcp.pending->next;
        heap_release(tcp.pending);
        tcp.pending = next;
    }
    if (tcp.data)
        heap_release(tcp.data);
    memset(&tcp, 0, sizeof(tcp));
}
static void tcp_copy_data(void *out, unsigned offset, unsigned bytes) {
    if (!bytes)
        return;
    unsigned position = (tcp.head + offset) % tcp.capacity;
    unsigned first = tcp.capacity - position;
    if (first > bytes)
        first = bytes;
    memcpy(out, tcp.data + position, first);
    if (bytes > first)
        memcpy((uint8_t *)out + first, tcp.data, bytes - first);
}
static void tcp_memory_profile(void) {
    uint64_t budget = ((uint64_t)memory_mb << 20) / 4u;
    if (budget < TCP_RECEIVE_MIN)
        budget = TCP_RECEIVE_MIN;
    if (budget > TCP_RECEIVE_MAX)
        budget = TCP_RECEIVE_MAX;
    tcp.memory_limit = tcp.limit = (unsigned)budget;
    tcp.scale_offer = 0;
    while ((tcp.memory_limit >> tcp.scale_offer) > 65535u && tcp.scale_offer < 14u)
        tcp.scale_offer++;
    tcp_last_limit = tcp.memory_limit;
}
static bool tcp_resize(unsigned capacity) {
    char *data = heap_allocate_owned((size_t)capacity + 1u, 1);
    if (!data) {
        tcp_allocation_failures++;
        return false;
    }
    if (capacity > tcp_peak_capacity)
        tcp_peak_capacity = capacity;
    if (tcp.used)
        tcp_copy_data(data, 0, tcp.used);
    data[tcp.used] = 0;
    if (tcp.data)
        heap_release(tcp.data);
    tcp.data = data;
    tcp.head = 0;
    tcp.capacity = capacity;
    return true;
}
static void tcp_grow(unsigned desired) {
    unsigned limit = tcp.limit ? tcp.limit : TCP_UNSCALED_MAX;
    if (desired > limit)
        desired = limit;
    if (desired <= tcp.capacity) {
        if (desired > tcp.goal)
            tcp.goal = desired;
        return;
    }
    unsigned capacity = tcp.capacity ? tcp.capacity : TCP_RECEIVE_MIN;
    while (capacity < desired) {
        if (capacity > limit / 2u) {
            capacity = limit;
            break;
        }
        capacity *= 2u;
    }
    if (tcp_resize(capacity))
        tcp.goal = capacity;
}
static void tcp_delivery_sample(unsigned bytes) {
    if (!tcp.rate_at)
        tcp.rate_at = frame_clock;
    tcp.rate_bytes += bytes;
    uint32_t elapsed = frame_clock - tcp.rate_at;
    uint32_t interval = tcp.measured ? tcp.srtt : 10u;
    if (interval < 10u)
        interval = 10u;
    if (elapsed < interval)
        return;
    uint64_t target =
        (uint64_t)tcp.rate_bytes * (tcp.measured ? tcp.srtt : interval) * 2u / elapsed;
    target += 4u * TCP_MSS;
    if (target > tcp.goal) {
        tcp.pressure_at = frame_clock;
        tcp_grow(target > TCP_RECEIVE_MAX ? TCP_RECEIVE_MAX : (unsigned)target);
    }
    tcp.rate_at = frame_clock;
    tcp.rate_bytes = 0;
}
static void tcp_rtt_ack(uint32_t ack) {
    if (!tcp.sample_active || (int32_t)(ack - tcp.sample_end) < 0)
        return;
    tcp.sample_active = false;
    uint32_t sample = frame_clock - tcp.sample_at;
    if (!sample)
        sample = 1;
    if (sample > TCP_RTO_MAX)
        sample = TCP_RTO_MAX;
    if (!tcp.measured) {
        tcp.srtt = sample;
        tcp.rttvar = sample / 2u;
        tcp.measured = true;
    } else {
        uint32_t difference = tcp.srtt > sample ? tcp.srtt - sample : sample - tcp.srtt;
        tcp.rttvar = (3u * tcp.rttvar + difference) / 4u;
        tcp.srtt = (7u * tcp.srtt + sample) / 8u;
    }
    uint32_t variation = 4u * tcp.rttvar;
    tcp.rto = tcp.srtt + (variation ? variation : 1u);
    if (tcp.rto < TCP_RTO_MIN)
        tcp.rto = TCP_RTO_MIN;
    if (tcp.rto > TCP_RTO_MAX)
        tcp.rto = TCP_RTO_MAX;
    tcp_last_srtt = tcp.srtt;
    tcp_last_rto = tcp.rto;
}
static void tcp_backoff(void) {
    tcp.sample_active = false;
    uint32_t rto = tcp_rto();
    tcp.rto = rto > TCP_RTO_MAX / 2u ? TCP_RTO_MAX : rto * 2u;
    tcp_last_rto = tcp.rto;
}
static unsigned tcp_granted(void) {
    if (!tcp.window_valid || (int32_t)(tcp.right_edge - tcp.ack) <= 0)
        return 0;
    return tcp.right_edge - tcp.ack;
}
static unsigned tcp_window(void) {
    unsigned desired = tcp.goal > tcp.used ? tcp.goal - tcp.used : 0;
    unsigned granted = tcp_granted();
    unsigned window = desired > granted ? desired : granted;
    unsigned available = tcp.capacity - tcp.used;
    return window > available ? available : window;
}
static void tcp_tune_receive(void) {
    uint32_t quiet = tcp_rto() * 4u;
    if (quiet < 2000u)
        quiet = 2000u;
    if (tcp.pending)
        return;
    /* A streaming reader often drains the ring immediately. Low occupancy is
     * not idleness: shrinking it during an active transfer constrains the
     * advertised receive window and can induce avoidable sender stalls. */
    if (tcp.receive_started && (uint32_t)(frame_clock - tcp.receive_last) < quiet)
        return;
    unsigned minimum = 2u * (1u << tcp.scale_rx);
    if (minimum < TCP_RECEIVE_MIN)
        minimum = TCP_RECEIVE_MIN;
    if (frame_clock - tcp.pressure_at >= quiet && tcp.used <= tcp.goal / 4u && tcp.goal > minimum) {
        tcp.goal /= 2u;
        if (tcp.goal < minimum)
            tcp.goal = minimum;
        tcp.pressure_at = frame_clock;
    }
    /* Previously granted bytes remain valid until consumed. Defer physical
     * shrinking until the peer's outstanding credit fits the smaller buffer. */
    if (tcp.capacity > tcp.goal && tcp.used + tcp_granted() <= tcp.goal)
        tcp_resize(tcp.goal);
}
void stream_set_read_policy(uint32_t minimum, uint32_t maximum) {
    tcp.idle_min = minimum;
    tcp.idle_max = maximum < minimum ? minimum : maximum;
}
static uint32_t tcp_read_budget(void) {
    uint32_t minimum = tcp.idle_min ? tcp.idle_min : 15000u;
    uint32_t maximum = tcp.idle_max ? tcp.idle_max : 120000u;
    uint32_t budget = tcp_rto() * 8u;
    if (budget < minimum)
        budget = minimum;
    return budget > maximum ? maximum : budget;
}
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
    uint8_t p[1480];
    unsigned header = (flags & 2) ? 40u : tcp.timestamps ? 32u : 20u;
    memset(p, 0, header);
    put16(p, tcp.port);
    put16(p + 2, tcp.remote_port);
    put32(p + 4, tcp.seq);
    put32(p + 8, tcp.ack);
    p[12] = (header/4)<<4;
    p[13] = flags;
    unsigned shift = (flags & 2) ? 0u : tcp.scale_rx;
    unsigned window = tcp_window() >> shift;
    if (window > 65535u)
        window = 65535u;
    put16(p + 14, window);
    if (!(flags & 2) && tcp.state >= 2) {
        uint32_t edge = tcp.ack + (window << shift);
        if (!tcp.window_valid || (int32_t)(edge - tcp.right_edge) > 0)
            tcp.right_edge = edge;
        tcp.window_valid = true;
        tcp.last_ack_sent = tcp.ack;
        tcp.ack_pending = false;
        tcp_ack_frames++;
    }
    if (flags & 2) {
        p[20] = 2;
        p[21] = 4;
        put16(p + 22, TCP_MSS);
        p[24] = 1;
        p[25] = 3;
        p[26] = 3;
        p[27] = tcp.scale_offer;
        p[28] = p[29] = 1;
        p[30] = 8;
        p[31] = 10;
        put32(p + 32, frame_clock);
        put32(p + 36, 0);
    } else if (tcp.timestamps) {
        p[20] = p[21] = 1;
        p[22] = 8;
        p[23] = 10;
        put32(p + 24, frame_clock);
        put32(p + 28, tcp.ts_recent);
    }
    if (n)
        memcpy(p + header, data, n);
    uint32_t s = be16(local_ip) + be16(local_ip + 2) + be16(tcp.ip) + be16(tcp.ip + 2) + 6 + header + n;
    put16(p + 16, sum(p, header + n, s));
    send_ip(tcp.ip, 6, p, header + n);
}
/* Limit UPDATE HTTPS receive-buffer memory to 10 MiB without reserving it.
 * The SYN already advertised the system's memory-dependent window scale.
 * Normal HTTP retains its own larger/unscaled autotuning limits. A stream
 * starts at 1 MiB and grows to the cap only when measured delivery/RTT calls
 * tcp_grow(). Allocation failure keeps the previous, valid credit. */
static void tcp_prepare_bulk_receive(void) {
    if (tcp.state != 2) return;
    if (tcp.limit > TCP_BULK_RECEIVE_TARGET)
        tcp.limit = TCP_BULK_RECEIVE_TARGET;
    unsigned previous = tcp.capacity;
    tcp_grow(TCP_BULK_RECEIVE_WARMUP);
    if (tcp.capacity > previous)
        send_tcp(0x10, NULL, 0); /* Announce additional credit to the peer. */
}
static void tcp_flush_ack(void) {
    if (tcp.ack_pending)
        send_tcp(0x10, NULL, 0);
}
/* Consume only the contiguous suffix of a segment, trimming retransmitted
 * prefixes. Comparisons use signed differences to survive sequence wrap. */
static bool tcp_contiguous(uint32_t seq,const uint8_t *data,unsigned bytes,bool fin) {
    if(tcp.state!=2) return true;
    if((int32_t)(seq-tcp.ack)>0) return false;
    uint32_t end=seq+bytes;
    unsigned skip=tcp.ack-seq;
    unsigned fresh=skip<bytes ? bytes-skip : 0;
    if (fresh > tcp.capacity - tcp.used)
        return false;
    if (fresh && tcp.used + fresh > tcp.goal / 2u) {
        tcp.pressure_at = frame_clock;
        tcp.goal = tcp.capacity;
        if (tcp.used + fresh > tcp.capacity / 2u)
            tcp_grow(tcp.capacity > TCP_RECEIVE_MAX / 2u ? TCP_RECEIVE_MAX : tcp.capacity * 2u);
    }
    if(fresh) {
        unsigned tail = (tcp.head + tcp.used) % tcp.capacity;
        unsigned first = tcp.capacity - tail;
        if (first > fresh)
            first = fresh;
        memcpy(tcp.data + tail, data + skip, first);
        if (fresh > first)
            memcpy(tcp.data, data + skip + first, fresh - first);
        tcp.used += fresh;
        tcp.ack += fresh;
        if (!tcp.receive_started) {
            tcp.receive_started = true;
            tcp.receive_at = frame_clock;
        }
        tcp.received_bytes += fresh;
        tcp.receive_last = frame_clock;
    } else if(bytes) tcp_duplicates++;
    if(fin && end==tcp.ack) {
        tcp.ack++;send_tcp(0x11,NULL,0);tcp.seq++;tcp.state=3;
    }
    return true;
}
static void tcp_drain_pending(void) {
    while (tcp.pending && (int32_t)(tcp.pending->seq - tcp.ack) <= 0) {
        TcpPending *packet = tcp.pending;
        if (!tcp_contiguous(packet->seq, packet->data, packet->bytes, packet->fin))
            break;
        tcp.pending = packet->next;
        if (!tcp.pending)
            tcp.pending_tail = NULL;
        tcp.pending_memory -= sizeof(*packet) + packet->bytes;
        heap_release(packet);
    }
}
static void tcp_payload(uint32_t seq,const uint8_t *data,unsigned bytes,bool fin) {
    if (tcp_contiguous(seq, data, bytes, fin))
        tcp_drain_pending();
    else if ((int32_t)(seq - tcp.ack) > 0) {
        tcp_gaps++;
        unsigned gap = seq - tcp.ack;
        unsigned window = tcp.window_valid ? tcp_granted() : tcp.capacity - tcp.used;
        if(bytes<=1460 && gap<window && bytes<=window-gap) {
            TcpPending **link = &tcp.pending;
            if (tcp.pending_tail && (int32_t)(seq - tcp.pending_tail->seq) > 0)
                link = &tcp.pending_tail->next;
            while (*link && (int32_t)((*link)->seq - seq) <= 0) {
                if ((*link)->seq == seq && (*link)->bytes == bytes && (*link)->fin == fin) {
                    tcp_duplicates++;
                    send_tcp(0x10, NULL, 0);
                    return;
                }
                link = &(*link)->next;
            }
            size_t allocation = sizeof(TcpPending) + bytes;
            if (allocation <= tcp.capacity - tcp.pending_memory) {
                TcpPending *packet = heap_allocate_owned(allocation, 1);
                if (packet) {
                    *packet = (TcpPending){.next = *link, .seq = seq, .bytes = bytes, .fin = fin};
                    if (bytes)
                        memcpy(packet->data, data, bytes);
                    *link = packet;
                    if (!packet->next)
                        tcp.pending_tail = packet;
                    tcp.pending_memory += allocation;
                    send_tcp(0x10, NULL, 0);
                    return;
                }
                tcp_allocation_failures++;
            }
        }
        tcp_queue_drops++;
    }
    if (bytes || fin) {
        if ((int32_t)(seq - tcp.ack) > 0 || fin)
            send_tcp(0x10, NULL, 0);
        else
            tcp.ack_pending = true;
    }
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
typedef struct {
    unsigned mss, scale;
    bool has_scale, has_time;
    uint32_t timestamp;
} TcpOptions;
static bool tcp_options(const uint8_t *data, unsigned header, TcpOptions *options) {
    *options = (TcpOptions){.mss = 536};
    for (unsigned i = 20; i < header;) {
        unsigned kind = data[i++];
        if (!kind)
            break;
        if (kind == 1)
            continue;
        if (i >= header || data[i] < 2 || data[i] > header - i + 1)
            return false;
        unsigned count = data[i++];
        if (kind == 2) {
            if (count != 4 || !be16(data + i))
                return false;
            options->mss = be16(data + i);
        }
        if (kind == 3) {
            if (count != 3 || options->has_scale)
                return false;
            options->has_scale = true;
            options->scale = data[i] > 14 ? 14 : data[i];
        }
        if (kind == 8) {
            if (count != 10 || options->has_time)
                return false;
            options->has_time = true;
            options->timestamp = be32(data + i);
        }
        i += count - 2u;
    }
    return true;
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
        TcpOptions options;
        if (!tcp_options(d, th, &options))
            return;
        tcp.peer_mss = options.mss;
        tcp.scaled = options.has_scale;
        tcp.scale_rx = options.has_scale ? tcp.scale_offer : 0;
        tcp.scale_tx = options.has_scale ? options.scale : 0;
        tcp.limit = options.has_scale ? tcp.memory_limit : TCP_UNSCALED_MAX;
        if (!tcp.limit)
            tcp.limit = TCP_UNSCALED_MAX;
        tcp_last_scale = tcp.scale_rx;
        tcp_last_limit = tcp.limit;
        tcp.timestamps = options.has_time;
        tcp.ts_recent = options.timestamp;
        tcp.ts_recent_at = frame_clock;
        tcp.peer_right = ack + be16(d + 14);
        tcp.peer_window_valid = true;
        tcp.peer_window_seq = seq;
        tcp.peer_window_ack = ack;
        tcp.seq++;
        tcp.remote_ack=ack;
        tcp_rtt_ack(ack);
        tcp.ack = seq + 1;
        tcp.state = 2;
        send_tcp(0x10, 0, 0);
        return;
    }
    if (tcp.state < 2)
        return;
    TcpOptions options;
    if (!tcp_options(d, th, &options))
        return;
    if (tcp.timestamps) {
        if (!options.has_time || ((int32_t)(options.timestamp - tcp.ts_recent) < 0 &&
                                  frame_clock - tcp.ts_recent_at < TCP_PAWS_AGE_MS)) {
            tcp_paws_drops++;
            send_tcp(0x10, NULL, 0);
            return;
        }
        if ((int32_t)(seq - tcp.last_ack_sent) <= 0) {
            tcp.ts_recent = options.timestamp;
            tcp.ts_recent_at = frame_clock;
        }
    }
    if(!(flags&0x10) || (int32_t)(ack-tcp.seq)>0) {send_tcp(0x10,NULL,0);return;}
    if ((int32_t)(ack - tcp.remote_ack) > 0) {
        tcp.remote_ack = ack;
        tcp_rtt_ack(ack);
    }
    if (!tcp.peer_window_valid || (int32_t)(seq - tcp.peer_window_seq) > 0 ||
        (seq == tcp.peer_window_seq && (int32_t)(ack - tcp.peer_window_ack) >= 0)) {
        tcp.peer_right = ack + ((uint32_t)be16(d + 14) << tcp.scale_tx);
        tcp.peer_window_seq = seq;
        tcp.peer_window_ack = ack;
        tcp.peer_window_valid = true;
    }
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
/* A 64 KiB RTL8139 receive ring always wraps, including with RxNoWrap set.
 * Copy the second fragment from the start of the DMA ring. */
static void copy_rx_frame(void *out, unsigned position, unsigned bytes) {
    position %= 65536u;
    unsigned first = 65536u - position;
    if (first > bytes)
        first = bytes;
    memcpy(out, rx + position, first);
    if (bytes > first)
        memcpy((uint8_t *)out + first, rx, bytes - first);
}
static void network_poll(void) {
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
        tcp_flush_ack();
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
            copy_rx_frame(packet, rxpos + 4u, len - 4u);
            receive(packet, len - 4);
            rxcount++;
        }
        rxpos = (rxpos + len + 4 + 3) & ~3u;
        rxpos %= 65536;
        outw(io + 0x38, rxpos - 16);
    }
    outw(io + 0x3e, 0xffff);
    tcp_flush_ack();
    time_network_poll();
}
void net_poll(void) {
    PerfArea previous = perf_enter(PERF_NETWORK);
    network_poll();
    perf_leave(previous);
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
        const uint32_t adaptive[] = {
            tcp.capacity, (uint32_t)tcp.pending_memory, tcp_peak_capacity, tcp_last_srtt,
            tcp_last_rto, tcp_allocation_failures,      http_timeouts};
        const char *adaptive_fields[] = {"TCP receive buffer bytes: ",
                                         "TCP reorder memory bytes: ",
                                         "TCP peak buffer bytes: ",
                                         "TCP last RTT ms: ",
                                         "TCP RTO ms: ",
                                         "TCP allocation failures: ",
                                         "HTTP timeouts: "};
        for (unsigned i = 0; i < 7; i++) {
            char value[16];
            number(value, adaptive[i]);
            append(out, adaptive_fields[i], cap);
            append(out, value, cap);
            append(out, "\n", cap);
        }
        const uint32_t scaled[] = {tcp_last_scale,      tcp_last_limit, tcp_last_receive_bytes,
                                   tcp_last_receive_ms, tcp_paws_drops, tcp_ack_frames};
        const char *scaled_fields[] = {
            "TCP receive scale: ",   "TCP receive budget bytes: ", "TCP last receive bytes: ",
            "TCP last receive ms: ", "TCP timestamp drops: ",      "TCP ACK frames: "};
        for (unsigned i = 0; i < 6; i++) {
            char value[16];
            number(value, scaled[i]);
            append(out, scaled_fields[i], cap);
            append(out, value, cap);
            append(out, "\n", cap);
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
    stream_close();
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
    if (http_expired()) {
        connect_error = transport_error;
        return -1;
    }
    tcp_memory_profile();
    unsigned initial = 2u * (1u << tcp.scale_offer);
    if (initial < TCP_RECEIVE_MIN)
        initial = TCP_RECEIVE_MIN;
    if (!tcp_resize(initial)) {
        connect_error = "Insufficient memory for TCP receive buffer.";
        return -1;
    }
    tcp.goal = tcp.capacity;
    tcp.rto = TCP_RTO_MIN;
    tcp.pressure_at = frame_clock;
    memcpy(tcp.ip, ip, 4);
    tcp.port = 40000 + (++connections % 20000);
    tcp.remote_port = port;
    tcp.seq = frame_clock * 7919 + connections*65537u + 1000;
    tcp.peer_mss=536;
    transport_error="TCP connecting.";
    tcp.state = 1;
    for (int attempt = 0; attempt < 3 && tcp.state == 1; attempt++) {
        tcp_syns++;
        tcp.sample_active = attempt == 0;
        tcp.sample_at = frame_clock;
        tcp.sample_end = tcp.seq + 1u;
        send_tcp(2, 0, 0);
        uint32_t start = frame_clock;
        while (tcp.state == 1 && frame_clock - start < tcp_rto()) {
            net_poll();
            if (http_expired()) {
                connect_error = transport_error;
                stream_close();
                return -1;
            }
            if (tcp.state != 1)
                break;
            task_yield();
        }
        if (tcp.state == 1)
            tcp_backoff();
        else if (attempt && tcp.rto < 3000u)
            tcp.rto = 3000u;
    }
    if (tcp.state != 2) {
        connect_error=tcp.state<0 ? "TCP connection reset by peer." : "TCP connection timed out after DNS resolution.";
        stream_close();
        return -1;
    }
    tcp_last_rto = tcp_rto();
    connect_error="TCP connected.";
    transport_error="TCP connected.";
    return 0;
}
int stream_write(const void *data, unsigned n) {
    const char *p = data;
    unsigned total = 0;
    while (total < n) {
        if (http_expired())
            return -1;
        unsigned chunk = n - total;
        unsigned maximum = tcp.timestamps ? 1448u : TCP_MSS;
        if (chunk > maximum)
            chunk = maximum;
        if (chunk > tcp.peer_mss) chunk=tcp.peer_mss;
        if (tcp.peer_window_valid) {
            unsigned credit =
                (int32_t)(tcp.peer_right - tcp.seq) > 0 ? tcp.peer_right - tcp.seq : 0;
            if (chunk > credit)
                chunk = credit ? credit : 1u;
        }
        uint32_t seq = tcp.seq, next = seq + chunk;
        bool acked = false;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            if (tcp.state != 2)
                return -1;
            tcp.seq = seq;
            tcp.sample_active = attempt == 0;
            tcp.sample_at = frame_clock;
            tcp.sample_end = next;
            send_tcp(0x18, p + total, chunk);
            tcp.seq = next;
            uint32_t start = frame_clock;
            while (frame_clock - start < tcp_rto()) {
                net_poll();
                if (http_expired())
                    return -1;
                if ((int32_t)(tcp.remote_ack - next) >= 0) {
                    acked = true;
                    break;
                }
                if (tcp.state < 0)
                    return -1;
                task_yield();
            }
            if (!acked)
                tcp_backoff();
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
    if (!n)
        return 0;
    uint32_t start = frame_clock, last_ack = frame_clock, probe_wait = tcp_rto();
    if (http_expired())
        return -1;
    while (!tcp.used) {
        net_poll();
        if (http_expired())
            return -1;
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
        if (frame_clock - start > tcp_read_budget()) {
            if (http_operation.active) {
                http_timeouts++;
                transport_error = "HTTP response idle timeout.";
            } else {
                tcp_timeouts++;
                transport_error = "TCP receive wait budget exceeded.";
            }
            return -1;
        }
        if (frame_clock - last_ack >= probe_wait) {
            send_tcp(0x10, NULL, 0);
            last_ack = frame_clock;
            if (probe_wait < TCP_RTO_MAX)
                probe_wait = probe_wait > TCP_RTO_MAX / 2u ? TCP_RTO_MAX : probe_wait * 2u;
        }
        task_yield();
    }
    if (n > tcp.used)
        n = tcp.used;
    tcp_copy_data(out, 0, n);
    tcp.head = (tcp.head + n) % tcp.capacity;
    tcp.used -= n;
    tcp_delivery_sample(n);
    tcp_drain_pending();
    tcp_tune_receive();
    unsigned window = tcp_window() >> tcp.scale_rx;
    unsigned granted = tcp_granted() >> tcp.scale_rx;
    /* Window updates need not consume a Wi-Fi TX frame on every small read.
     * Always reopen a zero window promptly; otherwise batch credit updates
     * while preserving immediate ACKs already pending for received data. */
    unsigned released = window > granted ? window - granted : 0;
    unsigned threshold = tcp.capacity / 8u;
    if (threshold < TCP_WINDOW_UPDATE_MIN) threshold = TCP_WINDOW_UPDATE_MIN;
    if (threshold > TCP_WINDOW_UPDATE_MAX) threshold = TCP_WINDOW_UPDATE_MAX;
    if (tcp.ack_pending || (released && (!granted || released >= threshold)))
        send_tcp(0x10, NULL, 0);
    return n;
}
void stream_close(void) {
    if (tcp.state == 2)
        send_tcp(0x11, 0, 0);
    tcp_release();
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
static int http_response(char *response, unsigned used, net_body *out,
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
        if (!net_body_append(out, body, redirect_body)) {
            append(error, "The redirect response is larger than the browser accepts.", ecap);
            return -1;
        }
        return (int)out->len;
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
    if (!chunked && content_length < 0) {
        /* The answer carries no length at all: the body ends with the close.
         * Naming it in the log separates this case from a truncated reply. */
        serial_write("HTTP: no length header; body delimited by the connection close\n");
    }
    if ((secure && secure_error == 31) || (!secure && n < 0)) {
        /* RFC 7230 section 3.3.3: a response with neither Content-Length nor
         * chunked framing is delimited by the connection close, so the bytes
         * already read are the whole answer. Rejecting them only hides a legal
         * HTTP/1.0 reply. A declared length the peer never delivered is still
         * a truncated response and stays rejected. */
        if (!chunked && content_length >= 0 && (unsigned)content_length > available) {
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
            if (!net_body_append(out, body + pos, size)) {
                append(error, out->owned
                               ? "The document is larger than the browser accepts."
                               : "Chunked document exceeds the destination size limit.",
                       ecap);
                return -1;
            }
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
        if (!net_body_append(out, body, written)) {
            append(error, out->owned
                               ? "The document is larger than the browser accepts."
                               : "Document exceeds the destination size limit.",
                       ecap);
            return -1;
        }
    }
    if (!net_body_reserve(out, written)) {
        append(error, "The document could not be stored.", ecap);
        return -1;
    }
    out->data[written] = 0;
    out->len = written;
    return (int)written;
}

static int http_open(const char *url, bool *secured, char *error, size_t ecap) {
    error[0] = 0;
    bool secure = !strncmp(url, "https://", 8);
    *secured = secure;
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
    stream_set_read_policy(HTTP_IDLE_TIMEOUT_MS, HTTP_IDLE_TIMEOUT_MS);
    if (secure && !tls_connect(host, error, ecap)) {
        stream_close();
        return -1;
    }
    char request[4608] = "GET ";
    append(request, path, sizeof(request));
    /* HTTP/1.1 is required by several front ends; HTTP/1.0 invites a close
     * delimited body that leaves the response without a length. */
    append(request, " HTTP/1.1\r\nHost: ", sizeof(request));
    append(request, host, sizeof(request));
    if (port != (secure ? 443u : 80u)) {
        char p[16];
        number(p, port);
        append(request, ":", sizeof(request));
        append(request, p, sizeof(request));
    }
    append(
        request,
        "\r\nUser-Agent: LunaBrowser/0.3\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
        "Accept-Language: pt-BR,pt;q=0.9,en;q=0.8\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n",
        sizeof(request));
    if ((secure ? tls_write(request, strlen(request)) : stream_write(request, strlen(request))) <
        0) {
        append(error,
               secure ? "TLS handshake / certificate validation failed." : "HTTP request failed.",
               ecap);
        stream_close();
        return -1;
    }
    return 0;
}

static int http_once(const char *url, net_body *out, char *error, size_t ecap,
                     int *status_out, char *location, size_t locap) {
    out->len = 0;
    if (out->data)
        out->data[0] = 0;
    *status_out = 0;
    location[0] = 0;
    bool secure;
    if (http_open(url, &secure, error, ecap) < 0)
        return -1;
    size_t capacity = WEB_CAP + 8192u;
    size_t ceiling = NET_BODY_MAX + 65536u;
    char *response = heap_allocate(capacity);
    if (!response) {
        stream_close();
        append(error, "Insufficient memory for HTTP response.", ecap);
        return -1;
    }
    unsigned used = 0;
    int n = 0;
    bool truncated = false;
    for (;;) {
        if (used + 1u >= capacity) {
            /* The response is larger than the first buffer: grow it instead
             * of rejecting the document for being big. */
            size_t grown = capacity * 2;
            if (grown > ceiling)
                grown = ceiling;
            if (grown <= capacity) {
                truncated = true;
                break;
            }
            char *bigger = heap_allocate(grown);
            if (!bigger) {
                heap_release(response);
                stream_close();
                append(error, "Insufficient memory for HTTP response.", ecap);
                return -1;
            }
            memcpy(bigger, response, used);
            heap_release(response);
            response = bigger;
            capacity = grown;
        }
        n = secure ? tls_read(response + used, capacity - used - 1u)
                   : stream_read(response + used, capacity - used - 1u);
        if (n <= 0)
            break;
        used += (unsigned)n;
    }
    response[used] = 0;
    unsigned secure_error = secure ? tls_error() : 0;
    stream_close();
    int result = -1;
    if (truncated)
        append(error, "The response is larger than the browser accepts.", ecap);
    else
        result = http_response(response, used, out, error, ecap, status_out, location, locap,
                               secure, secure_error, n);
    heap_release(response);
    return result;
}

/* Builds an absolute address from a base document and a reference that may
 * be absolute, protocol relative, rooted or relative to the base directory. */
static bool url_resolve(const char *base, const char *reference, char *out, size_t cap) {
    return net_url_resolve(base,reference,out,cap);
}

static int http_impl(const char *url, net_body *out, char *error, size_t ecap,
                     bool https_only) {
    final_http_url[0]=0;
    char current[4096];
    size_t n = strlen(url);
    if (n >= sizeof(current)) {
        if (out->data)
            out->data[0] = 0;
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
        int result = http_once(current, out, error, ecap, &status, location, sizeof(location));
        if (result < 0)
            return -1;
        if (status < 300 || status >= 400) {
            strcpy(final_http_url,current);
            return result;
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
    fs_abort_write(pid);
    if (network_owner == pid) {
        network_owner = 0;
        tcp_release();
        http_operation.active = false;
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
    http_begin(HTTP_OPERATION_TIMEOUT_MS);
    net_body body = {out, 0, cap, false};
    int result = http_end(http_impl(url, &body, error, ecap, false), &body, error, ecap);
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
    http_begin(HTTP_OPERATION_TIMEOUT_MS);
    net_body body = {out, 0, cap, false};
    int result = http_end(http_impl(url, &body, error, ecap, true), &body, error, ecap);
    network_owner = 0;
    return result;
}

/* Fetches a document into a buffer that grows with it. On success *out owns
 * the bytes and the caller releases them with heap_release. */
int net_http_alloc(const char *url, char **out, size_t *out_len, char *error, size_t ecap) {
    if (!out || !out_len) return -1;
    *out = 0;
    *out_len = 0;
    if (network_owner) {
        error[0] = 0;
        append(error, "Network service busy; try again shortly.", ecap);
        return -1;
    }
    network_owner = task_current()->pid;
    http_begin(HTTP_OPERATION_TIMEOUT_MS);
    net_body body = {0, 0, 0, true};
    int result = http_end(http_impl(url, &body, error, ecap, false), &body, error, ecap);
    network_owner = 0;
    if (result < 0) {
        if (body.data)
            heap_release(body.data);
        return -1;
    }
    *out = body.data;
    *out_len = body.len;
    return result;
}

int net_https_alloc(const char *url, char **out, size_t *out_len, char *error, size_t ecap) {
    if (!out || !out_len) return -1;
    *out = 0;
    *out_len = 0;
    if (!ecap) return -1;
    error[0] = 0;
    if (network_owner) { append(error, "Network service busy.", ecap); return -1; }
    network_owner = task_current()->pid;
    http_begin(HTTP_OPERATION_TIMEOUT_MS);
    net_body body = {0, 0, 0, true};
    int result = http_end(http_impl(url, &body, error, ecap, true), &body, error, ecap);
    network_owner = 0;
    if (result < 0) {
        if (body.data)
            heap_release(body.data);
        return -1;
    }
    *out = body.data;
    *out_len = body.len;
    return result;
}

static HttpInput download_input;
static int download_read(void *context, void *out, unsigned bytes) {
    return (uintptr_t)context ? tls_read(out, bytes) : stream_read(out, bytes);
}
static bool download_sink(void *context, const void *data, size_t bytes) {
    (void)context;
    return fs_write_part(data, bytes);
}
bool net_download_file(const char *url, const char *path, uint64_t *written, char *error,
                       size_t ecap) {
    if (!url || !path || !written || !error || !ecap)
        return false;
    *written = 0;
    error[0] = 0;
    if (network_owner) {
        append(error, "Network service busy.", ecap);
        return false;
    }
    network_owner = task_current()->pid;
    bool result = false;
    char current[4096];
    if (strlen(url) >= sizeof(current)) {
        append(error, "URL too long.", ecap);
        goto done;
    }
    strcpy(current, url);
    /* Existing disk-backed files may take time to copy into the inactive
     * snapshot. Finish that preparation before a TCP peer starts sending. */
    if (!fs_begin_write(path, false, network_owner)) {
        append(error, "Cannot stage download on the writable data volume.", ecap);
        goto done;
    }
    http_begin(0);
    for (unsigned hop = 0; hop < 8; hop++) {
        bool secure;
        if (http_open(current, &secure, error, ecap) < 0)
            goto done;
        download_input = (HttpInput){.read = download_read, .context = (void *)(uintptr_t)secure};
        HttpReply reply;
        if (!http_stream_headers(&download_input, &reply, error, ecap))
            goto done;
        if (reply.status >= 300 && reply.status < 400) {
            stream_close();
            char next[4096];
            if (!reply.location[0] || !url_resolve(current, reply.location, next, sizeof(next))) {
                append(error, "Invalid HTTP redirect.", ecap);
                goto done;
            }
            strcpy(current, next);
            continue;
        }
        if (reply.status < 200 || reply.status >= 300) {
            append(error, "HTTP status rejected.", ecap);
            goto done;
        }
        if (!http_stream_body(&download_input, &reply, download_sink, 0, written, error, ecap))
            goto done;
        if (!reply.chunked && !reply.have_length &&
            ((secure && tls_error()) || (!secure && download_input.last < 0))) {
            append(error, "Transport interrupted; incomplete HTTP document.", ecap);
            goto done;
        }
        if (secure && tls_error() && tls_error() != 31) {
            append(error, "TLS response authentication failed.", ecap);
            goto done;
        }
        stream_close();
        if (!fs_finish_write()) {
            append(error, "Cannot commit download to disk.", ecap);
            goto done;
        }
        strcpy(final_http_url, current);
        result = true;
        goto done;
    }
    append(error, "Too many HTTP redirects.", ecap);
done:
    http_operation.active = false;
    stream_close();
    fs_abort_write(network_owner);
    network_owner = 0;
    if (!result)
        *written = 0;
    return result;
}


/* A single authenticated HTTPS stream for v2 update packages. Redirects are
 * authenticated and every hop must remain HTTPS. No temporary LunaFS file. */
bool net_https_stream(const char *url, NetHttpsSink sink, void *context, uint64_t *written,
                      char *error, size_t ecap) {
    if (!url || !sink || !written || !error || !ecap) return false;
    *written = 0;
    error[0] = 0;
    if (network_owner) {
        append(error, "Network service busy.", ecap);
        return false;
    }
    network_owner = task_current()->pid;
    char current[4096];
    if (strlen(url) >= sizeof(current)) {
        append(error, "Update URL too long.", ecap);
        network_owner = 0;
        return false;
    }
    strcpy(current, url);
    /* Streaming requests have an idle timeout but no total-size deadline. */
    http_begin(0);
    bool result = false;
    for (unsigned hop = 0; hop < 8; ++hop) {
        if (strncmp(current, "https://", 8)) {
            append(error, "HTTPS downgrade refused.", ecap);
            goto done;
        }
        bool secure = false;
        if (http_open(current, &secure, error, ecap) < 0 || !secure)
            goto done;
        tcp_prepare_bulk_receive();
        static HttpInput update_https_input;
        update_https_input = (HttpInput){.read = download_read, .context = (void *)(uintptr_t)1};
        HttpReply reply;
        if (!http_stream_headers(&update_https_input, &reply, error, ecap)) goto done;
        if (reply.status >= 300 && reply.status < 400) {
            stream_close();
            char next[4096];
            if (!reply.location[0] ||
                !url_resolve(current, reply.location, next, sizeof(next))) {
                append(error, "Invalid update redirect.", ecap);
                goto done;
            }
            strcpy(current, next);
            continue;
        }
        if (reply.status != 200) {
            append(error, "HTTP status ", ecap);
            char code[16];
            number(code, reply.status);
            append(error, code, ecap);
            append(error, ".", ecap);
            goto done;
        }
        if (!http_stream_body(&update_https_input, &reply, sink, context, written, error, ecap))
            goto done;
        if (!reply.chunked && !reply.have_length && tls_error() && tls_error() != 31) {
            append(error, "Incomplete authenticated HTTPS body.", ecap);
            goto done;
        }
        if (tls_error() && tls_error() != 31) {
            append(error, "TLS verification failed during package transfer.", ecap);
            goto done;
        }
        result = true;
        goto done;
    }
    append(error, "Too many update redirects.", ecap);
done:
    http_operation.active = false;
    stream_close();
    network_owner = 0;
    if (!result) *written = 0;
    return result;
}
