/*
 * Driver for the ALLO KATANA CODEC
 *
 * Author: Jaikumar <sudeepkumar@cem-solutions.net>
 *		Copyright 2018
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/of_irq.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <sound/jack.h>

#include "../codecs/cs43130.h"

#include <linux/clk.h>
#include <linux/gcd.h>
#define DEBUG

#define CS43130_DSD_EN_MASK             0x10
#define CS43130_PDN_DONE_INT_MASK        0x00

static struct gpio_desc *snd_allo_clk44gpio;
static struct gpio_desc *snd_allo_clk48gpio;

struct  cs43130_priv {
	struct snd_soc_component        *component;
	struct regmap                   *regmap;
	struct regulator_bulk_data      supplies[CS43130_NUM_SUPPLIES];
	struct gpio_desc                *reset_gpio;
	unsigned int                    dev_id; /* codec device ID */
	int                             xtal_ibias;
	/* shared by both DAIs */
	struct mutex                    clk_mutex;
	int                             clk_req;
	bool                            pll_bypass;
	struct completion               xtal_rdy;
	struct completion               pll_rdy;
	unsigned int                    mclk;
	unsigned int                    mclk_int;
	int                             mclk_int_src;

	/* DAI specific */
	struct cs43130_dai              dais[CS43130_DAI_ID_MAX];

	/* HP load specific */
	bool                            dc_meas;
	bool                            ac_meas;
	bool                            hpload_done;
	struct completion               hpload_evt;
	unsigned int                    hpload_stat;
	u16                             hpload_dc[2];
	u16                             dc_threshold[CS43130_DC_THRESHOLD];
	u16                             ac_freq[CS43130_AC_FREQ];
	u16                             hpload_ac[CS43130_AC_FREQ][2];
	struct workqueue_struct         *wq;
	struct work_struct              work;
	struct snd_soc_jack             jack;
};

static const struct reg_default cs43130_reg_defaults[] = {
	{CS43130_SYS_CLK_CTL_1, 0x06},
	{CS43130_SP_SRATE, 0x01},
	{CS43130_SP_BITSIZE, 0x05},
	{CS43130_PAD_INT_CFG, 0x03},
	{CS43130_PWDN_CTL, 0xFE},
	{CS43130_CRYSTAL_SET, 0x04},
	{CS43130_PLL_SET_1, 0x00},
	{CS43130_PLL_SET_2, 0x00},
	{CS43130_PLL_SET_3, 0x00},
	{CS43130_PLL_SET_4, 0x00},
	{CS43130_PLL_SET_5, 0x40},
	{CS43130_PLL_SET_6, 0x10},
	{CS43130_PLL_SET_7, 0x80},
	{CS43130_PLL_SET_8, 0x03},
	{CS43130_PLL_SET_9, 0x02},
	{CS43130_PLL_SET_10, 0x02},
	{CS43130_CLKOUT_CTL, 0x00},
	{CS43130_ASP_NUM_1, 0x01},
	{CS43130_ASP_NUM_2, 0x00},
	{CS43130_ASP_DEN_1, 0x08},
	{CS43130_ASP_DEN_2, 0x00},
	{CS43130_ASP_LRCK_HI_TIME_1, 0x1F},
	{CS43130_ASP_LRCK_HI_TIME_2, 0x00},
	{CS43130_ASP_LRCK_PERIOD_1, 0x3F},
	{CS43130_ASP_LRCK_PERIOD_2, 0x00},
	{CS43130_ASP_CLOCK_CONF, 0x0C},
	{CS43130_ASP_FRAME_CONF, 0x0A},
	{CS43130_XSP_NUM_1, 0x01},
	{CS43130_XSP_NUM_2, 0x00},
	{CS43130_XSP_DEN_1, 0x02},
	{CS43130_XSP_DEN_2, 0x00},
	{CS43130_XSP_LRCK_HI_TIME_1, 0x1F},
	{CS43130_XSP_LRCK_HI_TIME_2, 0x00},
	{CS43130_XSP_LRCK_PERIOD_1, 0x3F},
	{CS43130_XSP_LRCK_PERIOD_2, 0x00},
	{CS43130_XSP_CLOCK_CONF, 0x0C},
	{CS43130_XSP_FRAME_CONF, 0x0A},
	{CS43130_ASP_CH_1_LOC, 0x00},
	{CS43130_ASP_CH_2_LOC, 0x00},
	{CS43130_ASP_CH_1_SZ_EN, 0x06},
	{CS43130_ASP_CH_2_SZ_EN, 0x0E},
	{CS43130_XSP_CH_1_LOC, 0x00},
	{CS43130_XSP_CH_2_LOC, 0x00},
	{CS43130_XSP_CH_1_SZ_EN, 0x06},
	{CS43130_XSP_CH_2_SZ_EN, 0x0E},
	{CS43130_DSD_VOL_B, 0x78},
	{CS43130_DSD_VOL_A, 0x78},
	{CS43130_DSD_PATH_CTL_1, 0xA8},
	{CS43130_DSD_INT_CFG, 0x00},
	{CS43130_DSD_PATH_CTL_2, 0x02},
	{CS43130_DSD_PCM_MIX_CTL, 0x00},
	{CS43130_DSD_PATH_CTL_3, 0x40},
	{CS43130_HP_OUT_CTL_1, 0x30},
	{CS43130_PCM_FILT_OPT, 0x02},
	{CS43130_PCM_VOL_B, 0x78},
	{CS43130_PCM_VOL_A, 0x78},
	{CS43130_PCM_PATH_CTL_1, 0xA8},
	{CS43130_PCM_PATH_CTL_2, 0x00},
	{CS43130_CLASS_H_CTL, 0x1E},
	{CS43130_HP_DETECT, 0x04},
	{CS43130_HP_LOAD_1, 0x00},
	{CS43130_HP_MEAS_LOAD_1, 0x00},
	{CS43130_HP_MEAS_LOAD_2, 0x00},
	{CS43130_INT_MASK_1, 0xFF},
	{CS43130_INT_MASK_2, 0xFF},
	{CS43130_INT_MASK_3, 0xFF},
	{CS43130_INT_MASK_4, 0xFF},
	{CS43130_INT_MASK_5, 0xFF},
};
static bool cs43130_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS43130_INT_STATUS_1 ... CS43130_INT_STATUS_5:
	case CS43130_HP_DC_STAT_1 ... CS43130_HP_DC_STAT_2:
	case CS43130_HP_AC_STAT_1 ... CS43130_HP_AC_STAT_2:
		return true;
	default:
		return false;
	}
}

static const char * const pcm_spd_texts[] = {
	"Fast",
	"Slow",
};

static SOC_ENUM_SINGLE_DECL(pcm_spd_enum, CS43130_PCM_FILT_OPT, 7,
			pcm_spd_texts);

static const SNDRV_CTL_TLVD_DECLARE_DB_MINMAX(master_tlv, -12750, 0);

static const struct snd_kcontrol_new cs43130_controls[] = {
	SOC_DOUBLE_R_TLV("Master Playback Volume", CS43130_PCM_VOL_B,
			CS43130_PCM_VOL_A, 0, 255, 1, master_tlv),
	SOC_DOUBLE("Master Playback Switch", CS43130_PCM_PATH_CTL_1,
			0, 1, 1, 1),
	SOC_DOUBLE_R_TLV("Digital Playback Volume", CS43130_DSD_VOL_B,
			CS43130_DSD_VOL_A, 0, 255, 1, master_tlv),
	SOC_DOUBLE("Digital Playback Switch", CS43130_DSD_PATH_CTL_1,
			0, 1, 1, 1),
	SOC_SINGLE("HV_Enable", CS43130_HP_OUT_CTL_1, 0, 1, 0),
	SOC_ENUM("PCM Filter Speed", pcm_spd_enum),
	SOC_SINGLE("PCM Phase Compensation", CS43130_PCM_FILT_OPT, 6, 1, 0),
	SOC_SINGLE("PCM Nonoversample Emulate", CS43130_PCM_FILT_OPT, 5, 1, 0),
	SOC_SINGLE("PCM High-pass Filter", CS43130_PCM_FILT_OPT, 1, 1, 0),
	SOC_SINGLE("PCM De-emphasis Filter", CS43130_PCM_FILT_OPT, 0, 1, 0),
};

static bool cs43130_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS43130_DEVID_AB ... CS43130_SYS_CLK_CTL_1:
	case CS43130_SP_SRATE ... CS43130_PAD_INT_CFG:
	case CS43130_PWDN_CTL:
	case CS43130_CRYSTAL_SET:
	case CS43130_PLL_SET_1 ... CS43130_PLL_SET_5:
	case CS43130_PLL_SET_6:
	case CS43130_PLL_SET_7:
	case CS43130_PLL_SET_8:
	case CS43130_PLL_SET_9:
	case CS43130_PLL_SET_10:
	case CS43130_CLKOUT_CTL:
	case CS43130_ASP_NUM_1 ... CS43130_ASP_FRAME_CONF:
	case CS43130_XSP_NUM_1 ... CS43130_XSP_FRAME_CONF:
	case CS43130_ASP_CH_1_LOC:
	case CS43130_ASP_CH_2_LOC:
	case CS43130_ASP_CH_1_SZ_EN:
	case CS43130_ASP_CH_2_SZ_EN:
	case CS43130_XSP_CH_1_LOC:
	case CS43130_XSP_CH_2_LOC:
	case CS43130_XSP_CH_1_SZ_EN:
	case CS43130_XSP_CH_2_SZ_EN:
	case CS43130_DSD_VOL_B ... CS43130_DSD_PATH_CTL_3:
	case CS43130_HP_OUT_CTL_1:
	case CS43130_PCM_FILT_OPT ... CS43130_PCM_PATH_CTL_2:
	case CS43130_CLASS_H_CTL:
	case CS43130_HP_DETECT:
	case CS43130_HP_STATUS:
	case CS43130_HP_LOAD_1:
	case CS43130_HP_MEAS_LOAD_1:
	case CS43130_HP_MEAS_LOAD_2:
	case CS43130_HP_DC_STAT_1:
	case CS43130_HP_DC_STAT_2:
	case CS43130_HP_AC_STAT_1:
	case CS43130_HP_AC_STAT_2:
	case CS43130_HP_LOAD_STAT:
	case CS43130_INT_STATUS_1 ... CS43130_INT_STATUS_5:
	case CS43130_INT_MASK_1 ... CS43130_INT_MASK_5:
		return true;
	default:
		return false;
	}
}
static bool cs43130_precious_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CS43130_INT_STATUS_1 ... CS43130_INT_STATUS_5:
		return true;
	default:
		return false;
	}
}
static int cs43130_pcm_pdn(struct snd_soc_component *component)
{
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);
	int ret;
	unsigned int reg, pdn_int;

	regmap_write(cs43130->regmap, CS43130_DSD_PATH_CTL_2, 0x02);
	regmap_update_bits(cs43130->regmap, CS43130_INT_MASK_1,
			CS43130_PDN_DONE_INT_MASK, 0);
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
			CS43130_PDN_HP_MASK, 1 << CS43130_PDN_HP_SHIFT);
	usleep_range(10, 50);
	ret = regmap_read(cs43130->regmap, CS43130_INT_STATUS_1, &reg);
	pdn_int = reg & 0xFE;
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
			CS43130_PDN_ASP_MASK, 1 << CS43130_PDN_ASP_SHIFT);
	return 0;

}
static int cs43130_pwr_up_asp_dac(struct snd_soc_component *component)
{
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);

	regmap_update_bits(cs43130->regmap, CS43130_PAD_INT_CFG,
			CS43130_ASP_3ST_MASK, 0);
	regmap_write(cs43130->regmap, CS43130_DXD1, 0x99);
	regmap_write(cs43130->regmap, CS43130_DXD13, 0x20);
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
			CS43130_PDN_ASP_MASK, 0);
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
			CS43130_PDN_HP_MASK, 0);
	usleep_range(10000, 12000);
	regmap_write(cs43130->regmap, CS43130_DXD1, 0x00);
	regmap_write(cs43130->regmap, CS43130_DXD13, 0x00);
	return 0;
}
static int cs43130_change_clksrc(struct snd_soc_component *component,
				enum cs43130_mclk_src_sel src)
{
	int ret;
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);
	int mclk_int_decoded;

	if (src == cs43130->mclk_int_src) {
		/* clk source has not changed */
		return 0;
	}
	switch (cs43130->mclk_int) {
	case CS43130_MCLK_22M:
		mclk_int_decoded = CS43130_MCLK_22P5;
		break;
	case CS43130_MCLK_24M:
		mclk_int_decoded = CS43130_MCLK_24P5;
		break;
	default:
		dev_err(component->dev, "Invalid MCLK INT freq: %u\n",
			cs43130->mclk_int);
		return -EINVAL;
	}

	switch (src) {
	case CS43130_MCLK_SRC_EXT:
		cs43130->pll_bypass = true;
		cs43130->mclk_int_src = CS43130_MCLK_SRC_EXT;
		if (cs43130->xtal_ibias == CS43130_XTAL_UNUSED) {
			regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
					CS43130_PDN_XTAL_MASK,
					1 << CS43130_PDN_XTAL_SHIFT);
		} else {
			reinit_completion(&cs43130->xtal_rdy);
			regmap_update_bits(cs43130->regmap, CS43130_INT_MASK_1,
					CS43130_XTAL_RDY_INT_MASK, 0);
			regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
					CS43130_PDN_XTAL_MASK, 0);
			ret = wait_for_completion_timeout(&cs43130->xtal_rdy,
					msecs_to_jiffies(100));
			regmap_update_bits(cs43130->regmap, CS43130_INT_MASK_1,
					CS43130_XTAL_RDY_INT_MASK,
					1 << CS43130_XTAL_RDY_INT_SHIFT);
			if (ret == 0) {
				dev_err(component->dev, "Timeout waiting for XTAL_READY interrupt\n");
				return -ETIMEDOUT;
			}
		}
	regmap_update_bits(cs43130->regmap, CS43130_SYS_CLK_CTL_1,
				CS43130_MCLK_SRC_SEL_MASK,
				src << CS43130_MCLK_SRC_SEL_SHIFT);
	regmap_update_bits(cs43130->regmap, CS43130_SYS_CLK_CTL_1,
				CS43130_MCLK_INT_MASK,
				mclk_int_decoded << CS43130_MCLK_INT_SHIFT);
	usleep_range(150, 200);
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
				CS43130_PDN_PLL_MASK,
				1 << CS43130_PDN_PLL_SHIFT);
	break;
	case CS43130_MCLK_SRC_RCO:
		cs43130->mclk_int_src = CS43130_MCLK_SRC_RCO;

		regmap_update_bits(cs43130->regmap, CS43130_SYS_CLK_CTL_1,
				CS43130_MCLK_SRC_SEL_MASK,
				src << CS43130_MCLK_SRC_SEL_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_SYS_CLK_CTL_1,
				CS43130_MCLK_INT_MASK,
				CS43130_MCLK_22P5 << CS43130_MCLK_INT_SHIFT);
		usleep_range(150, 200);
		regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
				CS43130_PDN_XTAL_MASK,
				1 << CS43130_PDN_XTAL_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
				CS43130_PDN_PLL_MASK,
				1 << CS43130_PDN_PLL_SHIFT);
	break;
	default:
		dev_err(component->dev, "Invalid MCLK source value\n");
		return -EINVAL;
	}

	return 0;
}
static const struct cs43130_bitwidth_map cs43130_bitwidth_table[] = {
	{8,     CS43130_SP_BIT_SIZE_8,  CS43130_CH_BIT_SIZE_8},
	{16,    CS43130_SP_BIT_SIZE_16, CS43130_CH_BIT_SIZE_16},
	{24,    CS43130_SP_BIT_SIZE_24, CS43130_CH_BIT_SIZE_24},
	{32,    CS43130_SP_BIT_SIZE_32, CS43130_CH_BIT_SIZE_32},
};

static const struct cs43130_bitwidth_map *cs43130_get_bitwidth_table(
					unsigned int bitwidth)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs43130_bitwidth_table); i++) {
		if (cs43130_bitwidth_table[i].bitwidth == bitwidth)
			return &cs43130_bitwidth_table[i];
	}

	return NULL;
}
static int cs43130_set_bitwidth(int dai_id, unsigned int bitwidth_dai,
				struct regmap *regmap)
{
	const struct cs43130_bitwidth_map *bw_map;

	bw_map = cs43130_get_bitwidth_table(bitwidth_dai);
	if (!bw_map)
		return -EINVAL;

	switch (dai_id) {
	case CS43130_ASP_PCM_DAI:
	case CS43130_ASP_DOP_DAI:
		regmap_update_bits(regmap, CS43130_ASP_CH_1_SZ_EN,
				CS43130_CH_BITSIZE_MASK, bw_map->ch_bit);
		regmap_update_bits(regmap, CS43130_ASP_CH_2_SZ_EN,
				CS43130_CH_BITSIZE_MASK, bw_map->ch_bit);
		regmap_update_bits(regmap, CS43130_SP_BITSIZE,
				CS43130_ASP_BITSIZE_MASK, bw_map->sp_bit);
		break;
	case CS43130_XSP_DOP_DAI:
		regmap_update_bits(regmap, CS43130_XSP_CH_1_SZ_EN,
				CS43130_CH_BITSIZE_MASK, bw_map->ch_bit);
		regmap_update_bits(regmap, CS43130_XSP_CH_2_SZ_EN,
				CS43130_CH_BITSIZE_MASK, bw_map->ch_bit);
		regmap_update_bits(regmap, CS43130_SP_BITSIZE,
				CS43130_XSP_BITSIZE_MASK, bw_map->sp_bit <<
				CS43130_XSP_BITSIZE_SHIFT);
	break;
	default:
		return -EINVAL;
	}

	return 0;
}
static const struct cs43130_rate_map cs43130_rate_table[] = {
	{32000,         CS43130_ASP_SPRATE_32K},
	{44100,         CS43130_ASP_SPRATE_44_1K},
	{48000,         CS43130_ASP_SPRATE_48K},
	{88200,         CS43130_ASP_SPRATE_88_2K},
	{96000,         CS43130_ASP_SPRATE_96K},
	{176400,        CS43130_ASP_SPRATE_176_4K},
	{192000,        CS43130_ASP_SPRATE_192K},
	{352800,        CS43130_ASP_SPRATE_352_8K},
	{384000,        CS43130_ASP_SPRATE_384K},
};

static const struct cs43130_rate_map *cs43130_get_rate_table(int fs)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs43130_rate_table); i++) {
		if (cs43130_rate_table[i].fs == fs)
			return &cs43130_rate_table[i];
	}

	return NULL;
}

static const struct cs43130_clk_gen *cs43130_get_clk_gen(int mclk_int, int fs,
	const struct cs43130_clk_gen *clk_gen_table, int len_clk_gen_table)
{
	int i;

	for (i = 0; i < len_clk_gen_table; i++) {
		if (clk_gen_table[i].mclk_int == mclk_int &&
					clk_gen_table[i].fs == fs)
			return &clk_gen_table[i];
	}
	return NULL;
}

static int cs43130_set_sp_fmt(int dai_id, unsigned int bitwidth_sclk,
				struct snd_pcm_hw_params *params,
				struct cs43130_priv *cs43130)
{
	u16 frm_size;
	u16 hi_size;
	u8 frm_delay;
	u8 frm_phase;
	u8 frm_data;
	u8 sclk_edge;
	u8 lrck_edge;
	u8 clk_data;
	u8 loc_ch1;
	u8 loc_ch2;
	u8 dai_mode_val;
	const struct cs43130_clk_gen *clk_gen;

	switch (cs43130->dais[dai_id].dai_format) {
	case SND_SOC_DAIFMT_I2S:
		hi_size = bitwidth_sclk;
		frm_delay = 2;
		frm_phase = 0;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		hi_size = bitwidth_sclk;
		frm_delay = 2;
		frm_phase = 1;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		hi_size = 1;
		frm_delay = 2;
		frm_phase = 1;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		hi_size = 1;
		frm_delay = 0;
		frm_phase = 1;
		break;
	default:
		return -EINVAL;
	}
	switch (cs43130->dais[dai_id].dai_mode) {
	case SND_SOC_DAIFMT_CBS_CFS:
		dai_mode_val = 0;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		dai_mode_val = 1;
		break;
	default:
		return -EINVAL;
	}

	frm_size = bitwidth_sclk * params_channels(params);
	sclk_edge = 1;
	lrck_edge = 0;
	loc_ch1 = 0;
	loc_ch2 = bitwidth_sclk * (params_channels(params) - 1);

	frm_data = frm_delay & CS43130_SP_FSD_MASK;
	frm_data |= (frm_phase << CS43130_SP_STP_SHIFT) & CS43130_SP_STP_MASK;

	clk_data = lrck_edge & CS43130_SP_LCPOL_IN_MASK;
	clk_data |= (lrck_edge << CS43130_SP_LCPOL_OUT_SHIFT) &
			CS43130_SP_LCPOL_OUT_MASK;
	clk_data |= (sclk_edge << CS43130_SP_SCPOL_IN_SHIFT) &
			CS43130_SP_SCPOL_IN_MASK;
	clk_data |= (sclk_edge << CS43130_SP_SCPOL_OUT_SHIFT) &
			CS43130_SP_SCPOL_OUT_MASK;
	clk_data |= (dai_mode_val << CS43130_SP_MODE_SHIFT) &
			CS43130_SP_MODE_MASK;
	switch (dai_id) {
	case CS43130_ASP_PCM_DAI:
	case CS43130_ASP_DOP_DAI:
		regmap_update_bits(cs43130->regmap, CS43130_ASP_LRCK_PERIOD_1,
			CS43130_SP_LCPR_DATA_MASK, (frm_size - 1) >>
			CS43130_SP_LCPR_LSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_ASP_LRCK_PERIOD_2,
			CS43130_SP_LCPR_DATA_MASK, (frm_size - 1) >>
			CS43130_SP_LCPR_MSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_ASP_LRCK_HI_TIME_1,
			CS43130_SP_LCHI_DATA_MASK, (hi_size - 1) >>
			CS43130_SP_LCHI_LSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_ASP_LRCK_HI_TIME_2,
			CS43130_SP_LCHI_DATA_MASK, (hi_size - 1) >>
			CS43130_SP_LCHI_MSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_ASP_FRAME_CONF, frm_data);
		regmap_write(cs43130->regmap, CS43130_ASP_CH_1_LOC, loc_ch1);
		regmap_write(cs43130->regmap, CS43130_ASP_CH_2_LOC, loc_ch2);
		regmap_update_bits(cs43130->regmap, CS43130_ASP_CH_1_SZ_EN,
			CS43130_CH_EN_MASK, 1 << CS43130_CH_EN_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_ASP_CH_2_SZ_EN,
			CS43130_CH_EN_MASK, 1 << CS43130_CH_EN_SHIFT);
		regmap_write(cs43130->regmap, CS43130_ASP_CLOCK_CONF, clk_data);
		break;
	case CS43130_XSP_DOP_DAI:
		regmap_update_bits(cs43130->regmap, CS43130_XSP_LRCK_PERIOD_1,
			CS43130_SP_LCPR_DATA_MASK, (frm_size - 1) >>
			CS43130_SP_LCPR_LSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_XSP_LRCK_PERIOD_2,
			CS43130_SP_LCPR_DATA_MASK, (frm_size - 1) >>
			CS43130_SP_LCPR_MSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_XSP_LRCK_HI_TIME_1,
			CS43130_SP_LCHI_DATA_MASK, (hi_size - 1) >>
			CS43130_SP_LCHI_LSB_DATA_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_XSP_LRCK_HI_TIME_2,
			CS43130_SP_LCHI_DATA_MASK, (hi_size - 1) >>
			CS43130_SP_LCHI_MSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_XSP_FRAME_CONF, frm_data);
		regmap_write(cs43130->regmap, CS43130_XSP_CH_1_LOC, loc_ch1);
		regmap_write(cs43130->regmap, CS43130_XSP_CH_2_LOC, loc_ch2);
		regmap_update_bits(cs43130->regmap, CS43130_XSP_CH_1_SZ_EN,
			CS43130_CH_EN_MASK, 1 << CS43130_CH_EN_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_XSP_CH_2_SZ_EN,
			CS43130_CH_EN_MASK, 1 << CS43130_CH_EN_SHIFT);
		regmap_write(cs43130->regmap, CS43130_XSP_CLOCK_CONF, clk_data);
		break;
	default:
		return -EINVAL;
	}
	switch (frm_size) {
	case 16:
		clk_gen = cs43130_get_clk_gen(cs43130->mclk_int,
						params_rate(params),
						cs43130_16_clk_gen,
						ARRAY_SIZE(cs43130_16_clk_gen));
		break;
	case 32:
		clk_gen = cs43130_get_clk_gen(cs43130->mclk_int,
						params_rate(params),
						cs43130_32_clk_gen,
						ARRAY_SIZE(cs43130_32_clk_gen));
		break;
	case 48:
		clk_gen = cs43130_get_clk_gen(cs43130->mclk_int,
						params_rate(params),
						cs43130_48_clk_gen,
						ARRAY_SIZE(cs43130_48_clk_gen));
		break;
	case 64:
		clk_gen = cs43130_get_clk_gen(cs43130->mclk_int,
						params_rate(params),
						cs43130_64_clk_gen,
						ARRAY_SIZE(cs43130_64_clk_gen));
		break;
	default:
		return -EINVAL;
	}
	if (!clk_gen)
		return -EINVAL;
	switch (dai_id) {
	case CS43130_ASP_PCM_DAI:
	case CS43130_ASP_DOP_DAI:
		regmap_write(cs43130->regmap, CS43130_ASP_DEN_1,
				(clk_gen->v.denominator & CS43130_SP_M_LSB_DATA_MASK) >>
				CS43130_SP_M_LSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_ASP_DEN_2,
				(clk_gen->v.denominator & CS43130_SP_M_MSB_DATA_MASK) >>
				CS43130_SP_M_MSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_ASP_NUM_1,
				(clk_gen->v.numerator & CS43130_SP_N_LSB_DATA_MASK) >>
				CS43130_SP_N_LSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_ASP_NUM_2,
				(clk_gen->v.numerator & CS43130_SP_N_MSB_DATA_MASK) >>
				CS43130_SP_N_MSB_DATA_SHIFT);
		break;
	case CS43130_XSP_DOP_DAI:
		regmap_write(cs43130->regmap, CS43130_XSP_DEN_1,
				(clk_gen->v.denominator & CS43130_SP_M_LSB_DATA_MASK) >>
				CS43130_SP_M_LSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_XSP_DEN_2,
				(clk_gen->v.denominator & CS43130_SP_M_MSB_DATA_MASK) >>
				CS43130_SP_M_MSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_XSP_NUM_1,
				(clk_gen->v.numerator & CS43130_SP_N_LSB_DATA_MASK) >>
				CS43130_SP_N_LSB_DATA_SHIFT);
		regmap_write(cs43130->regmap, CS43130_XSP_NUM_2,
				(clk_gen->v.numerator & CS43130_SP_N_MSB_DATA_MASK) >>
				CS43130_SP_N_MSB_DATA_SHIFT);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cs43130_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);
	const struct cs43130_rate_map *rate_map;
	unsigned int sclk = cs43130->dais[dai->id].sclk;
	unsigned int bitwidth_sclk;
	unsigned int bitwidth_dai = (unsigned int)(params_width(params));
	unsigned int dop_rate = (unsigned int)(params_rate(params));
	unsigned int required_clk, ret;
	u8 dsd_speed;

	cs43130->pll_bypass = true;
	cs43130_pcm_pdn(component);
	mutex_lock(&cs43130->clk_mutex);
	if (!cs43130->clk_req) {
		/* no DAI is currently using clk */
		if (!(CS43130_MCLK_22M % params_rate(params))) {
			required_clk = CS43130_MCLK_22M;
			cs43130->mclk_int =  CS43130_MCLK_22M;
			gpiod_set_value_cansleep(snd_allo_clk44gpio, 1);
			gpiod_set_value_cansleep(snd_allo_clk48gpio, 0);
			usleep_range(13500, 14000);
		} else {
			required_clk = CS43130_MCLK_24M;
			cs43130->mclk_int =  CS43130_MCLK_24M;
			gpiod_set_value_cansleep(snd_allo_clk48gpio, 1);
			gpiod_set_value_cansleep(snd_allo_clk44gpio, 0);
			usleep_range(13500, 14000);
		}
		if (cs43130->pll_bypass)
			cs43130_change_clksrc(component, CS43130_MCLK_SRC_EXT);
		else
			cs43130_change_clksrc(component, CS43130_MCLK_SRC_PLL);
	}

	cs43130->clk_req++;
	mutex_unlock(&cs43130->clk_mutex);

	switch (dai->id) {
	case CS43130_ASP_DOP_DAI:
	case CS43130_XSP_DOP_DAI:
		/* DoP bitwidth is always 24-bit */
		bitwidth_dai = 24;
		sclk = params_rate(params) * bitwidth_dai *
				params_channels(params);

		switch (params_rate(params)) {
		case 176400:
			dsd_speed = 0;
			break;
		case 352800:
			dsd_speed = 1;
			break;
		default:
			dev_err(component->dev, "Rate(%u) not supported\n",
				params_rate(params));
			return -EINVAL;
		}

		regmap_update_bits(cs43130->regmap, CS43130_DSD_PATH_CTL_2,
					CS43130_DSD_SPEED_MASK,
					dsd_speed << CS43130_DSD_SPEED_SHIFT);
		break;
	case CS43130_ASP_PCM_DAI:
		rate_map = cs43130_get_rate_table(params_rate(params));
		if (!rate_map)
			return -EINVAL;

		regmap_write(cs43130->regmap, CS43130_SP_SRATE, rate_map->val);
		if ((dop_rate == 176400) && (bitwidth_dai == 24)) {
			dsd_speed = 0;
			regmap_update_bits(cs43130->regmap,
					CS43130_DSD_PATH_CTL_2,
					CS43130_DSD_SPEED_MASK,
					dsd_speed << CS43130_DSD_SPEED_SHIFT);
			regmap_update_bits(cs43130->regmap,
					CS43130_DSD_PATH_CTL_2,
					CS43130_DSD_SRC_MASK,
					CS43130_DSD_SRC_ASP <<
					CS43130_DSD_SRC_SHIFT);
			regmap_update_bits(cs43130->regmap,
					CS43130_DSD_PATH_CTL_2,
					CS43130_DSD_EN_MASK, 0x01 <<
					CS43130_DSD_EN_SHIFT);
		}
		break;
	default:
		dev_err(component->dev, "Invalid DAI (%d)\n", dai->id);
		return -EINVAL;
	}

	switch (dai->id) {
	case CS43130_ASP_DOP_DAI:
		regmap_update_bits(cs43130->regmap, CS43130_DSD_PATH_CTL_2,
				CS43130_DSD_SRC_MASK, CS43130_DSD_SRC_ASP <<
				CS43130_DSD_SRC_SHIFT);
		regmap_update_bits(cs43130->regmap, CS43130_DSD_PATH_CTL_2,
				CS43130_DSD_EN_MASK, 0x01 <<
				CS43130_DSD_EN_SHIFT);
		break;
	case CS43130_XSP_DOP_DAI:
		regmap_update_bits(cs43130->regmap, CS43130_DSD_PATH_CTL_2,
				CS43130_DSD_SRC_MASK, CS43130_DSD_SRC_XSP <<
				CS43130_DSD_SRC_SHIFT);
		break;
	}
	if (!sclk && cs43130->dais[dai->id].dai_mode ==
						SND_SOC_DAIFMT_CBM_CFM) {
		/* Calculate SCLK in master mode if unassigned */
		sclk = params_rate(params) * bitwidth_dai *
				params_channels(params);
	}
	if (!sclk) {
		/* at this point, SCLK must be set */
		dev_err(component->dev, "SCLK freq is not set\n");
		return -EINVAL;
	}

	bitwidth_sclk = (sclk / params_rate(params)) / params_channels(params);
	if (bitwidth_sclk < bitwidth_dai) {
		dev_err(component->dev, "Format not supported: SCLK freq is too low\n");
		return -EINVAL;
	}

	dev_dbg(component->dev,
		"sclk = %u, fs = %d, bitwidth_dai = %u\n",
		sclk, params_rate(params), bitwidth_dai);

	dev_dbg(component->dev,
		"bitwidth_sclk = %u, num_ch = %u\n",
		bitwidth_sclk, params_channels(params));

	cs43130_set_bitwidth(dai->id, bitwidth_dai, cs43130->regmap);
	cs43130_set_sp_fmt(dai->id, bitwidth_sclk, params, cs43130);
	ret = cs43130_pwr_up_asp_dac(component);
	return 0;
}

static int cs43130_hw_free(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);

	mutex_lock(&cs43130->clk_mutex);
	cs43130->clk_req--;
	if (!cs43130->clk_req) {
		/* no DAI is currently using clk */
		cs43130_change_clksrc(component, CS43130_MCLK_SRC_RCO);
		cs43130_pcm_pdn(component);
	}
	mutex_unlock(&cs43130->clk_mutex);

	return 0;
}

static const unsigned int cs43130_asp_src_rates[] = {
	32000, 44100, 48000, 88200, 96000, 176400, 192000
};

static const struct snd_pcm_hw_constraint_list cs43130_asp_constraints = {
	.count  = ARRAY_SIZE(cs43130_asp_src_rates),
	.list   = cs43130_asp_src_rates,
};

static int cs43130_pcm_startup(struct snd_pcm_substream *substream,
					struct snd_soc_dai *dai)
{
	return snd_pcm_hw_constraint_list(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_RATE,
					&cs43130_asp_constraints);
}

static int cs43130_pcm_set_fmt(struct snd_soc_dai *codec_dai, unsigned int fmt)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		cs43130->dais[codec_dai->id].dai_mode = SND_SOC_DAIFMT_CBS_CFS;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		cs43130->dais[codec_dai->id].dai_mode = SND_SOC_DAIFMT_CBM_CFM;
		break;
	default:
		dev_err(component->dev, "unsupported mode\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		cs43130->dais[codec_dai->id].dai_format = SND_SOC_DAIFMT_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		cs43130->dais[codec_dai->id].dai_format = SND_SOC_DAIFMT_LEFT_J;
		break;
	default:
		dev_err(component->dev,
			"unsupported audio format\n");
		return -EINVAL;
	}

	dev_dbg(component->dev, "dai_id = %d,  dai_mode = %u, dai_format = %u\n",
			codec_dai->id,
			cs43130->dais[codec_dai->id].dai_mode,
			cs43130->dais[codec_dai->id].dai_format);

	return 0;
}

static int cs43130_set_sysclk(struct snd_soc_dai *codec_dai,
					int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *component = codec_dai->component;
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);

	cs43130->dais[codec_dai->id].sclk = freq;
	dev_dbg(component->dev, "dai_id = %d,  sclk = %u\n", codec_dai->id,
				cs43130->dais[codec_dai->id].sclk);

	return 0;
}

static int cs43130_component_set_sysclk(struct snd_soc_component *component,
					int clk_id, int source,
					unsigned int freq, int dir)
{
	struct cs43130_priv *cs43130 =
				snd_soc_component_get_drvdata(component);

	dev_dbg(component->dev, "clk_id = %d, source = %d, freq = %d, dir = %d\n",
		clk_id, source, freq, dir);

	switch (freq) {
	case CS43130_MCLK_22M:
	case CS43130_MCLK_24M:
		cs43130->mclk = freq;
		break;
	default:
		dev_err(component->dev, "Invalid MCLK INT freq: %u\n", freq);
		return -EINVAL;
	}

	if (source == CS43130_MCLK_SRC_EXT) {
		cs43130->pll_bypass = true;
	} else {
		dev_err(component->dev, "Invalid MCLK source\n");
		return -EINVAL;
	}

	return 0;
}
static u16 const cs43130_ac_freq[CS43130_AC_FREQ] = {
	24,
	43,
	93,
	200,
	431,
	928,
	2000,
	4309,
	9283,
	20000,
};
static const struct snd_soc_dai_ops cs43130_dai_ops = {
	.startup        = cs43130_pcm_startup,
	.hw_params	= cs43130_hw_params,
	.hw_free        = cs43130_hw_free,
	.set_sysclk     = cs43130_set_sysclk,
	.set_fmt        = cs43130_pcm_set_fmt,
};

static struct snd_soc_dai_driver cs43130_codec_dai = {
	.name = "allo-cs43130",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min = 44100,
		.rate_max = 192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			SNDRV_PCM_FMTBIT_S24_LE |
			SNDRV_PCM_FMTBIT_S32_LE

	},
	.ops = &cs43130_dai_ops,
};

static struct snd_soc_component_driver cs43130_component_driver = {
	.idle_bias_on           = true,
	.controls		= cs43130_controls,
	.num_controls		= ARRAY_SIZE(cs43130_controls),
	.set_sysclk             = cs43130_component_set_sysclk,
	.idle_bias_on           = 1,
	.use_pmdown_time        = 1,
	.endianness             = 1,
};

static const struct regmap_config cs43130_regmap = {
	.reg_bits               = 24,
	.pad_bits               = 8,
	.val_bits               = 8,

	.max_register           = CS43130_LASTREG,
	.reg_defaults           = cs43130_reg_defaults,
	.num_reg_defaults       = ARRAY_SIZE(cs43130_reg_defaults),
	.readable_reg           = cs43130_readable_register,
	.precious_reg           = cs43130_precious_register,
	.volatile_reg           = cs43130_volatile_register,
	.cache_type             = REGCACHE_RBTREE,
	/* needed for regcache_sync */
	.use_single_read        = true,
	.use_single_write       = true,
};

static u16 const cs43130_dc_threshold[CS43130_DC_THRESHOLD] = {
	50,
	120,
};

static int cs43130_handle_device_data(struct i2c_client *i2c_client,
					struct cs43130_priv *cs43130)
{
	struct device_node *np = i2c_client->dev.of_node;
	unsigned int val;
	int i;

	if (of_property_read_u32(np, "cirrus,xtal-ibias", &val) < 0) {
	/* Crystal is unused. System clock is used for external MCLK */
		cs43130->xtal_ibias = CS43130_XTAL_UNUSED;
		return 0;
	}

	switch (val) {
	case 1:
		cs43130->xtal_ibias = CS43130_XTAL_IBIAS_7_5UA;
		break;
	case 2:
		cs43130->xtal_ibias = CS43130_XTAL_IBIAS_12_5UA;
		break;
	case 3:
		cs43130->xtal_ibias = CS43130_XTAL_IBIAS_15UA;
		break;
	default:
		dev_err(&i2c_client->dev,
			"Invalid cirrus,xtal-ibias value: %d\n", val);
		return -EINVAL;
	}

	cs43130->dc_meas = of_property_read_bool(np, "cirrus,dc-measure");
	cs43130->ac_meas = of_property_read_bool(np, "cirrus,ac-measure");

	if (of_property_read_u16_array(np, "cirrus,ac-freq", cs43130->ac_freq,
					CS43130_AC_FREQ) < 0) {
		for (i = 0; i < CS43130_AC_FREQ; i++)
			cs43130->ac_freq[i] = cs43130_ac_freq[i];
	}

	if (of_property_read_u16_array(np, "cirrus,dc-threshold",
					cs43130->dc_threshold,
					CS43130_DC_THRESHOLD) < 0) {
		for (i = 0; i < CS43130_DC_THRESHOLD; i++)
			cs43130->dc_threshold[i] = cs43130_dc_threshold[i];
	}

	return 0;
}


static int allo_cs43130_component_probe(struct i2c_client *i2c)
{
	struct regmap *regmap;
	struct regmap_config config = cs43130_regmap;
	struct device *dev = &i2c->dev;
	struct cs43130_priv *cs43130;
	unsigned int devid = 0;
	unsigned int reg;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	cs43130 = devm_kzalloc(dev, sizeof(struct cs43130_priv),
					GFP_KERNEL);
	if (!cs43130)
		return -ENOMEM;

	dev_set_drvdata(dev, cs43130);
	cs43130->regmap = regmap;

	if (i2c->dev.of_node) {
		ret = cs43130_handle_device_data(i2c, cs43130);
		if (ret != 0)
			return ret;
	}
	usleep_range(2000, 2050);

	ret = regmap_read(cs43130->regmap, CS43130_DEVID_AB, &reg);
	devid = (reg & 0xFF) << 12;
	ret = regmap_read(cs43130->regmap, CS43130_DEVID_CD, &reg);
	devid |= (reg & 0xFF) << 4;
	ret = regmap_read(cs43130->regmap, CS43130_DEVID_E, &reg);
	devid |= (reg & 0xF0) >> 4;
	if (devid != CS43198_CHIP_ID) {
		dev_err(dev, "Failed to read Chip or wrong Chip id: %d\n", ret);
		return ret;
	}

	cs43130->mclk_int_src = CS43130_MCLK_SRC_RCO;
	msleep(20);

	ret = snd_soc_register_component(dev, &cs43130_component_driver,
				    &cs43130_codec_dai, 1);
	if (ret != 0) {
		dev_err(dev, "failed to register codec: %d\n", ret);
		return ret;
	}
	regmap_update_bits(cs43130->regmap, CS43130_PAD_INT_CFG,
			CS43130_ASP_3ST_MASK, 0);
	regmap_update_bits(cs43130->regmap, CS43130_PAD_INT_CFG,
			CS43130_XSP_3ST_MASK, 1);
	regmap_update_bits(cs43130->regmap, CS43130_PWDN_CTL,
			CS43130_PDN_HP_MASK, 1 << CS43130_PDN_HP_SHIFT);
	msleep(20);
	regmap_write(cs43130->regmap, CS43130_CLASS_H_CTL, 0x06);
	snd_allo_clk44gpio = devm_gpiod_get(dev, "clock44", GPIOD_OUT_HIGH);
	if (IS_ERR(snd_allo_clk44gpio))
		dev_err(dev, "devm_gpiod_get() failed\n");

	snd_allo_clk48gpio = devm_gpiod_get(dev, "clock48", GPIOD_OUT_LOW);
	if (IS_ERR(snd_allo_clk48gpio))
		dev_err(dev, "devm_gpiod_get() failed\n");

	return 0;
}

static void allo_cs43130_component_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_component(&i2c->dev);
}

static const struct i2c_device_id allo_cs43130_component_id[] = {
	{ "allo-cs43198", },
	{ }
};
MODULE_DEVICE_TABLE(i2c, allo_cs43130_component_id);

static const struct of_device_id allo_cs43130_codec_of_match[] = {
	{ .compatible = "allo,allo-cs43198", },
	{ }
};
MODULE_DEVICE_TABLE(of, allo_cs43130_codec_of_match);

static struct i2c_driver allo_cs43130_component_driver = {
	.probe		= allo_cs43130_component_probe,
	.remove		= allo_cs43130_component_remove,
	.id_table	= allo_cs43130_component_id,
	.driver		= {
	.name		= "allo-cs43198",
	.of_match_table = allo_cs43130_codec_of_match,
	},
};

module_i2c_driver(allo_cs43130_component_driver);

MODULE_DESCRIPTION("ASoC Allo Boss2 Codec Driver");
MODULE_AUTHOR("Sudeepkumar <sudeepkumar@cem-solutions.net>");
MODULE_LICENSE("GPL v2");
