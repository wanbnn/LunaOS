/* Real native TCP receive/send logic with a deterministic Ethernet transport.
 * This does not emulate a radio or claim physical RF validation. */
#include "../drivers/net.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void append(char *out, const char *text, size_t capacity) {
    size_t used = strlen(out), n = strlen(text);
    if (used >= capacity)
        return;
    if (n >= capacity - used)
        n = capacity - used - 1u;
    memcpy(out + used, text, n);
    out[used + n] = 0;
}
static bool fail_allocate;
static size_t live_allocations;
void *heap_allocate_owned(size_t bytes, uint32_t owner) {
    assert(owner == 1);
    if (fail_allocate) {
        fail_allocate = false;
        return NULL;
    }
    void *p = malloc(bytes);
    if (p)
        live_allocations++;
    return p;
}
int heap_release(void *p) {
    if (p) {
        assert(live_allocations);
        live_allocations--;
        free(p);
    }
    return 0;
}
uint32_t memory_mb = 4096;
volatile uint32_t ticks,frame_clock;
static uint8_t last_tx[1514];
static unsigned last_bytes, transmitted;
static bool auto_ack,pending_ack;
static bool flush_each = true;
static uint32_t delayed_at;
static unsigned data_sends,drop_sends,data_sizes[8];
static uint32_t data_sequences[8];
static void segment(uint32_t,unsigned,const char *,unsigned);
void wifi_poll(void) {}
void task_yield(void) { frame_clock++;if(frame_clock%10u==0)ticks++; }
void fs_abort_write(uint32_t owner) {
    (void)owner;
}
File *fs_find(const char *path) { (void)path;return NULL; }
int netif_transmit(NetInterface *nic,const uint8_t *p,unsigned n) {
    (void)nic;assert(n<=sizeof(last_tx));
    memcpy(last_tx,p,n);last_bytes=n;transmitted++;
    if(auto_ack && n>54 && p[23]==6) {
        unsigned header=(p[46]>>4)*4;
        unsigned size=n-34-header;
        if(size) {
            assert(data_sends<8);data_sizes[data_sends]=size;
            data_sequences[data_sends++]=be32(p+38);
            pending_ack=data_sends>drop_sends;
        }
    }
    return 0;
}
void netif_poll(NetInterface *nic,uint32_t now,void (*input)(void *,uint8_t *,unsigned),void *ctx) {
    (void)nic;(void)now;(void)input;(void)ctx;
    if (delayed_at && frame_clock >= delayed_at) {
        delayed_at = 0;
        segment(tcp.ack, 0x18, "late", 4);
    }
    if(pending_ack) {pending_ack=false;segment(tcp.ack,0x10,NULL,0);}
}
static void setup(void) {
    tcp_release();
    http_operation.active = false;
    fail_allocate = false;
    assert(tcp_resize(TCP_UNSCALED_MAX));
    tcp.goal = tcp.capacity;
    tcp.limit = tcp.memory_limit = TCP_UNSCALED_MAX;
    tcp.rto = TCP_RTO_MIN;
    memcpy(local_ip,"\x0a\0\2\x0f",4);memcpy(gateway,"\x0a\0\2\2",4);
    memcpy(netmask,"\xff\xff\xff\0",4);memcpy(tcp.ip,gateway,4);
    memcpy(mac,"\x02\0\0\0\0\1",6);memcpy(gateway_mac,"\x02\0\0\0\0\2",6);
    gateway_known=net_ready=true;peer_known=false;pending_bytes=0;
    external_nic.started=external_nic.dhcp.configured=true;
    memcpy(external_nic.dhcp.config.address,local_ip,4);
    memcpy(external_nic.dhcp.config.gateway,gateway,4);
    memcpy(external_nic.dhcp.config.mask,netmask,4);
    tcp.state=2;tcp.port=40000;tcp.remote_port=80;tcp.ack=100;tcp.seq=tcp.remote_ack=1000;
    tcp.peer_mss=536;auto_ack=pending_ack=false;data_sends=drop_sends=0;
    last_bytes = transmitted = 0;
    delayed_at = 0;
    flush_each = true;
}
static void segment_options(uint32_t seq,unsigned flags,const char *data,unsigned n,
                            const uint8_t *options,unsigned option_bytes) {
    uint8_t frame[1514]={0};assert(n+option_bytes<=1460 && !(option_bytes%4));
    memcpy(frame,mac,6);memcpy(frame+6,gateway_mac,6);put16(frame+12,0x800);
    uint8_t *ip=frame+14,*p=ip+20;
    ip[0]=0x45;put16(ip+2,40+option_bytes+n);ip[8]=64;ip[9]=6;
    memcpy(ip+12,tcp.ip,4);memcpy(ip+16,local_ip,4);put16(ip+10,sum(ip,20,0));
    put16(p,tcp.remote_port);put16(p+2,tcp.port);put32(p+4,seq);put32(p+8,tcp.seq+!!(flags&2));
    p[12]=((20+option_bytes)/4)<<4;p[13]=flags;put16(p+14,65535);
    if(option_bytes) memcpy(p+20,options,option_bytes);
    if(n) memcpy(p+20+option_bytes,data,n);
    uint32_t pseudo=be16(ip+12)+be16(ip+14)+be16(ip+16)+be16(ip+18)+6+20+option_bytes+n;
    put16(p + 16, sum(p, 20 + option_bytes + n, pseudo));
    receive(frame,54+option_bytes+n);
    if(flush_each)tcp_flush_ack();
}
static void segment(uint32_t seq,unsigned flags,const char *data,unsigned n) {
    segment_options(seq,flags,data,n,NULL,0);
}
int main(void) {
    uint8_t wrapped[1400];
    memset(rx, 0, sizeof(rx));
    memset(rx + 65500, 0x78, 36);
    memset(rx, 0x78, sizeof(wrapped) - 36u);
    copy_rx_frame(wrapped, 65500, sizeof(wrapped));
    for (size_t i = 0; i < sizeof(wrapped); i++)
        assert(wrapped[i] == 0x78);
    /* Header at the last aligned word, payload starts at ring byte zero. */
    copy_rx_frame(wrapped, 65536, sizeof(wrapped) - 36u);
    for (size_t i = 0; i < sizeof(wrapped) - 36u; i++)
        assert(wrapped[i] == 0x78);
    setup();segment(100,0x18,"abc",3);segment(101,0x18,"bcDEF",5);
    assert(tcp.ack==106 && tcp.used==6 && !memcmp(tcp.data,"abcDEF",6));
    setup();segment(106,0x18,"ghi",3);segment(103,0x18,"def",3);
    assert(tcp.ack==100 && !tcp.used); /* never expose a gap */
    segment(100,0x18,"abc",3);
    assert(tcp.ack==109 && tcp.used==9 && !memcmp(tcp.data,"abcdefghi",9));
    segment(103,0x18,"def",3);
    assert(tcp.ack==109 && tcp.used==9); /* no duplicated bytes */
    setup();segment(103,0x19,"def",3);assert(tcp.state==2);
    segment(100,0x18,"abc",3);
    assert(tcp.state==3 && tcp.ack==107 && tcp.used==6);
    char out[16];assert(stream_read(out,sizeof(out))==6 && !memcmp(out,"abcdef",6));
    assert(stream_read(out,sizeof(out))==0);
    setup();tcp.ack=UINT32_MAX-2;
    segment(UINT32_MAX-2,0x18,"abc",3);segment(UINT32_MAX,0x18,"cde",3);
    assert(tcp.ack==2 && tcp.used==5 && !memcmp(tcp.data,"abcde",5));
    setup();segment(99,0x14,NULL,0);assert(tcp.state==2); /* stale RST */
    segment(100,0x14,NULL,0);assert(tcp.state==-1);
    setup();tcp.state=1;send_tcp(2,NULL,0);
    assert(last_bytes == 74 && (last_tx[46] >> 4) == 10);
    assert(last_tx[54] == 2 && last_tx[55] == 4 && be16(last_tx + 56) == TCP_MSS);
    assert(!sum(last_tx+14,20,0));
    setup();tcp.state=1;
    const uint8_t mss[]={2,4,0,64};segment_options(99,0x12,NULL,0,mss,sizeof(mss));
    assert(tcp.state==2 && tcp.peer_mss==64 && tcp.ack==100);
    auto_ack=true;drop_sends=1;
    char request[130];memset(request,'X',sizeof(request));
    assert(stream_write(request,sizeof(request))==sizeof(request));
    assert(data_sends==4 && data_sizes[0]==64 && data_sizes[1]==64 && data_sizes[2]==64 && data_sizes[3]==2);
    assert(data_sequences[0]==data_sequences[1] && data_sequences[2]==data_sequences[0]+64);
    setup();
    unsigned budget_packets = tcp.capacity / (sizeof(TcpPending) + 1u);
    for (unsigned i = 0; i < budget_packets + 1u; i++)
        segment(101 + i, 0x18, "x", 1);
    segment(100,0x18,"a",1);
    assert(tcp.used == budget_packets + 1u && tcp.ack == 101u + budget_packets);
    segment(tcp.ack, 0x18, "x", 1);
    assert(tcp.used == budget_packets + 2u);
    setup();
    char payload[1200];
    for (unsigned i = 1; i < 49; i++) {
        memset(payload, (int)i, sizeof(payload));
        segment(100 + i * 1200u, 0x18, payload, sizeof(payload));
    }
    memset(payload, 0, sizeof(payload));
    segment(100, 0x18, payload, sizeof(payload));
    assert(tcp.used == 49u * 1200u && tcp.ack == 100u + 49u * 1200u);
    for (unsigned i = 0; i < 49; i++)
        for (unsigned j = 0; j < 1200; j++)
            assert((uint8_t)tcp.data[i * 1200u + j] == i);
    /* Duplicate detection must search beyond a freed earlier queue slot. */
    setup();
    segment(102, 0x18, "b", 1);
    segment(104, 0x18, "d", 1);
    segment(100, 0x18, "aa", 2);
    segment(104, 0x18, "d", 1);
    unsigned queued = 0;
    for (TcpPending *packet = tcp.pending; packet; packet = packet->next)
        queued++;
    assert(queued == 1);
    /* An entire burst survives a delayed missing leading segment. */
    setup();
    for (unsigned i = 0; i < 48; i++)
        segment(104 + i, 0x18, "x", 1);
    delayed_at = frame_clock + 3000u;
    assert(stream_read(out, sizeof(out)) == sizeof(out));
    assert(!memcmp(out, "latexxxxxxxxxxxx", sizeof(out)) && !delayed_at);
    setup();
    unsigned before = tcp_timeouts;
    assert(stream_read(out, sizeof(out)) < 0 && tcp_timeouts == before + 1u);
    setup();
    tcp.used = tcp.capacity - 1;
    segment(100, 0x18, "ab", 2);
    assert(tcp.ack == 100 && tcp.used == tcp.capacity - 1);
    assert(be16(last_tx+48)==1); /* advertise actual remaining receive window */
    /* Demand grows the buffer before advertising the additional memory. */
    setup();
    assert(tcp_resize(TCP_RECEIVE_MIN));
    tcp.goal = tcp.capacity;
    uint8_t block[1200];
    memset(block, 'g', sizeof(block));
    for (unsigned i = 0; i < 8; i++)
        segment(tcp.ack, 0x18, (const char *)block, sizeof(block));
    assert(tcp.capacity > TCP_RECEIVE_MIN && tcp.goal == tcp.capacity);
    assert(be16(last_tx + 48) <= tcp.capacity - tcp.used);
    /* Failed growth keeps every byte and the previously granted credit. */
    setup();
    assert(tcp_resize(TCP_RECEIVE_MIN));
    tcp.goal = tcp.capacity;
    for (unsigned i = 0; i < 6; i++)
        segment(tcp.ack, 0x18, (const char *)block, sizeof(block));
    fail_allocate = true;
    segment(tcp.ack, 0x18, (const char *)block, sizeof(block));
    assert(tcp.capacity == TCP_RECEIVE_MIN && tcp.used == 7u * sizeof(block));
    for (unsigned i = 0; i < tcp.used; i++)
        assert(tcp.data[i] == 'g');
    setup();
    fail_allocate = true;
    segment(tcp.ack + 1u, 0x18, "b", 1);
    assert(!tcp.pending && !tcp.pending_memory && tcp.ack == 100);
    segment(101, 0x18, "b", 1);
    segment(100, 0x18, "a", 1);
    assert(tcp.used == 2 && !memcmp(tcp.data, "ab", 2));
    /* Shrinking waits for advertised bytes to be consumed, without retraction. */
    setup();
    send_tcp(0x10, NULL, 0);
    uint32_t edge = tcp.right_edge;
    frame_clock += 4000u;
    tcp_tune_receive();
    assert(tcp.goal < tcp.capacity && tcp.capacity == TCP_UNSCALED_MAX);
    send_tcp(0x10, NULL, 0);
    assert(tcp.right_edge == edge);
    char read_buffer[1200];
    for (unsigned i = 0; i < 30; i++) {
        segment(tcp.ack, 0x18, (const char *)block, sizeof(block));
        assert(stream_read(read_buffer, sizeof(read_buffer)) == sizeof(read_buffer));
        assert((int32_t)(tcp.right_edge - edge) >= 0);
        edge = tcp.right_edge;
    }
    assert(tcp.capacity < TCP_UNSCALED_MAX && tcp.used == 0);
    /* RFC 6298 estimation, Karn exclusion and capped exponential backoff. */
    setup();
    tcp.sample_active = true;
    tcp.sample_at = frame_clock;
    tcp.sample_end = tcp.seq;
    frame_clock += 200u;
    tcp_rtt_ack(tcp.seq);
    assert(tcp.srtt == 200 && tcp.rttvar == 100 && tcp.rto == 1000);
    tcp.sample_active = true;
    tcp.sample_at = frame_clock;
    tcp.sample_end = tcp.seq;
    frame_clock += 4000u;
    tcp_rtt_ack(tcp.seq);
    assert(tcp.srtt == 675 && tcp.rttvar == 1025 && tcp.rto == 4775);
    tcp_backoff();
    assert(tcp.rto == 9550 && !tcp.sample_active);
    tcp_rtt_ack(tcp.seq);
    assert(tcp.srtt == 675 && tcp.rto == 9550);
    for (unsigned i = 0; i < 20; i++)
        tcp_backoff();
    assert(tcp.rto == TCP_RTO_MAX);
    assert(tcp_read_budget() == 120000u);
    stream_set_read_policy(200, 500);
    assert(tcp_read_budget() == 500);
    tcp.sample_active = true;
    tcp.sample_at = frame_clock;
    tcp.sample_end = tcp.seq;
    frame_clock += 1;
    uint32_t old_rto = tcp.rto;
    tcp_rtt_ack(tcp.seq);
    assert(tcp.rto >= TCP_RTO_MIN && tcp.rto < old_rto);
    /* Sequence and timer arithmetic both survive wraparound. */
    tcp.measured = false;
    tcp.sample_active = true;
    tcp.sample_at = UINT32_MAX - 49u;
    tcp.sample_end = tcp.seq;
    frame_clock = 50;
    tcp_rtt_ack(tcp.seq);
    assert(tcp.srtt == 100);
    /* Total HTTP deadlines expire even with a large TCP RTO and steady data. */
    setup();
    tcp.rto = TCP_RTO_MAX;
    stream_set_read_policy(60000, 60000);
    http_begin(20);
    for (unsigned i = 0; i < 19; i++) {
        frame_clock++;
        tcp.data[0] = 'x';
        tcp.used = 1;
        assert(stream_read(out, 1) == 1);
    }
    frame_clock++;
    tcp.data[0] = 'x';
    tcp.used = 1;
    assert(stream_read(out, 1) < 0 && tcp.used == 1 && tcp.rto == TCP_RTO_MAX);
    char failure[128];
    assert(http_end(-1, out, failure, sizeof(failure)) < 0);
    assert(strstr(failure, "HTTP operation deadline") && !http_operation.active);
    setup();
    segment(tcp.ack + 1, 0x18, "queued", 6);
    assert(tcp.pending);
    network_owner = 9;
    http_begin(300000);
    net_cancel_owner(9);
    assert(!network_owner && !http_operation.active);
    stream_close();
    assert(!tcp.data && !tcp.pending && !live_allocations);
    /* Scaled negotiation, unscaled SYN windows and bounded peer scales. */
    setup();
    memory_mb = 4096;
    tcp_memory_profile();
    assert(tcp.memory_limit == TCP_RECEIVE_MAX && tcp.scale_offer == 14);
    tcp.state = 1;
    send_tcp(2, NULL, 0);
    assert(be16(last_tx + 48) == TCP_UNSCALED_MAX && last_tx[61] == 14);
    const uint8_t high_options[] = {2, 4,  5, 180, 1, 3,   3, 20, 1, 1,
                                    8, 10, 0, 0,   0, 100, 0, 0,  0, 0};
    segment_options(99, 0x12, NULL, 0, high_options, sizeof(high_options));
    assert(tcp.state == 2 && tcp.scaled && tcp.scale_rx == 14 && tcp.scale_tx == 14);
    assert(tcp.limit == TCP_RECEIVE_MAX && tcp.timestamps);
    assert(tcp.peer_right - tcp.seq == 65535u); /* SYN window is never scaled. */
    uint8_t timestamp_options[] = {1, 1, 8, 10, 0, 0, 0, 101, 0, 0, 0, 0};
    segment_options(tcp.ack, 0x10, NULL, 0, timestamp_options, sizeof(timestamp_options));
    assert(tcp.peer_right - tcp.seq == (65535u << 14));
    uint32_t old_ack = tcp.ack;
    unsigned drops = tcp_paws_drops;
    timestamp_options[7] = 99;
    segment_options(tcp.ack, 0x18, "x", 1, timestamp_options, sizeof(timestamp_options));
    assert(tcp.ack == old_ack && tcp_paws_drops == drops + 1u);
    timestamp_options[7] = 102;
    segment_options(tcp.ack, 0x18, "x", 1, timestamp_options, sizeof(timestamp_options));
    assert(tcp.ack == old_ack + 1u && be32(last_tx + 62) == 102u);
    tcp.ts_recent = UINT32_MAX - 50u;
    timestamp_options[7] = 25;
    segment_options(tcp.ack, 0x18, "y", 1, timestamp_options, sizeof(timestamp_options));
    assert(tcp.ack == old_ack + 2u && tcp.ts_recent == 25);
    /* A peer omitting WS keeps the normal 16-bit receive limit. */
    setup();
    tcp_memory_profile();
    tcp.state = 1;
    segment(99, 0x12, NULL, 0);
    assert(!tcp.scaled && !tcp.timestamps && tcp.limit == TCP_UNSCALED_MAX);
    /* Large buffers are allocated only on demand and survive circular wrap. */
    setup();
    tcp.limit = 8u * 1024 * 1024;
    tcp.scale_rx = 8;
    tcp_grow(2600000u);
    assert(tcp.capacity >= 2600000u && tcp.capacity <= tcp.limit);
    send_tcp(0x10, NULL, 0);
    assert(be16(last_tx + 48) > 255u && tcp_granted() <= tcp.capacity);
    setup();
    assert(tcp_resize(TCP_RECEIVE_MIN));
    tcp.goal = tcp.capacity;
    tcp.head = tcp.capacity - 10u;
    const char *wrapped_text = "abcdefghijklmnopqrst";
    segment(tcp.ack, 0x18, wrapped_text, 20);
    char wrapped_out[32];
    assert(stream_read(wrapped_out, 20) == 20 && !memcmp(wrapped_out, wrapped_text, 20));
    assert(tcp.head == 10u);
    segment(tcp.ack, 0x18, wrapped_text, 20);
    assert(tcp_resize(32768u));
    assert(stream_read(wrapped_out, 20) == 20 && !memcmp(wrapped_out, wrapped_text, 20));
    /* A receive burst emits one cumulative ACK at the poll boundary. */
    setup();
    flush_each = false;
    for (unsigned i = 0; i < 10; i++)
        segment(tcp.ack, 0x18, "x", 1);
    assert(!transmitted && tcp.ack_pending);
    tcp_flush_ack();
    assert(transmitted == 1);
    /* Delivery-rate/RTT adaptation can open a multi-megabyte window. */
    setup();
    tcp.limit = 16u * 1024 * 1024;
    tcp.scale_rx = 9;
    tcp.measured = true;
    tcp.srtt = 30;
    tcp.rate_at = frame_clock;
    tcp.rate_bytes = 2600000u;
    frame_clock += 30u;
    tcp_delivery_sample(0);
    assert(tcp.capacity >= 5200000u);
    /* 10 Gbps at 30 ms needs 37.5 MB in flight, with headroom for delivery. */
    tcp.limit = TCP_RECEIVE_MAX;
    tcp.rate_at = frame_clock;
    tcp.rate_bytes = 37500000u;
    frame_clock += 30u;
    tcp_delivery_sample(0);
    assert(tcp.capacity >= 75000000u && tcp.capacity <= tcp.limit);
    stream_close();
    assert(!live_allocations);
    puts("PASS: TCP overlap, reordering, duplicate suppression, FIN gaps, sequence wrap, RST "
         "validation, MSS, adaptive buffers, memory pressure and RTT timers");
}
