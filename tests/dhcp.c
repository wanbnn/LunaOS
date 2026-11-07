#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../include/net/dhcp.h"
static unsigned sends;
static uint8_t last[512], source[4], destination[4];
static bool unicast;
static int send_packet(void *ctx,const uint8_t src[4],const uint8_t dst[4],const uint8_t *mac,
                         const uint8_t *p,size_t n) {
    (void)ctx; assert(n<=sizeof(last)); memcpy(last,p,n); memcpy(source,src,4);
    memcpy(destination,dst,4); unicast=mac!=0; sends++; return 0;
}
static void put32(uint8_t *p,uint32_t n) { p[0]=n>>24;p[1]=n>>16;p[2]=n>>8;p[3]=n; }
static unsigned reply(uint8_t *p,const DhcpClient *d,uint8_t type) {
    memset(p,0,300); p[0]=2;p[1]=1;p[2]=6;put32(p+4,d->xid); memcpy(p+28,d->mac,6);
    memcpy(p+16,"\xc0\xa8\x32\x09",4); put32(p+236,0x63825363);
    unsigned n=240;
    p[n++]=53;p[n++]=1;p[n++]=type;
    p[n++]=54;p[n++]=4;memcpy(p+n,"\xc0\xa8\x32\x01",4);n+=4;
    p[n++]=1;p[n++]=4;memcpy(p+n,"\xff\xff\xff\x00",4);n+=4;
    p[n++]=3;p[n++]=4;memcpy(p+n,"\xc0\xa8\x32\x01",4);n+=4;
    p[n++]=6;p[n++]=4;memcpy(p+n,"\x01\x01\x01\x01",4);n+=4;
    p[n++]=51;p[n++]=4;put32(p+n,80);n+=4;p[n++]=255;return n;
}
int main(void) {
    const uint8_t mac[6]={2,0,0,0,0,1}, server_mac[6]={2,0,0,0,0,2};
    DhcpClient d={0}; DhcpOps ops={0,send_packet}; uint8_t p[300];
    assert(!dhcp_start(&d,&ops,mac,123,0) && sends==1 && last[242]==1);
    assert(!unicast && source[0]==0 && destination[0]==255);
    unsigned n=reply(p,&d,2); p[4]^=1;
    assert(dhcp_receive(&d,server_mac,p,n,1)==-22 && d.phase==DHCP_SELECTING);p[4]^=1;
    assert(!dhcp_receive(&d,server_mac,p,n,1) && d.phase==DHCP_REQUESTING && sends==2);
    assert(last[242]==3);
    n=reply(p,&d,5);
    for(unsigned bytes=0;bytes<n;bytes++) assert(dhcp_receive(&d,server_mac,p,bytes,2)==-22);
    assert(!dhcp_receive(&d,server_mac,p,n,2) && d.configured && d.phase==DHCP_BOUND);
    assert(d.config.address[3]==9 && d.config.dns[0]==1 && d.lease_ms==80000);
    dhcp_poll(&d,40002); assert(d.phase==DHCP_RENEWING && unicast && source[3]==9);
    assert(!memcmp(last+12,d.config.address,4));
    dhcp_poll(&d,70002); assert(d.phase==DHCP_REBINDING && !unicast);
    dhcp_poll(&d,80002); assert(d.phase==DHCP_FAILED && !d.configured && d.error==-110);
    dhcp_stop(&d); assert(!dhcp_start(&d,&ops,mac,124,UINT32_MAX-100));
    n=reply(p,&d,2);assert(!dhcp_receive(&d,server_mac,p,n,UINT32_MAX-90));
    n=reply(p,&d,5);assert(!dhcp_receive(&d,server_mac,p,n,UINT32_MAX-80));
    dhcp_poll(&d,40000);assert(d.phase==DHCP_RENEWING); /* clock wrap */
    n=reply(p,&d,5);assert(!dhcp_receive(&d,server_mac,p,n,40001) && d.phase==DHCP_BOUND);
    dhcp_stop(&d);assert(!dhcp_start(&d,&ops,mac,125,0));
    n=reply(p,&d,2);assert(!dhcp_receive(&d,server_mac,p,n,1));
    n=reply(p,&d,6);assert(dhcp_receive(&d,server_mac,p,n,2)==-13 && !d.configured);
    dhcp_stop(&d);assert(!dhcp_start(&d,&ops,mac,126,0));
    for(unsigned i=0;i<6;i++) dhcp_poll(&d,100000*(i+1));
    assert(d.phase==DHCP_FAILED && !d.configured);
    puts("PASS: DHCP DORA, identity/bounds checks, renewal/rebind, expiry, NAK, timeout, clock wrap");
}
