#include "../kernel/netif.h"
#include <string.h>
static unsigned get16(const uint8_t *p) { return (unsigned)p[0]*256+p[1]; }
static void put16(uint8_t *p,unsigned n) { p[0]=n>>8; p[1]=n; }
static uint32_t accum(const uint8_t *p,unsigned n,uint32_t sum) {
    while(n>1) { sum+=get16(p); p+=2; n-=2; }
    if(n) sum+=(unsigned)*p<<8;
    while(sum>>16) sum=(sum&65535)+(sum>>16);
    return sum;
}
static unsigned checksum(const uint8_t *p,unsigned n) { return (~accum(p,n,0))&65535; }
static int dhcp_send(void *ctx,const uint8_t src[4],const uint8_t dest[4],
                       const uint8_t *dest_mac,const uint8_t *payload,size_t n) {
    NetInterface *nic=ctx;
    if(n>1472 || !nic->ops->link_up(nic->context)) return -100;
    uint8_t p[1514]={0};
    if(dest_mac) memcpy(p,dest_mac,6); else memset(p,255,6);
    memcpy(p+6,nic->mac,6); put16(p+12,0x800);
    uint8_t *ip=p+14; ip[0]=0x45; put16(ip+2,28+n); ip[8]=64; ip[9]=17;
    memcpy(ip+12,src,4); memcpy(ip+16,dest,4); put16(ip+10,checksum(ip,20));
    uint8_t *udp=ip+20; put16(udp,68); put16(udp+2,67); put16(udp+4,8+n);
    memcpy(udp+8,payload,n);
    uint32_t sum=accum(ip+12,8,17+8+n); unsigned check=(~accum(udp,8+n,sum))&65535;
    put16(udp+6,check ? check : 65535);
    return nic->ops->transmit(nic->context,p,42+n);
}
int netif_start(NetInterface *nic,const NetDeviceOps *ops,void *ctx,const uint8_t mac[6],
                  uint32_t xid,uint32_t now) {
    if(!nic || !ops || !ops->link_up || !ops->transmit || !ops->receive || !mac || (mac[0]&1))
        return -22;
    if(nic->started) return -16;
    if(!ops->link_up(ctx)) return -100;
    memset(nic,0,sizeof(*nic)); nic->ops=ops; nic->context=ctx; memcpy(nic->mac,mac,6);
    nic->started=true;
    DhcpOps dhcp={nic,dhcp_send}; int rc=dhcp_start(&nic->dhcp,&dhcp,mac,xid,now);
    if(rc) { nic->started=false; nic->error=rc; }
    return rc;
}
void netif_stop(NetInterface *nic) { if(nic) memset(nic,0,sizeof(*nic)); }
int netif_transmit(NetInterface *nic,const uint8_t *p,unsigned n) {
    if(!nic || !p || n<14 || n>1514) return -22;
    if(!nic->started || !nic->dhcp.configured || !nic->ops->link_up(nic->context)) return -100;
    if(memcmp(p+6,nic->mac,6)) return -13;
    return nic->ops->transmit(nic->context,p,n);
}
/* Return true for any DHCP-directed UDP packet, including malformed ones:
 * DHCP must never leak through to the ordinary IP stack before validation. */
static bool dhcp_input(NetInterface *nic,const uint8_t *p,unsigned n,uint32_t now) {
    if(get16(p+12)!=0x800 || n<34) return false;
    const uint8_t *ip=p+14; unsigned ih=(ip[0]&15)*4;
    if((ip[0]>>4)!=4 || ip[9]!=17 || ih<20 || ih>n-14 || n-14-ih<8) return false;
    const uint8_t *udp=ip+ih; if(get16(udp+2)!=68) return false;
    unsigned total=get16(ip+2), len=get16(udp+4);
    if(get16(udp)!=67 || total<ih+8 || total>n-14 || (get16(ip+6)&0x3fff) ||
       checksum(ip,ih) || len<8 || len!=total-ih) return true;
    const uint8_t broadcast[4]={255,255,255,255};
    if(memcmp(ip+16,broadcast,4) && memcmp(ip+16,nic->dhcp.offered,4) &&
       (!nic->dhcp.configured || memcmp(ip+16,nic->dhcp.config.address,4))) return true;
    if(get16(udp+6)) {
        uint32_t sum=accum(ip+12,8,17+len);
        if(accum(udp,len,sum)!=65535) return true;
    }
    dhcp_receive(&nic->dhcp,p+6,udp+8,len-8,now); return true;
}
void netif_poll(NetInterface *nic,uint32_t now,void (*input)(void *,uint8_t *,unsigned),void *ctx) {
    if(!nic || !nic->started) return;
    if(!nic->ops->link_up(nic->context)) {
        dhcp_stop(&nic->dhcp); nic->error=-100; return;
    }
    dhcp_poll(&nic->dhcp,now); /* Expire leases before delivering any IP. */
    uint8_t p[1514]; const uint8_t broadcast[6]={255,255,255,255,255,255};
    for(unsigned budget=0;budget<32;budget++) {
        unsigned n=0; int rc=nic->ops->receive(nic->context,p,sizeof(p),&n);
        if(rc<0) { dhcp_stop(&nic->dhcp); nic->error=rc; return; }
        if(!rc) break;
        if(rc!=1 || n>sizeof(p)) { dhcp_stop(&nic->dhcp); nic->error=-22; return; }
        if(n<14 || (memcmp(p,nic->mac,6) && memcmp(p,broadcast,6))) continue;
        if(dhcp_input(nic,p,n,now)) continue;
        /* EAPOL belongs to the supplicant, never to the IPv4 stack. */
        if(nic->dhcp.configured && input && (get16(p+12)==0x800 || get16(p+12)==0x806))
            input(ctx,p,n);
    }
    dhcp_poll(&nic->dhcp,now);
    if(nic->dhcp.error) nic->error=nic->dhcp.error;
}
