// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx codec driver
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * Analog Devices Inc. documentation cited in some of the comments below:
 *
 * [1] AD2420(W)/6(W)/7(W)/8(W)/9(W) Automotive Audio Bus A2B Transceiver
 *     Technical Reference, Revision 1.1, October 2019, Part Number 82-100138-01
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc-component.h>
#include <sound/soc.h>

#define AD24XX_FORMATS_16 (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE)
#define AD24XX_FORMATS_32                                    \
	(SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE | \
	 SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_U32_LE)

#define AD24XX_RATES_SUB_48 \
	(SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)
#define AD24XX_RATES_SUB_44_1                                                 \
	(SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_22050 | SNDRV_PCM_RATE_44100 | \
	 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_176400)
#define AD24XX_RATES_MAIN_48 SNDRV_PCM_RATE_48000
#define AD24XX_RATES_MAIN_44_1 SNDRV_PCM_RATE_44100

struct ad24xx_codec {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct regmap *regmap;
	struct snd_soc_dai_driver *dai_drv;
	unsigned int active_substreams;
	enum a2b_slot_size min_slot_size;
	bool report_slots;
};

static int ad24xx_codec_calc_a_dnslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int dnslots;
	unsigned int dnmasken;
	unsigned int ldnslots;
	unsigned int bcdnslots;
	unsigned int dnmaskrx;
	__le32 dnmask;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of downstream slots to be received by this
	 * node's A-side transceiver. For main nodes this is trivially zero
	 * because the A-side is inactive. Following [1] section 3-18
	 * "Downstream Data Slots", for subordinate nodes the calculation
	 * depends on whether the A2B_LDNSLOTS.DNMASKEN bit is set:
	 *
	 *   DNMASKEN=0 => A2B_BCDNSLOTS + A2B_DNSLOTS + A2B_LDNSLOTS
	 *   DNMASKEN=1 => max(A2B_DNSLOTS, dnmaskrx)
	 *
	 * where dnmaskrx is the most significant bit of the A2B_DNMASK{0,3}
	 * mask.
	 */

	if (is_a2b_main(node))
		return 0;

	ret = regmap_read(adc->regmap, A2B_DNSLOTS, &val);
	if (ret)
		return ret;

	dnslots = FIELD_GET(A2B_DNSLOTS_DNSLOTS_MASK, val);

	ret = regmap_read(adc->regmap, A2B_LDNSLOTS, &val);
	if (ret)
		return ret;

	ldnslots = FIELD_GET(A2B_LDNSLOTS_LDNSLOTS_MASK, val);
	dnmasken = FIELD_GET(A2B_LDNSLOTS_DNMASKEN_MASK, val);

	if (!dnmasken) {
		ret = regmap_read(adc->regmap, A2B_BCDNSLOTS, &val);
		if (ret)
			return ret;

		bcdnslots = FIELD_GET(A2B_BCDNSLOTS_BCDNSLOTS_MASK, val);

		return bcdnslots + dnslots + ldnslots;
	}

	ret = regmap_bulk_read(adc->regmap, A2B_DNMASK0, &dnmask, 4);
	if (ret)
		return ret;

	dnmaskrx = fls(le32_to_cpu(dnmask));

	return max(dnslots, dnmaskrx);
}

static int ad24xx_codec_calc_b_dnslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int dnslots;
	unsigned int dnmasken;
	unsigned int ldnslots;
	unsigned int bcdnslots;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of downstream slots to be transmitted by this
	 * node's B-side transceiver. Following [1] section 3-18 "Downstream
	 * Data Slots", for main nodes the number is A2B_DNSLOTS. For
	 * subordinate nodes the calculation depends on whether the
	 * A2B_LDNSLOTS.DNMASKEN bit is set:
	 *
	 *   DNMASKEN=0 => A2B_BCDNSLOTS + A2B_DNSLOTS
	 *   DNMASKEN=1 => A2B_DNSLOTS + A2B_LDNSLOTS
	 */

	ret = regmap_read(adc->regmap, A2B_DNSLOTS, &val);
	if (ret)
		return ret;

	dnslots = FIELD_GET(A2B_DNSLOTS_DNSLOTS_MASK, val);

	if (is_a2b_main(node))
		return dnslots;

	ret = regmap_read(adc->regmap, A2B_LDNSLOTS, &val);
	if (ret)
		return ret;

	ldnslots = FIELD_GET(A2B_LDNSLOTS_LDNSLOTS_MASK, val);
	dnmasken = FIELD_GET(A2B_LDNSLOTS_DNMASKEN_MASK, val);

	if (dnmasken)
		return dnslots + ldnslots;

	ret = regmap_read(adc->regmap, A2B_BCDNSLOTS, &val);
	if (ret)
		return ret;

	bcdnslots = FIELD_GET(A2B_BCDNSLOTS_BCDNSLOTS_MASK, val);

	return bcdnslots + dnslots;
}

static unsigned int ad24xx_codec_calc_a_upslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int upslots;
	unsigned int lupslots;
	unsigned int val;
	int ret;

	/*
	 * Calculate the number of upstream slots to be transmitted by this
	 * node's A-side transceiver. According to [1] section 3-20 "Upstream
	 * Data Slots", this is A2B_UPSLOTS + A2B_LUPSLOTS for subordinate
	 * nodes. For the main node it is trivially always zero, as its A-side
	 * is inactive.
	 */

	if (is_a2b_main(node))
		return 0;

	ret = regmap_read(adc->regmap, A2B_UPSLOTS, &val);
	if (ret)
		return ret;

	upslots = FIELD_GET(A2B_UPSLOTS_UPSLOTS_MASK, val);

	ret = regmap_read(adc->regmap, A2B_LUPSLOTS, &val);
	if (ret)
		return ret;

	lupslots = FIELD_GET(A2B_LUPSLOTS_LUPSLOTS_MASK, val);

	return upslots + lupslots;
}

static unsigned int ad24xx_codec_calc_b_upslots(struct ad24xx_codec *adc)
{
	struct a2b_node *node = adc->node;
	unsigned int upslots;
	unsigned int upmaskrx;
	unsigned int upmask;
	unsigned int val;
	u8 buf[4];
	int ret;

	/*
	 * Calculate the number of upstream slots to be received by this node's
	 * B-side transceiver. This is, cf. [1] section 3-20, max(A2B_UPSLOTS,
	 * upmaskrx), where upmaskrx is the most significant bit of the
	 * A2B_UPMASK{0,3} mask. For main nodes it is simply the value of
	 * A2B_UPSLOTS, as they have no upstream data RX mask to configure.
	 */

	ret = regmap_read(adc->regmap, A2B_UPSLOTS, &val);
	if (ret)
		return ret;

	upslots = FIELD_GET(A2B_UPSLOTS_UPSLOTS_MASK, val);

	if (is_a2b_main(node))
		return upslots;

	ret = regmap_bulk_read(adc->regmap, A2B_UPMASK0, buf, 4);
	if (ret)
		return ret;

	upmask = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
	upmaskrx = fls(upmask);

	return max(upslots, upmaskrx);
}

static void ad24xx_codec_report_slots(struct ad24xx_codec *adc)
{
	struct a2b_node_slots slots = {
		.a_dnslots = ad24xx_codec_calc_a_dnslots(adc),
		.a_upslots = ad24xx_codec_calc_a_upslots(adc),
		.b_dnslots = ad24xx_codec_calc_b_dnslots(adc),
		.b_upslots = ad24xx_codec_calc_b_upslots(adc),
		/*
		 * The Playback (resp. Capture) stream can consist of
		 * both upstream and downstream slots, so request the
		 * same slot size for both upstream and downstream A2B
		 * data. On the ASoC side, DAI symmetry ensures that
		 * substreams have the same sample bits. Alternate slot
		 * format is not supported.
		 */
		.size_dn = adc->min_slot_size,
		.size_up = adc->min_slot_size,
		.format_dn = A2B_SLOT_FORMAT_NORMAL,
		.format_up = A2B_SLOT_FORMAT_NORMAL,
	};

	/*
	 * Report this new slot configuration and apply the structure. Structure
	 * validation can cause the latter step to fail, in which case the call
	 * will return an error code. The error is ignored for several reasons:
	 *
	 *   - the slot configuration is programmed non-atomically through DAPM,
	 *     so this node's configuration might not yet be valid on this pass
	 *     of the DAPM sequence
	 *
	 *   - structure validity is a function of all nodes' slot
	 *     configurations, and the other nodes might not have reported their
	 *     final configuration yet
	 *
	 *   - the ASoC codepath through which this function is called means
	 *     that a non-zero return code will be ignored anyway
	 *
	 * Instead, the assumption made here is that all quiescent states of the
	 * sound card will yield a valid structure. If that is the case,
	 * eventually the structure will be applied.
	 *
	 * This gratuitous application of a new structure also permits
	 * reconfiguration of the slots (via kcontrols) while the sound card is
	 * open and data is flowing.
	 */
	a2b_node_report_slots(adc->node, &slots);
}

static int ad24xx_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	bool bclk_invert;
	unsigned int val;
	int ret;

	/* Main node must be BCLK/FSYNC consumer, subordinate node provider */
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    (is_a2b_main(adc->node) ? SND_SOC_DAIFMT_CBC_CFC :
				      SND_SOC_DAIFMT_CBP_CFP))
		return -EINVAL;

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		if (adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = false;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		if (!adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = false;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		if (adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = true;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		if (!adc->node->invert_sync)
			return -EINVAL;
		bclk_invert = true;
		break;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		if (!adc->node->alternating_sync || !adc->node->early_sync)
			return -EINVAL;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		if (adc->node->alternating_sync || !adc->node->early_sync)
			return -EINVAL;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		if (adc->node->alternating_sync || adc->node->early_sync)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	val = bclk_invert ? A2B_I2SCFG_RXBCLKINV_MASK :
			    A2B_I2SCFG_TXBCLKINV_MASK;
	ret = regmap_update_bits(
		adc->regmap, A2B_I2SCFG,
		A2B_I2SCFG_TXBCLKINV_MASK | A2B_I2SCFG_RXBCLKINV_MASK, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_codec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	unsigned int rate = params_rate(params);
	int ret;

	/* Configure I2S/TDM rate */
	if (is_a2b_main(adc->node)) {
		/*
		 * The I2S rate of the main node DAIs is fixed at the superframe
		 * frequency (SFF) and cannot change.
		 */
		if (!((rate == 48000 && adc->node->sff == A2B_SFF_48000) ||
		      (rate == 44100 && adc->node->sff == A2B_SFF_44100)))
			return -EINVAL;
	} else {
		/*
		 * The I2S rate of subordinate nodes can be set to (SFF * x)
		 * for x in { 0.25, 0.5, 1, 2, 4 }.
		 */
		unsigned int sff_rate =
			adc->node->sff == A2B_SFF_48000 ? 48000 : 44100;
		unsigned int val = 0;

		if (rate == sff_rate / 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 2);
		else if (rate == sff_rate / 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 1);
		else if (rate == sff_rate)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 0);
		/* A2B_I2SRRATE.RRDIV support is not implemented */
		else if (rate == sff_rate * 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 5);
		else if (rate == sff_rate * 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 6);
		else
			return -EINVAL;

		ret = regmap_update_bits(adc->regmap, A2B_I2SRATE,
					 A2B_I2SRATE_I2SRATE_MASK, val);
		if (ret)
			return ret;
	}

	/* Report the minimum A2B slot size needed to support these hw_params */
	if (!adc->active_substreams) {
		enum a2b_slot_size slot_size;

		switch (snd_pcm_format_width(params_format(params))) {
		case 8:
			slot_size = A2B_SLOT_SIZE_8;
			break;
		case 12:
			slot_size = A2B_SLOT_SIZE_12;
			break;
		case 16:
			slot_size = A2B_SLOT_SIZE_16;
			break;
		case 20:
			slot_size = A2B_SLOT_SIZE_20;
			break;
		case 24:
			slot_size = A2B_SLOT_SIZE_24;
			break;
		case 28:
			slot_size = A2B_SLOT_SIZE_28;
			break;
		case 32:
			slot_size = A2B_SLOT_SIZE_32;
			break;
		default:
			return -EINVAL;
		}

		adc->min_slot_size = slot_size;
		adc->report_slots = true;
		adc->active_substreams |= BIT(substream->stream);
	}

	return 0;
}

static int ad24xx_codec_hw_free(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);

	adc->active_substreams &= ~BIT(substream->stream);

	if (!adc->active_substreams) {
		adc->min_slot_size = 0;
		adc->report_slots = true;
	}

	return 0;
}

static const struct snd_soc_dai_ops ad24xx_codec_dai_ops = {
	.set_fmt = ad24xx_codec_set_fmt,
	.hw_params = ad24xx_codec_hw_params,
	.hw_free = ad24xx_codec_hw_free,
};

enum ad24xx_codec_dai {
	AD24XX_DAI_I2S,
};

static const struct snd_soc_dai_driver ad24xx_codec_dai_drv[] = {
	[AD24XX_DAI_I2S] = {
		.name = "ad24xx-i2s",
		.playback = {
			.stream_name = "I2S Playback",
			.channels_min = 1,
			.channels_max = 32,
		},
		.capture = {
			.stream_name = "I2S Capture",
			.channels_min = 1,
			.channels_max = 32,
		},
		.ops = &ad24xx_codec_dai_ops,
		.symmetric_rate = 1,
		.symmetric_sample_bits = 1,
	},
};

static int ad24xx_codec_put_slots(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_context *dapm =
		snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct snd_soc_component *component = snd_soc_dapm_to_component(dapm);
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int mask = (1 << fls(mc->max)) - 1;
	unsigned int val = ucontrol->value.integer.value[0] & mask;

	if (val == dapm_kcontrol_get_value(kcontrol))
		return 0;

	/*
	 * The slots are being reconfigured. Set this flag to trigger a new slot
	 * report. This allows the structure to be changed at runtime when there
	 * are active substreams.
	 */
	adc->report_slots = true;

	return snd_soc_dapm_put_volsw(kcontrol, ucontrol);
}

/*
 * Autodisabled kcontrol macro for slot configuration register fields. It is
 * essentially the same as SOC_DAPM_SINGLE_AUTODISABLE, but with a custom put
 * function that triggers a slot report when the value is changed.
 */
#define A2B_DAPM_SLOT_SINGLE(xname, reg, shift, max) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER,       \
	  .name = xname,                             \
	  .info = snd_soc_info_volsw,                \
	  .get = snd_soc_dapm_get_volsw,             \
	  .put = ad24xx_codec_put_slots,             \
	  .private_value = SOC_SINGLE_VALUE(reg, shift, max, 0, 1) }

static const char *const ad24xx_dnmasken_text[] = { "Disabled", "Enabled" };
static SOC_ENUM_SINGLE_DECL(ad24xx_dnmasken_enum, A2B_LDNSLOTS,
			    A2B_LDNSLOTS_DNMASKEN_SHIFT, ad24xx_dnmasken_text);

static const struct snd_kcontrol_new ad24xx_dnmasken_kcontrol =
	SOC_DAPM_ENUM("DNMASKEN", ad24xx_dnmasken_enum);

static const struct snd_kcontrol_new ad24xx_ldnslots_kcontrol =
	A2B_DAPM_SLOT_SINGLE("LDNSLOTS", A2B_LDNSLOTS, 0, 32);

static const struct snd_kcontrol_new ad24xx_bcdnslots_kcontrol =
	A2B_DAPM_SLOT_SINGLE("BCDNSLOTS", A2B_BCDNSLOTS, 0, 32);

static const struct snd_kcontrol_new ad24xx_lupslots_kcontrol =
	A2B_DAPM_SLOT_SINGLE("LUPSLOTS", A2B_LUPSLOTS, 0, 32);

static const struct snd_kcontrol_new ad24xx_dnslots_kcontrol =
	A2B_DAPM_SLOT_SINGLE("DNSLOTS", A2B_DNSLOTS, 0, 32);

static const struct snd_kcontrol_new ad24xx_upslots_kcontrol =
	A2B_DAPM_SLOT_SINGLE("UPSLOTS", A2B_UPSLOTS, 0, 32);

static const struct snd_kcontrol_new ad24xx_upmask_kcontrols[] = {
	A2B_DAPM_SLOT_SINGLE("UPMASK0", A2B_UPMASK0, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("UPMASK1", A2B_UPMASK1, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("UPMASK2", A2B_UPMASK2, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("UPMASK3", A2B_UPMASK3, 0, 0xFF),
};

static const struct snd_kcontrol_new ad24xx_dnmask_kcontrols[] = {
	A2B_DAPM_SLOT_SINGLE("DNMASK0", A2B_DNMASK0, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("DNMASK1", A2B_DNMASK1, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("DNMASK2", A2B_DNMASK2, 0, 0xFF),
	A2B_DAPM_SLOT_SINGLE("DNMASK3", A2B_DNMASK3, 0, 0xFF),
};

static const struct snd_kcontrol_new ad24xx_codec_controls_data_rx_mask[] = {
	SOC_SINGLE("UPOFFSET", A2B_UPOFFSET, 0, 31, 0),
	SOC_SINGLE("DNOFFSET", A2B_DNOFFSET, 0, 31, 0),
};

static int ad24xx_codec_slot_mixer_event(struct snd_soc_dapm_widget *w,
					 struct snd_kcontrol *kcontrol,
					 int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);

	if (event & (SND_SOC_DAPM_WILL_PMU | SND_SOC_DAPM_WILL_PMD))
		adc->report_slots = true;

	return 0;
}

/*
 * Slot mixer control macro. These snd_soc_dapm_mixer_named_ctl widgets will
 * trigger a new slot report when powering up or down.
 */
#define A2B_SOC_DAPM_SLOT_MIXER(wname, wcontrols, wncontrols)     \
	SND_SOC_DAPM_MIXER_NAMED_CTL_E(                           \
		wname, SND_SOC_NOPM, 0, 0, wcontrols, wncontrols, \
		ad24xx_codec_slot_mixer_event,                    \
		SND_SOC_DAPM_WILL_PMU | SND_SOC_DAPM_WILL_PMD)

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets_main[] = {
	SND_SOC_DAPM_AIF_IN("RX0", NULL, 0, A2B_I2SCFG, 4, 0),
	SND_SOC_DAPM_AIF_IN("RX1", NULL, 0, A2B_I2SCFG, 5, 0),
	SND_SOC_DAPM_AIF_OUT("TX0", NULL, 0, A2B_I2SCFG, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", NULL, 0, A2B_I2SCFG, 1, 0),
	SND_SOC_DAPM_AIF_IN("TRXB UP", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TRXB DN", NULL, 0, SND_SOC_NOPM, 0, 0),
	A2B_SOC_DAPM_SLOT_MIXER("DNSLOTS", &ad24xx_dnslots_kcontrol, 1),
	A2B_SOC_DAPM_SLOT_MIXER("UPSLOTS", &ad24xx_upslots_kcontrol, 1),
};

#define A2B_SOC_DAPM_BUFFER(wname)   \
	{ .id = snd_soc_dapm_buffer, \
	  .name = wname,             \
	  SND_SOC_DAPM_INIT_REG_VAL(SND_SOC_NOPM, 0, 0) }

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets_sub[] = {
	SND_SOC_DAPM_AIF_IN("RX0", NULL, 0, A2B_I2SCFG, 4, 0),
	SND_SOC_DAPM_AIF_IN("RX1", NULL, 0, A2B_I2SCFG, 5, 0),
	SND_SOC_DAPM_AIF_OUT("TX0", NULL, 0, A2B_I2SCFG, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", NULL, 0, A2B_I2SCFG, 1, 0),
	SND_SOC_DAPM_AIF_IN("TRXA DN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("TRXB UP", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TRXA UP", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TRXB DN", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MUX("TRXB DN MUX", SND_SOC_NOPM, 0, 0,
			 &ad24xx_dnmasken_kcontrol),
	SND_SOC_DAPM_MUX("TX MUX", SND_SOC_NOPM, 0, 0,
			 &ad24xx_dnmasken_kcontrol),
	A2B_SOC_DAPM_SLOT_MIXER("LDNSLOTS RX", &ad24xx_ldnslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("LDNSLOTS RX PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("LDNSLOTS TX", &ad24xx_ldnslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("LDNSLOTS TX PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("BCDNSLOTS", &ad24xx_bcdnslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("BCDNSLOTS PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("LUPSLOTS", &ad24xx_lupslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("LUPSLOTS PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("DNSLOTS", &ad24xx_dnslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("DNSLOTS PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("UPSLOTS", &ad24xx_upslots_kcontrol, 1),
	A2B_SOC_DAPM_BUFFER("UPSLOTS PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("UPMASK", ad24xx_upmask_kcontrols, 4),
	A2B_SOC_DAPM_BUFFER("UPMASK PRE"),
	A2B_SOC_DAPM_SLOT_MIXER("DNMASK", ad24xx_dnmask_kcontrols, 4),
	A2B_SOC_DAPM_BUFFER("DNMASK PRE"),
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_main[] = {
	{ "TRXB DN", NULL, "DNSLOTS" },
	{ "DNSLOTS", "DNSLOTS", "I2S Playback" },
	{ "I2S Capture", NULL, "UPSLOTS" },
	{ "UPSLOTS", "UPSLOTS", "TRXB UP" },
	{ "TX0", NULL, "I2S Capture" },
	{ "TX1", NULL, "I2S Capture" },
	{ "I2S Playback", NULL, "RX0" },
	{ "I2S Playback", NULL, "RX1" },
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_sub[] = {
	{ "LDNSLOTS RX PRE", NULL, "I2S Capture" },
	{ "LDNSLOTS RX", "LDNSLOTS", "LDNSLOTS RX PRE" },
	{ "TRXB DN MUX", "Enabled", "LDNSLOTS RX" },

	{ "LDNSLOTS TX PRE", NULL, "TRXA DN" },
	{ "LDNSLOTS TX", "LDNSLOTS", "LDNSLOTS TX PRE" },
	{ "TX MUX", "Disabled", "LDNSLOTS TX" },

	{ "BCDNSLOTS PRE", NULL, "TRXA DN" },
	{ "BCDNSLOTS", "BCDNSLOTS", "BCDNSLOTS PRE" },
	{ "TRXB DN MUX", "Disabled", "BCDNSLOTS" },

	{ "I2S Playback", NULL, "TX MUX" },
	{ "TRXB DN", NULL, "TRXB DN MUX" },

	{ "LUPSLOTS PRE", NULL, "I2S Capture" },
	{ "LUPSLOTS", "LUPSLOTS", "LUPSLOTS PRE" },
	{ "TRXA UP", NULL, "LUPSLOTS" },

	{ "TRXB DN", NULL, "DNSLOTS" },
	{ "DNSLOTS", "DNSLOTS", "DNSLOTS PRE" },
	{ "DNSLOTS PRE", NULL, "TRXA DN" },

	{ "UPSLOTS PRE", NULL, "TRXB UP" },
	{ "UPSLOTS", "UPSLOTS", "UPSLOTS PRE" },
	{ "TRXA UP", NULL, "UPSLOTS" },

	{ "UPMASK PRE", NULL, "TRXB UP" },
	{ "UPMASK", "UPMASK0", "UPMASK PRE" },
	{ "UPMASK", "UPMASK1", "UPMASK PRE" },
	{ "UPMASK", "UPMASK2", "UPMASK PRE" },
	{ "UPMASK", "UPMASK3", "UPMASK PRE" },
	{ "I2S Playback", NULL, "UPMASK" },

	{ "DNMASK PRE", NULL, "TRXA DN" },
	{ "DNMASK", "DNMASK0", "DNMASK PRE" },
	{ "DNMASK", "DNMASK1", "DNMASK PRE" },
	{ "DNMASK", "DNMASK2", "DNMASK PRE" },
	{ "DNMASK", "DNMASK3", "DNMASK PRE" },
	{ "I2S Playback", NULL, "DNMASK" },

	{ "I2S Capture", NULL, "RX0" },
	{ "I2S Capture", NULL, "RX1" },
	{ "TX0", NULL, "I2S Playback" },
	{ "TX1", NULL, "I2S Playback" },
};

static int
ad24xx_codec_component_stream_event(struct snd_soc_component *component,
				    int event)
{
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);

	if (adc->report_slots) {
		adc->report_slots = false;
		ad24xx_codec_report_slots(adc);
	}

	return 0;
}

static int ad24xx_codec_component_probe(struct snd_soc_component *component)
{
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	struct a2b_node *node = adc->node;
	int ret;

	snd_soc_component_init_regmap(component, adc->regmap);

	if (is_a2b_sub(node) &&
	    (node->chip_info->caps & A2B_CHIP_CAP_DATA_RX_MASK)) {
		ret = snd_soc_add_component_controls(
			component, ad24xx_codec_controls_data_rx_mask,
			ARRAY_SIZE(ad24xx_codec_controls_data_rx_mask));
		if (ret)
			return ret;
	}

	return 0;
}

static const struct snd_soc_component_driver ad24xx_codec_component_drv_main = {
	.name = "ad24xx-codec",
	.probe = ad24xx_codec_component_probe,
	.stream_event = ad24xx_codec_component_stream_event,
	.dapm_widgets = ad24xx_codec_dapm_widgets_main,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets_main),
	.dapm_routes = ad24xx_codec_dapm_routes_main,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_main),
	.endianness = 1,
};

static const struct snd_soc_component_driver ad24xx_codec_component_drv_sub = {
	.name = "ad24xx-codec",
	.probe = ad24xx_codec_component_probe,
	.stream_event = ad24xx_codec_component_stream_event,
	.dapm_widgets = ad24xx_codec_dapm_widgets_sub,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets_sub),
	.dapm_routes = ad24xx_codec_dapm_routes_sub,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_sub),
	.endianness = 1,
};

static const struct regmap_config ad24xx_codec_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static int ad24xx_codec_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	const struct snd_soc_component_driver *drv;
	struct snd_soc_dai_driver *i2s_dai;
	struct ad24xx_codec *adc;
	int ret;

	adc = devm_kzalloc(dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return -ENOMEM;

	adc->dev = dev;
	adc->func = func;
	adc->node = func->node;
	dev_set_drvdata(dev, adc);

	adc->regmap =
		devm_regmap_init_a2b_func(func, &ad24xx_codec_regmap_config);
	if (IS_ERR(adc->regmap))
		return PTR_ERR(adc->regmap);

	adc->dai_drv = devm_kmemdup(dev, ad24xx_codec_dai_drv,
				    sizeof(ad24xx_codec_dai_drv), GFP_KERNEL);
	if (!adc->dai_drv)
		return -ENOMEM;

	i2s_dai = &adc->dai_drv[AD24XX_DAI_I2S];

	if (adc->node->tdm_slot_size == A2B_TDMSS_32)
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			AD24XX_FORMATS_32;
	else
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			AD24XX_FORMATS_16;

	if (is_a2b_main(adc->node)) {
		if (adc->node->sff == A2B_SFF_48000)
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_MAIN_48;
		else
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_MAIN_44_1;
	} else {
		if (adc->node->sff == A2B_SFF_48000)
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_SUB_48;
		else
			i2s_dai->playback.rates = i2s_dai->capture.rates =
				AD24XX_RATES_SUB_44_1;
	}

	if (is_a2b_main(adc->node))
		drv = &ad24xx_codec_component_drv_main;
	else
		drv = &ad24xx_codec_component_drv_sub;

	ret = devm_snd_soc_register_component(dev, drv, adc->dai_drv,
					      ARRAY_SIZE(ad24xx_codec_dai_drv));
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_codec_of_match_table[] = {
	{ .compatible = "adi,ad2403-codec" },
	{ .compatible = "adi,ad2410-codec" },
	{ .compatible = "adi,ad2425-codec" },
	{ .compatible = "adi,ad2428-codec" },
	{ .compatible = "adi,ad2429-codec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_codec_of_match_table);

static struct a2b_driver ad24xx_codec_driver = {
	.driver = {
		.name = "ad24xx-codec",
		.of_match_table = ad24xx_codec_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_codec_probe,
};
module_a2b_driver(ad24xx_codec_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx codec driver");
MODULE_LICENSE("GPL");
