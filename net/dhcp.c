#include "../include/net/dhcp.h"
#include <string.h>
#define EINVAL (-22)
#define EACCES (-13)
#define ETIMEDOUT (-110)
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static void put32(uint8_t *p, uint32_t n) { p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n; }
static bool ip_valid(const uint8_t *p) {
    return p[0] && p[0]!=127 && p[0]<224 && get32(p)!=UINT32_MAX;
}
static bool mask_valid(const uint8_t *p) {
    uint32_t m=get32(p), inv=~m; return m && (inv & (inv+1))==0;
}
static void clear_config(DhcpClient *d) { memset(&d->config,0,sizeof(d->config)); d->configured=false; }
static int transmit(DhcpClient *d, uint32_t now) {
    uint8_t p[300]={0}, zero[4]={0}, broadcast[4]={255,255,255,255};
    p[0]=1; p[1]=1; p[2]=6; put32(p+4,d->xid);
    bool renewal=d->phase==DHCP_RENEWING || d->phase==DHCP_REBINDING;
    if(renewal) memcpy(p+12,d->config.address,4);
    if(d->phase!=DHCP_RENEWING) p[10]=0x80; /* broadcast replies */
    memcpy(p+28,d->mac,6); put32(p+236,0x63825363);
    unsigned n=240;
    p[n++]=53; p[n++]=1; p[n++]=d->phase==DHCP_SELECTING ? 1 : 3;
    p[n++]=61; p[n++]=7; p[n++]=1; memcpy(p+n,d->mac,6); n+=6;
    if(d->phase==DHCP_REQUESTING) {
        p[n++]=50; p[n++]=4; memcpy(p+n,d->offered,4); n+=4;
        p[n++]=54; p[n++]=4; memcpy(p+n,d->server,4); n+=4;
    }
    p[n++]=55; p[n++]=6; p[n++]=1; p[n++]=3; p[n++]=6; p[n++]=51; p[n++]=58; p[n++]=59;
    p[n++]=255;
    int rc=d->ops.send(d->ops.context,renewal ? d->config.address : zero,
        d->phase==DHCP_RENEWING ? d->server : broadcast,
        d->phase==DHCP_RENEWING ? d->server_mac : 0,p,sizeof(p));
    d->attempts++;
    unsigned delay=1000u << (d->attempts>4 ? 4 : d->attempts-1);
    d->deadline=now+delay;
    return rc;
}
int dhcp_start(DhcpClient *d, const DhcpOps *ops, const uint8_t mac[6], uint32_t xid, uint32_t now) {
    if(!d || !ops || !ops->send || !mac || (mac[0]&1) || !xid) return EINVAL;
    if(d->phase!=DHCP_OFF) return -16;
    memset(d,0,sizeof(*d)); d->ops=*ops; memcpy(d->mac,mac,6); d->xid=xid;
    d->phase=DHCP_SELECTING;
    int rc=transmit(d,now);
    if(rc) { d->error=rc; d->phase=DHCP_FAILED; }
    return rc;
}
void dhcp_stop(DhcpClient *d) { if(d) memset(d,0,sizeof(*d)); }
int dhcp_receive(DhcpClient *d, const uint8_t src[6], const uint8_t *p, size_t n, uint32_t now) {
    if(!d || !src || !p || d->phase==DHCP_OFF || d->phase==DHCP_FAILED) return EINVAL;
    if(n<241 || n>1472 || p[0]!=2 || p[1]!=1 || p[2]!=6 || get32(p+4)!=d->xid ||
       memcmp(p+28,d->mac,6) || get32(p+236)!=0x63825363 || (src[0]&1)) return EINVAL;
    NetIpv4Config cfg={0}; memcpy(cfg.address,p+16,4);
    uint8_t server[4]={0}, type=0; uint32_t lease=0,t1=0,t2=0;
    unsigned seen=0; bool end=false;
    for(size_t i=240;i<n;) {
        unsigned option=p[i++]; if(!option) continue;
        if(option==255) { end=true; break; }
        if(i==n) return EINVAL;
        unsigned len=p[i++]; if(len>n-i) return EINVAL;
        const uint8_t *v=p+i; unsigned flag=0;
        switch(option) {
        case 53: flag=1; if(len!=1) return EINVAL; type=*v; break;
        case 54: flag=2; if(len!=4) return EINVAL; memcpy(server,v,4); break;
        case 1: flag=4; if(len!=4) return EINVAL; memcpy(cfg.mask,v,4); break;
        case 3: flag=8; if(len<4 || len%4) return EINVAL; memcpy(cfg.gateway,v,4); break;
        case 6: flag=16; if(len<4 || len%4) return EINVAL; memcpy(cfg.dns,v,4); break;
        case 51: flag=32; if(len!=4) return EINVAL; lease=get32(v); break;
        case 58: flag=64; if(len!=4) return EINVAL; t1=get32(v); break;
        case 59: flag=128; if(len!=4) return EINVAL; t2=get32(v); break;
        case 52: return -95; /* Overloaded BOOTP fields not parsed. */
        default: break;
        }
        if(flag && (seen&flag)) return EINVAL;
        seen|=flag; i+=len;
    }
    if(!end || !(seen&1) || !(seen&2) || !ip_valid(server)) return EINVAL;
    if(d->phase==DHCP_SELECTING && type==2) {
        if(!ip_valid(cfg.address)) return EINVAL;
        memcpy(d->offered,cfg.address,4); memcpy(d->server,server,4);
        d->phase=DHCP_REQUESTING; d->attempts=0;
        int rc=transmit(d,now);
        if(rc) { d->error=rc; d->phase=DHCP_FAILED; }
        return rc;
    }
    bool requesting=d->phase==DHCP_REQUESTING, renewing=d->phase==DHCP_RENEWING;
    if(!requesting && !renewing && d->phase!=DHCP_REBINDING) return EACCES;
    if((requesting || renewing) && memcmp(server,d->server,4)) return EACCES;
    if(type==6) {
        clear_config(d); d->phase=DHCP_FAILED; d->error=EACCES; return EACCES;
    }
    if(type!=5) return EACCES;
    if(!ip_valid(cfg.address) || (requesting && memcmp(cfg.address,d->offered,4)) ||
       (!requesting && memcmp(cfg.address,d->config.address,4))) return EINVAL;
    if(!requesting) {
        if(!(seen&4)) memcpy(cfg.mask,d->config.mask,4);
        if(!(seen&8)) memcpy(cfg.gateway,d->config.gateway,4);
        if(!(seen&16)) memcpy(cfg.dns,d->config.dns,4);
    }
    /* Limit durations to fit wrap-safe millisecond comparisons (<2^31 ms).
     * A longer lease is used conservatively for 24 hours before renewal. */
    if(!(seen&32) || lease<4 || !mask_valid(cfg.mask) || !ip_valid(cfg.gateway) ||
       !ip_valid(cfg.dns)) return EINVAL;
    if(lease>86400) { lease=86400; t1=t2=0; }
    if(!t1) t1=lease/2;
    if(!t2) t2=lease*7/8;
    if(!t1 || t1>=t2 || t2>=lease) return EINVAL;
    uint32_t mask=get32(cfg.mask), address=get32(cfg.address), router=get32(cfg.gateway);
    if((address&mask)!=(router&mask) || (address&~mask)==0 || (address&~mask)==~mask)
        return EINVAL;
    d->config=cfg; memcpy(d->server,server,4); memcpy(d->server_mac,src,6);
    d->configured=true; d->lease_start=now; d->lease_ms=lease*1000;
    d->t1_ms=t1*1000; d->t2_ms=t2*1000; d->phase=DHCP_BOUND; d->error=0;
    return 0;
}
void dhcp_poll(DhcpClient *d, uint32_t now) {
    if(!d || d->phase==DHCP_OFF || d->phase==DHCP_FAILED) return;
    if(d->configured) {
        uint32_t elapsed=now-d->lease_start;
        if(elapsed>=d->lease_ms) {
            clear_config(d); d->phase=DHCP_FAILED; d->error=ETIMEDOUT; return;
        }
        if(elapsed>=d->t2_ms && d->phase!=DHCP_REBINDING) {
            d->phase=DHCP_REBINDING; d->attempts=0; d->deadline=now;
        } else if(elapsed>=d->t1_ms && d->phase==DHCP_BOUND) {
            d->phase=DHCP_RENEWING; d->attempts=0; d->deadline=now;
        }
    }
    if(d->phase==DHCP_BOUND || (int32_t)(now-d->deadline)<0) return;
    if(!d->configured && d->attempts>=5) {
        d->phase=DHCP_FAILED; d->error=ETIMEDOUT; return;
    }
    int rc=transmit(d,now);
    if(rc) { d->error=rc; if(!d->configured) d->phase=DHCP_FAILED; }
}
