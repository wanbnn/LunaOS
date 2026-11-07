/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018-2019 Realtek Corporation
 * LunaOS adaptation of rtw88 PHY table loading, efuse and RTL8822B channels.
 */
#include "rtw8822be_phy.h"
#include "driver.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint32_t __le32;
#define BIT(n) (1u << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MASKBYTE0 0xffu
#define MASKBYTE1 0xff00u
#define MASKBYTE2 0xff0000u
#define MASKBYTE3 0xff000000u
#define MASKLWORD 0xffffu
#define MASKHWORD 0xffff0000u
#define MASKDWORD UINT32_MAX
#define RFREG_MASK 0xfffffu
#define RF_PATH_A 0
#define RF_PATH_B 1
#define RF_1T1R 0
#define RF_2T2R 2
#define BB_PATH_A 1
#define BB_PATH_B 2
#define BB_PATH_AB 3
#define RTW_CHANNEL_WIDTH_20 0
#define RTW_CHANNEL_WIDTH_40 1
#define RTW_CHANNEL_WIDTH_80 2
#define RTW_CHANNEL_WIDTH_5 3
#define RTW_CHANNEL_WIDTH_10 4
#define RTW_SC_20_UPPER 1
#define RTW_CHIP_VER_CUT_B 1
enum rtw_rfe_fem { RTW_RFE_IFEM, RTW_RFE_EFEM, RTW_RFE_IFEM2G_EFEM5G };
#define BUILD_BUG_ON(c) _Static_assert(!(c), "invalid register range")
#define IS_CH_2G_BAND(c) ((c)<=14)
#define IS_CH_5G_BAND_1(c) ((c)>=36 && (c)<=48)
#define IS_CH_5G_BAND_2(c) ((c)>=52 && (c)<=64)
#define IS_CH_5G_BAND_3(c) ((c)>=100 && (c)<=144)
#define IS_CH_5G_BAND_4(c) ((c)>=149 && (c)<=177)
#define IS_CH_5G_BAND_MID(c) (IS_CH_5G_BAND_2(c)||IS_CH_5G_BAND_3(c))
#define IS_CH_5G_BAND(c) (IS_CH_5G_BAND_1(c)||IS_CH_5G_BAND_2(c)||IS_CH_5G_BAND_3(c)||IS_CH_5G_BAND_4(c))
#define WARN(cond,...) ((cond) ? (rtwdev->error=-5,1) : 0)
#include "rtw8822b/reg.h"
static unsigned shift(uint32_t mask) { return __builtin_ctz(mask); }
static u32 read_reg(struct rtw_dev *d,u32 reg,unsigned width) {
    u32 value=0;
    if(!d->error) {
        int rc=d->io->read(d->io->ctx,reg,width,&value);
        if(rc) { d->error=rc; d->io->failed_register=reg; }
    }
    return value;
}
static void write_reg(struct rtw_dev *d,u32 reg,unsigned width,u32 value) {
    if(!d->error) {
        d->io->writes_started=true;
        int rc=d->io->write(d->io->ctx,reg,width,value);
        if(rc) { d->error=rc; d->io->failed_register=reg; }
    }
}
static u32 rtw_read32_mask(struct rtw_dev *d,u32 reg,u32 mask) {
    return (read_reg(d,reg,4)&mask)>>shift(mask);
}
static void rtw_write32_mask(struct rtw_dev *d,u32 reg,u32 mask,u32 value) {
    u32 old=read_reg(d,reg,4);
    write_reg(d,reg,4,(old&~mask)|((value<<shift(mask))&mask));
}
static void rtw_write32_set(struct rtw_dev *d,u32 r,u32 v) { rtw_write32_mask(d,r,v,UINT32_MAX); }
static void rtw_write32_clr(struct rtw_dev *d,u32 r,u32 v) { rtw_write32_mask(d,r,v,0); }
static void delay(struct rtw_dev *d,unsigned us) {
    if(!d->error) { int rc=d->io->delay_us(d->io->ctx,us); if(rc) d->error=rc; }
}
#define udelay(us) delay(rtwdev,us)
static u32 rtw_read_rf(struct rtw_dev *d,unsigned path,u32 reg,u32 mask) {
    return rtw_read32_mask(d,(path ? 0x2c00 : 0x2800)+((reg&255)*4),mask);
}
static void rtw_write_rf(struct rtw_dev *d,unsigned path,u32 reg,u32 mask,u32 value) {
    if(mask!=RFREG_MASK) {
        u32 old=rtw_read_rf(d,path,reg,RFREG_MASK);
        if(old==RFREG_MASK) { d->error=-5; return; }
        value=(old&~mask)|((value<<shift(mask))&mask);
    }
    write_reg(d,path ? 0xe90 : 0xc90,4,((reg&255)<<20)|(value&RFREG_MASK));
    delay(d,13);
}
static void rtw_set_channel_mac(struct rtw_dev *d,u8 channel,u8 bw,u8 primary) {
    (void)bw; (void)primary;
    write_reg(d,0x483,1,0);
    rtw_write32_mask(d,0x668,0x180,0); /* 20 MHz */
    rtw_write32_mask(d,0x454,BIT(7),channel>14);
}
#include "rtw8822b/channel_regs.h"
#include "rtw8822b/channel.h"
#include "rtw8822b/tables.h"
static int efuse(struct rtw_dev *d) {
    uint8_t physical[1024], logical[768];
    memset(logical,255,sizeof(logical));
    rtw_write32_mask(d,0x34,BIT(8)|BIT(9),0);
    u32 old=read_reg(d,0x37,1); write_reg(d,0x37,1,old&~128u);
    for(unsigned i=0;i<sizeof(physical) && !d->error;i++) {
        u32 ctl=read_reg(d,0x30,4);
        write_reg(d,0x30,4,(ctl&~(BIT(31)|0x3ffffu))|(i<<8));
        unsigned retry;
        for(retry=0;retry<10000;retry++) {
            ctl=read_reg(d,0x30,4);
            if(d->error || (ctl&BIT(31))) break;
            delay(d,1);
        }
        if(retry==10000) d->error=-110;
        physical[i]=ctl;
    }
    if(d->error) return d->error;
    write_reg(d,0x37,1,old);
    for(unsigned p=0;p<928;) {
        u8 h=physical[p++]; if(h==255) break;
        unsigned block=h>>4, words=h&15;
        if((h&31)==15) {
            if(p>=928) return -22;
            u8 second=physical[p++]; if(second==255) break;
            block=((second&0xf0)>>1)|((h>>5)&7); words=second&15;
        }
        for(unsigned j=0;j<4;j++) if(!(words&BIT(j))) {
            unsigned offset=block*8+j*2;
            if(p+2>928 || offset+2>sizeof(logical)) return -22;
            logical[offset]=physical[p++]; logical[offset+1]=physical[p++];
        }
    }
    d->efuse.rfe_option=logical[0xca];
    d->efuse.crystal_cap=logical[0xb9]==255 ? 0 : logical[0xb9]&63;
    d->efuse.channel_plan=logical[0xb8];
    if(d->efuse.rfe_option!=2 && d->efuse.rfe_option!=3 && d->efuse.rfe_option!=5)
        return -95; /* No guessed antenna/front-end configuration. */
    return d->error;
}
static bool condition(struct rtw_dev *d,u32 c) {
    unsigned cut=(c>>24)&15, pkg=(c>>12)&15, intf=(c>>8)&15;
    return (!cut || cut==(d->hal.cut_version ? d->hal.cut_version : 15)) &&
        (!pkg || pkg==15) && (!intf || intf==1) && (c&255)==d->efuse.rfe_option;
}
static void load(struct rtw_dev *d,const u32 *table,unsigned words,unsigned kind) {
    bool match=true, skipped=false; u32 cond=0;
    for(unsigned i=0;i+1<words && !d->error;i+=2) {
        u32 addr=table[i], value=table[i+1];
        if(addr&BIT(31)) {
            unsigned branch=(addr>>28)&3;
            if(branch==3) { match=true; skipped=false; }
            else if(branch==2) match=!skipped;
            else cond=addr;
        } else if(addr&BIT(30)) {
            match=!skipped && condition(d,cond);
            if(match) skipped=true;
        } else if(match) {
            if(kind==0) write_reg(d,addr,1,value);
            else if(kind==1) write_reg(d,addr,4,value);
            else if(kind==2) {
                if(addr==0xfe) delay(d,50000);
                else if(addr==0xfd) delay(d,5000);
                else if(addr==0xfc) delay(d,1000);
                else if(addr==0xfb) delay(d,50);
                else if(addr==0xfa) delay(d,5);
                else if(addr==0xf9) delay(d,1);
                else write_reg(d,addr,4,value);
            } else {
                if(addr==0xffe) delay(d,50000);
                else if(addr==0xfe) delay(d,100);
                else { rtw_write_rf(d,kind-3,addr,RFREG_MASK,value); delay(d,1); }
            }
        }
    }
}
int rtw8822be_phy_prepare(struct rtw_dev *d,RtwRegisterIo *io) {
    memset(d,0,sizeof(*d)); d->io=io;
    u32 id=read_reg(d,0xf0,4);
    d->hal.cut_version=(id>>12)&15;
    d->hal.rf_type=(id&BIT(27)) ? RF_2T2R : RF_1T1R;
    d->hal.rf_path_num=(id&BIT(27)) ? 2 : 1;
    d->hal.antenna_rx=d->hal.antenna_tx=d->hal.rf_path_num==2 ? BB_PATH_AB : BB_PATH_A;
    d->hal.current_channel=1;
    int rc=efuse(d); if(rc) return rc;
    u32 v=read_reg(d,2,1); write_reg(d,2,1,v|3);
    v=read_reg(d,0x1f,1); write_reg(d,0x1f,1,v|7);
    rtw_write32_set(d,0xec,7u<<24); rtw_write32_clr(d,REG_RXPSEL,BIT_RX_PSEL_RST);
#define LOAD(name,kind) load(d,rtw8822b_##name,ARRAY_SIZE(rtw8822b_##name),kind)
    LOAD(mac,0); LOAD(bb,2); LOAD(agc,1); LOAD(rf_a,3);
    if(d->hal.rf_path_num>1) LOAD(rf_b,4);
#undef LOAD
    rtw_write32_mask(d,0x24,0x7e000000,d->efuse.crystal_cap);
    rtw_write32_mask(d,0x28,0x7e,d->efuse.crystal_cap);
    rtw_write32_set(d,REG_RXPSEL,BIT_RX_PSEL_RST);
    /* Top-level front-end mux, from rtw8822b_phy_rfe_init. */
    rtw_write32_mask(d,0x64,BIT(29)|BIT(28),3);
    rtw_write32_mask(d,0x4c,BIT(26)|BIT(25),0); rtw_write32_mask(d,0x40,4,1);
    rtw_write32_mask(d,0x1990,0x3f,0x30); rtw_write32_mask(d,0x1990,0xc00,3);
    rtw_write32_mask(d,0x974,0x3f,0x3f); rtw_write32_mask(d,0x974,0xc00,3);
    rtw8822b_config_trx_mode(d,d->hal.antenna_tx,d->hal.antenna_rx,false);
    return d->error;
}
int rtw8822be_phy_channel(struct rtw_dev *d,uint8_t channel) {
    if(d->error) return d->error;
    if(!((channel>=1 && channel<=14) || (channel>=36 && channel<=48 && channel%4==0)))
        return -22;
    if(d->efuse.rfe_option!=2 && d->efuse.rfe_option!=3 && d->efuse.rfe_option!=5) return -95;
    rtw8822b_set_channel(d,channel,RTW_CHANNEL_WIDTH_20,0);
    delay(d,1000);
    u32 rf=rtw_read_rf(d,0,0x18,RFREG_MASK);
    if(!d->error && (rf&255)!=channel) d->error=-5;
    if(!d->error) d->hal.current_channel=channel;
    return d->error;
}
