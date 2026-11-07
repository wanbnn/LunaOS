/* Full shared-path integration with a simulated radio. Uses the actual native
 * IPv4 stack, WPA2, DHCP and station controlled-port implementation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/wifi_station.h"
#include "wpa2_vectors.h"
extern bool net_ready;
void net_poll(void);
volatile uint32_t ticks,frame_clock;
static unsigned sends, installs, clears;
static uint8_t sent[1514], rx[16][1514];
static unsigned sent_len, rx_len[16], head,count;
static bool rx_encrypted[16], ready=true, last_encrypted;
static bool radio_ready(void *ctx) { (void)ctx; return ready; }
static int random_data(void *ctx,uint8_t *out,size_t n) {
    (void)ctx;for(size_t i=0;i<n;i++) out[i]=33+i;return 0;
}
static int transmit(void *ctx,const uint8_t *p,unsigned n,bool encrypted) {
    (void)ctx;assert(n<=sizeof(sent));memcpy(sent,p,n);sent_len=n;
    last_encrypted=encrypted;sends++;return 0;
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
    unsigned before=sends;enqueue(a,sizeof(a),false);wifi_station_poll(&s,7);net_poll();
    assert(sends==before); /* plaintext injection after authorization blocked */
    enqueue(a,sizeof(a),true);wifi_station_poll(&s,8);net_poll();
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
    eapol(m3,sizeof(m3));wifi_station_poll(&s,10);
    assert(installs==1 && net_ready && s.link==WIFI_ONLINE); /* no key/DHCP reset */
    ready=false;wifi_station_poll(&s,11);assert(!net_ready && s.link==WIFI_DOWN);
    wifi_station_disconnect(&s);
    puts("PASS: simulated radio -> real WPA2 -> controlled port -> DHCP -> native ARP/IPv4/ICMP -> encrypted TX; disconnect invalidates IP");
}
