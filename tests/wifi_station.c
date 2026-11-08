/* Full shared-path integration with a simulated radio. Uses the actual native
 * IPv4 stack, WPA2, DHCP and station controlled-port implementation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/wifi_station.h"
#include "../kernel/net_time.h"
#include "../kernel/task.h"
#include "wpa2_vectors.h"
extern bool net_ready;
void net_poll(void);
bool net_set_dns(const char *);
int stream_connect(const char *,unsigned);
void stream_close(void);
bool random_bytes(void *p,size_t n) { memset(p,42,n);return true; }
volatile uint32_t ticks,frame_clock;
static unsigned sends, installs, clears;
static uint8_t sent[1514], rx[16][1514];
static unsigned sent_len, rx_len[16], head,count;
static bool rx_encrypted[16], ready=true, last_encrypted;
static WifiStation *polled_station;
static bool simulate_lookup;
static unsigned lookup_queries, lookup_drop, lookup_ttl=2, lookup_syns;
static void lookup_reply(const uint8_t *,unsigned);
File *fs_find(const char *path) { (void)path;return NULL; }
Task *task_current(void) { static Task task={.pid=1};return &task; }
void task_yield(void) { ticks++;frame_clock+=10; }
void number(char *out,uint32_t value) { sprintf(out,"%u",value); }
void append(char *out,const char *s,size_t cap) {
    size_t n=strlen(out);if(n<cap) snprintf(out+n,cap-n,"%s",s);
}
void wifi_poll(void) {
    if(polled_station) wifi_station_poll(polled_station,frame_clock);
}
static bool radio_ready(void *ctx) { (void)ctx; return ready; }
static int random_data(void *ctx,uint8_t *out,size_t n) {
    (void)ctx;for(size_t i=0;i<n;i++) out[i]=33+i;return 0;
}
static int transmit(void *ctx,const uint8_t *p,unsigned n,bool encrypted) {
    (void)ctx;assert(n<=sizeof(sent));memcpy(sent,p,n);sent_len=n;
    last_encrypted=encrypted;sends++;
    if(simulate_lookup) lookup_reply(p,n);
    return 0;
}
static int receive(void *ctx,uint8_t *p,unsigned cap,unsigned *n,bool *encrypted) {
    (void)ctx;if(!count) return 0;assert(rx_len[head]<=cap);
    *n=rx_len[head];memcpy(p,rx[head],*n);*encrypted=rx_encrypted[head];
    head=(head+1)%16;count--;return 1;
}
static int install(void *ctx,const Wpa2Keys *keys) {
    (void)ctx;assert(!memcmp(keys->pairwise,ptk_expected+32,16));installs++;return 0;
}
static void clear(void *ctx) { (void)ctx;clears++; }
static void enqueue(const uint8_t *p,unsigned n,bool encrypted) {
    assert(count<16 && n<=1514);unsigned tail=(head+count)%16;
    memcpy(rx[tail],p,n);rx_len[tail]=n;rx_encrypted[tail]=encrypted;count++;
}
static void eapol(const uint8_t *p,unsigned n) {
    uint8_t frame[1514];memcpy(frame,sta_mac,6);memcpy(frame+6,ap_mac,6);
    frame[12]=0x88;frame[13]=0x8e;memcpy(frame+14,p,n);enqueue(frame,n+14,false);
}
static void put16(uint8_t *p,unsigned n) { p[0]=n>>8;p[1]=n; }
static void put32(uint8_t *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
static unsigned sum(const uint8_t *p,unsigned n) {
    unsigned s=0;while(n>1) {s+=(unsigned)p[0]*256+p[1];p+=2;n-=2;}
    if(n) s+=(unsigned)*p<<8;
    while(s>>16) s=(s&65535)+(s>>16);
    return (~s)&65535;
}
static void lookup_reply(const uint8_t *p,unsigned n) {
    if(n<42 || p[12]!=8 || p[13]!=0) return;
    uint8_t reply[1514];memcpy(reply,p,n);
    memcpy(reply,sta_mac,6);memcpy(reply+6,ap_mac,6);
    memcpy(reply+26,p+30,4);memcpy(reply+30,p+26,4);
    if(p[23]==17 && p[36]==0 && p[37]==53 && p[34]==(53000>>8) && p[35]==(53000&255)) {
        lookup_queries++;
        if(lookup_queries<=lookup_drop) return;
        memcpy(reply+34,p+36,2);memcpy(reply+36,p+34,2);
        reply[44]=0x81;reply[45]=0x80;put16(reply+48,1);
        /* Compressed answer name, IN A, configurable TTL. */
        put16(reply+n,0xc00c);put16(reply+n+2,1);put16(reply+n+4,1);
        put32(reply+n+6,lookup_ttl);put16(reply+n+10,4);
        memcpy(reply+n+12,"\x01\x01\x01\x01",4);n+=16;
        put16(reply+38,n-34);put16(reply+40,0);
    } else if(p[23]==1 && p[34]==8) {
        reply[34]=0;put16(reply+36,0);put16(reply+36,sum(reply+34,n-34));
    } else if(p[23]==6 && n==54 && p[47]==2) {
        lookup_syns++;
        memcpy(reply+34,p+36,2);memcpy(reply+36,p+34,2);
        uint32_t seq=(uint32_t)p[38]<<24 | (uint32_t)p[39]<<16 | (uint32_t)p[40]<<8 | p[41];
        put32(reply+38,100);put32(reply+42,seq+1);reply[47]=0x12;
        put16(reply+50,0);
        uint8_t pseudo[32]={0};memcpy(pseudo,reply+26,8);
        pseudo[9]=6;put16(pseudo+10,20);memcpy(pseudo+12,reply+34,20);
        put16(reply+50,sum(pseudo,sizeof(pseudo)));
    } else return;
    put16(reply+16,n-14);put16(reply+24,0);put16(reply+24,sum(reply+14,20));
    enqueue(reply,n,true);
}
static void dhcp_reply(uint8_t type,uint32_t xid) {
    uint8_t frame[342]={0};memset(frame,255,6);memcpy(frame+6,ap_mac,6);
    put16(frame+12,0x800);uint8_t *ip=frame+14;
    ip[0]=0x45;put16(ip+2,328);ip[8]=64;ip[9]=17;
    memcpy(ip+12,"\xc0\xa8\x32\x01",4);memset(ip+16,255,4);put16(ip+10,sum(ip,20));
    uint8_t *udp=ip+20;put16(udp,67);put16(udp+2,68);put16(udp+4,308);
    uint8_t *p=udp+8;p[0]=2;p[1]=1;p[2]=6;put32(p+4,xid);memcpy(p+28,sta_mac,6);
    memcpy(p+16,"\xc0\xa8\x32\x09",4);put32(p+236,0x63825363);unsigned n=240;
    p[n++]=53;p[n++]=1;p[n++]=type;
    p[n++]=54;p[n++]=4;memcpy(p+n,"\xc0\xa8\x32\x01",4);n+=4;
    p[n++]=1;p[n++]=4;memcpy(p+n,"\xff\xff\xff\0",4);n+=4;
    p[n++]=3;p[n++]=4;memcpy(p+n,"\xc0\xa8\x32\x01",4);n+=4;
    p[n++]=6;p[n++]=4;memcpy(p+n,"\x01\x01\x01\x01",4);n+=4;
    p[n++]=51;p[n++]=4;put32(p+n,80);n+=4;p[n++]=255;
    enqueue(frame,sizeof(frame),true);
}
static void arp(uint8_t *p) {
    memset(p,0,42);memset(p,255,6);memcpy(p+6,ap_mac,6);put16(p+12,0x806);
    put16(p+14,1);put16(p+16,0x800);p[18]=6;p[19]=4;put16(p+20,1);
    memcpy(p+22,ap_mac,6);memcpy(p+28,"\xc0\xa8\x32\x01",4);
    memcpy(p+38,"\xc0\xa8\x32\x09",4);
}
int main(void) {
    WifiStation s={0};WifiAp ap={.ssid="IEEE",.ssid_len=4,.channel=36,.security=WIFI_WPA2_PSK};
    memcpy(ap.bssid,ap_mac,6);
    WifiStationRadio radio={0,radio_ready,random_data,transmit,receive,install,clear};
    assert(!wifi_station_associated(&s,&radio,sta_mac,&ap,"password",rsn_ie,sizeof(rsn_ie),0));
    uint8_t a[42];arp(a);enqueue(a,sizeof(a),false);
    wifi_station_poll(&s,1);assert(!s.attached && !sends && !net_ready);
    eapol(m1,sizeof(m1));wifi_station_poll(&s,2);
    assert(sends==1 && !last_encrypted && sent_len==14+sizeof(m2_expected));
    assert(!memcmp(sent+14,m2_expected,sizeof(m2_expected)));
    eapol(m3,sizeof(m3));wifi_station_poll(&s,3);
    assert(installs==1 && s.attached && s.link==WIFI_ASSOCIATED && !net_ready);
    assert(last_encrypted && sent[12]==8 && sent[13]==0); /* DHCP discover */
    assert(sent[42+242]==1);
    uint32_t xid=(uint32_t)sent[46]<<24 | (uint32_t)sent[47]<<16 | (uint32_t)sent[48]<<8 | sent[49];
    dhcp_reply(2,xid);wifi_station_poll(&s,4);frame_clock=4;net_poll();
    assert(!net_ready && sent[42+242]==3); /* request, still no configured IP */
    dhcp_reply(5,xid);wifi_station_poll(&s,5);frame_clock=5;net_poll();wifi_station_poll(&s,6);
    assert(net_ready && s.link==WIFI_ONLINE);
    /* A burst larger than the eight-frame IP queue must remain at the radio
     * until net_poll drains it, rather than being decrypted then discarded. */
    unsigned burst_before=sends;
    for(unsigned i=0;i<12;i++) enqueue(a,sizeof(a),true);
    wifi_station_poll(&s,6);
    assert(s.count==8 && count==4);
    net_poll();
    assert(sends==burst_before+8);
    wifi_station_poll(&s,6);net_poll();
    assert(!s.count && !count && sends==burst_before+12);
    unsigned before=sends;enqueue(a,sizeof(a),false);wifi_station_poll(&s,7);net_poll();
    assert(sends==before); /* plaintext injection after authorization blocked */
    /* A synchronous network caller must advance radio RX without a GUI tick. */
    polled_station=&s;
    enqueue(a,sizeof(a),true);frame_clock=8;net_poll();
    assert(sends==before+1 && last_encrypted && sent[20]==0 && sent[21]==2);
    assert(!memcmp(sent,ap_mac,6) && !memcmp(sent+28,"\xc0\xa8\x32\x09",4));
    uint8_t echo[42]={0};memcpy(echo,sta_mac,6);memcpy(echo+6,ap_mac,6);put16(echo+12,0x800);
    uint8_t *ip=echo+14;ip[0]=0x45;put16(ip+2,28);ip[8]=64;ip[9]=1;
    memcpy(ip+12,"\xc0\xa8\x32\x14",4);memcpy(ip+16,"\xc0\xa8\x32\x09",4);
    put16(ip+10,sum(ip,20));ip[20]=8;put16(ip+22,sum(ip+20,8));
    enqueue(echo,sizeof(echo),true);wifi_station_poll(&s,9);net_poll();
    assert(sent_len==42 && sent[12]==8 && sent[13]==6);
    assert(!memcmp(sent+38,"\xc0\xa8\x32\x14",4)); /* ARP directly to peer */
    const uint8_t peer[6]={2,0,0,0,0,3};
    memcpy(a,sta_mac,6);memcpy(a+6,peer,6);put16(a+20,2);memcpy(a+22,peer,6);
    memcpy(a+28,"\xc0\xa8\x32\x14",4);memcpy(a+32,sta_mac,6);
    enqueue(a,sizeof(a),true);wifi_station_poll(&s,9);net_poll();
    assert(!memcmp(sent,peer,6));
    assert(sent[34]==0 && !sum(sent+14,20) && !sum(sent+34,8));
    assert(!memcmp(sent+26,"\xc0\xa8\x32\x09",4) && last_encrypted);
    /* Manual DNS survives DHCP configuration refreshes in net_poll(). */
    assert(!net_set_dns("256.1.1.1") && !net_set_dns("0.0.0.0"));
    assert(!net_set_dns("127.0.0.1") && !net_set_dns("224.0.0.1"));
    assert(!net_set_dns("1.1.1.1junk"));
    assert(net_set_dns("8.8.8.8"));
    net_poll();
    uint8_t request[48]={0};
    assert(!net_time_begin("time.example",123,request));
    assert(sent[23]==17 && sent[36]==0 && sent[37]==53);
    assert(!memcmp(sent+30,"\x08\x08\x08\x08",4));
    net_time_cancel();
    simulate_lookup=true;lookup_drop=2;
    char result[256];
    assert(net_ping("python.org",result,sizeof(result)));
    assert(lookup_queries==3); /* first two UDP queries lost */
    assert(net_ping("python.org",result,sizeof(result)));
    assert(lookup_queries==3); /* reuse the answer while TTL is valid */
    assert(!stream_connect("python.org",80));
    assert(lookup_queries==3 && lookup_syns==1); /* browser reuses ping DNS */
    stream_close();
    frame_clock+=2000;
    assert(net_ping("python.org",result,sizeof(result)));
    assert(lookup_queries==4); /* expired answers are queried again */
    assert(net_set_dns("1.0.0.1"));
    assert(net_ping("python.org",result,sizeof(result)));
    assert(lookup_queries==5); /* changing DNS invalidates the cache */
    lookup_ttl=0;
    assert(net_ping("zero.example",result,sizeof(result)));
    assert(net_ping("zero.example",result,sizeof(result)));
    assert(lookup_queries==7); /* TTL zero must never be cached */
    simulate_lookup=false;
    assert(net_set_dns("auto"));net_poll();
    assert(!net_time_begin("time.example",123,request));
    assert(!memcmp(sent+30,"\x01\x01\x01\x01",4));
    net_time_cancel();
    eapol(m3,sizeof(m3));wifi_station_poll(&s,10);
    assert(installs==1 && net_ready && s.link==WIFI_ONLINE); /* no key/DHCP reset */
    ready=false;wifi_station_poll(&s,11);assert(!net_ready && s.link==WIFI_DOWN);
    wifi_station_disconnect(&s);
    polled_station=0;
    puts("PASS: simulated radio -> real WPA2 -> controlled port -> DHCP -> native ARP/IPv4/ICMP -> encrypted TX; disconnect invalidates IP");
}
