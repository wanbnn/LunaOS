#ifndef LUNA_RTW8822BE_PHY_H
#define LUNA_RTW8822BE_PHY_H
#include "rtw8822b_native.h"
/* Native state needed by the selected upstream 20-MHz receive path. */
struct rtw_hal {
    uint8_t cut_version, rf_type, rf_path_num, antenna_tx, antenna_rx, current_channel;
};
struct rtw_efuse { uint8_t rfe_option, crystal_cap, channel_plan; uint8_t mac[6]; };
struct rtw_dev {
    RtwRegisterIo *io;
    struct rtw_hal hal;
    struct rtw_efuse efuse;
    bool mp_mode;
    int error;
};
int rtw8822be_phy_prepare(struct rtw_dev *, RtwRegisterIo *);
int rtw8822be_phy_channel(struct rtw_dev *, uint8_t channel);
#endif
