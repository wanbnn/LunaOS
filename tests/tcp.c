/* Real native TCP receive/send logic with a deterministic Ethernet transport.
 * This does not emulate a radio or claim physical RF validation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/net.c"

volatile uint32_t ticks,frame_clock;
static uint8_t last_tx[1514];
static unsigned last_bytes, transmitted;
static bool auto_ack,pending_ack;
static unsigned data_sends,drop_sends,data_sizes[8];
static uint32_t data_sequences[8];
static void segment(uint32_t,unsigned,const char *,unsigned);
void wifi_poll(void) {}
void task_yield(void) { ticks++;frame_clock+=10; }
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
    if(pending_ack) {pending_ack=false;segment(tcp.ack,0x10,NULL,0);}
}
static void setup(void) {
    memset(&tcp,0,sizeof(tcp));
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
    last_bytes=transmitted=0;
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
    put16(p+16,sum(p,20+option_bytes+n,pseudo));receive(frame,54+option_bytes+n);
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
    assert(last_bytes==58 && (last_tx[46]>>4)==6);
    assert(last_tx[54]==2 && last_tx[55]==4 && be16(last_tx+56)==1200);
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
    for(unsigned i=0;i<5;i++) segment(101+i,0x18,"x",1);
    segment(100,0x18,"a",1);
    assert(tcp.used==5 && tcp.ack==105); /* four queued, fifth remains unacknowledged */
    segment(105,0x18,"x",1);assert(tcp.used==6 && tcp.ack==106);
    setup();tcp.used=sizeof(tcp.data)-2;
    segment(100,0x18,"ab",2);assert(tcp.ack==100 && tcp.used==sizeof(tcp.data)-2);
    assert(be16(last_tx+48)==1); /* advertise actual remaining receive window */
    puts("PASS: TCP overlap, reordering, duplicate suppression, FIN gaps, sequence wrap, RST validation and MSS");
}
