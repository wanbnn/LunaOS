/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018-2019 Realtek Corporation
 * Adapted from Linux rtw88 at d48723711bae9b8cdc56719c448ba42fef28cbee.
 * See assets/licenses/Realtek-rtw88-BSD.txt.
 */
static inline void
_rtw_write32s_mask(struct rtw_dev *rtwdev, u32 addr, u32 mask, u32 data)
{
	/* 0xC00-0xCFF and 0xE00-0xEFF have the same layout */
	rtw_write32_mask(rtwdev, addr, mask, data);
	rtw_write32_mask(rtwdev, addr + 0x200, mask, data);
}

#define rtw_write32s_mask(rtwdev, addr, mask, data)			       \
	do {								       \
		BUILD_BUG_ON((addr) < 0xC00 || (addr) >= 0xD00);	       \
									       \
		_rtw_write32s_mask(rtwdev, addr, mask, data);		       \
	} while (0)

/* phy status page0 */
#define GET_PHY_STAT_P0_PWDB(phy_stat)                                         \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x00), GENMASK(15, 8))

/* phy status page1 */
#define GET_PHY_STAT_P1_PWDB_A(phy_stat)                                       \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x00), GENMASK(15, 8))
#define GET_PHY_STAT_P1_PWDB_B(phy_stat)                                       \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x00), GENMASK(23, 16))
#define GET_PHY_STAT_P1_RF_MODE(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x03), GENMASK(29, 28))
#define GET_PHY_STAT_P1_L_RXSC(phy_stat)                                       \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), GENMASK(11, 8))
#define GET_PHY_STAT_P1_HT_RXSC(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x01), GENMASK(15, 12))
#define GET_PHY_STAT_P1_RXEVM_A(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x04), GENMASK(7, 0))
#define GET_PHY_STAT_P1_RXEVM_B(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x04), GENMASK(15, 8))
#define GET_PHY_STAT_P1_CFO_TAIL_A(phy_stat)                                 \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x05), GENMASK(7, 0))
#define GET_PHY_STAT_P1_CFO_TAIL_B(phy_stat)                                 \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x05), GENMASK(15, 8))
#define GET_PHY_STAT_P1_RXSNR_A(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x06), GENMASK(7, 0))
#define GET_PHY_STAT_P1_RXSNR_B(phy_stat)                                      \
	le32_get_bits(*((__le32 *)(phy_stat) + 0x06), GENMASK(15, 8))

#define RTW8822B_EDCCA_MAX	0x7f
#define RTW8822B_EDCCA_SRC_DEF	1
#define REG_HTSTFWT	0x800
#define REG_RXCCAMSK	0x814
#define REG_L1WT	0x83c
#define REG_L1PKWT	0x840
#define REG_MRC		0x850
#define REG_EDCCA_POW_MA	0x8a0
#define BIT_MA_LEVEL	GENMASK(1, 0)
#define REG_ADC40	0x8c8
#define REG_EDCCA_DECISION	0x8dc
#define BIT_EDCCA_OPTION	BIT(5)
#define REG_CDDTXP	0x93c
#define REG_TXPSEL1	0x940
#define REG_EDCCA_SOURCE	0x944
#define BIT_SOURCE_OPTION	GENMASK(29, 28)
#define REG_ACBB0	0x948
#define REG_ACBBRXFIR	0x94c
#define REG_ACGG2TBL	0x958
#define REG_ADCINI	0xa04
#define REG_TXSF2	0xa24
#define REG_TXSF6	0xa28
#define REG_RXDESC	0xa2c
#define REG_ENTXCCK	0xa80
#define REG_AGCTR_A	0xc08
#define REG_TXDFIR	0xc20
#define REG_TRSW	0xca0
#define REG_RFESEL0	0xcb0
#define REG_RFESEL8	0xcb4
#define REG_RFECTL	0xcb8
#define REG_RFEINV	0xcbc
#define REG_AGCTR_B	0xe08
#define REG_ANTWT	0x1904
#define REG_IQKFAILMSK	0x1bf0

extern const struct rtw_chip_info rtw8822b_hw_spec;

