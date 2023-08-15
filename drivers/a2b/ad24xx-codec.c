// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx codec driver
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/ad24xx.h>
#include <linux/bitfield.h>
#include <linux/module.h>
#include <sound/soc.h>
#include <linux/a2b/a2b.h>

#define AD24XX_RATES_SUB_48                                                   \
	(/* SNDRV_PCM_RATE_{12000,24000} missing? | */ SNDRV_PCM_RATE_48000 | \
	 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)
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
};

static const char *const ad24xx_codec_slotfmt_text[] = {
	"Normal Slot Format",
	"Alternate Slot Format",
};

static const char *const ad24xx_codec_slot_size_text[] = {
	"8 bits",  "12 bits", "16 bits", "20 bits",
	"24 bits", "28 bits", "32 bits",
};

static SOC_ENUM_SINGLE_DECL(ad24xx_codec_dn_slot_size_enum, A2B_SLOTFMT, 0,
			    ad24xx_codec_slot_size_text);
static SOC_ENUM_SINGLE_DECL(ad24xx_codec_dn_slotfmt_enum, A2B_SLOTFMT, 3,
			    ad24xx_codec_slotfmt_text);
static SOC_ENUM_SINGLE_DECL(ad24xx_codec_up_slot_size_enum, A2B_SLOTFMT, 4,
			    ad24xx_codec_slot_size_text);
static SOC_ENUM_SINGLE_DECL(ad24xx_codec_up_slotfmt_enum, A2B_SLOTFMT, 7,
			    ad24xx_codec_slotfmt_text);

static const struct snd_kcontrol_new ad24xx_codec_controls_main[] = {
	SOC_SINGLE("Downstream Slots", A2B_DNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots", A2B_UPSLOTS, 0, 32, 0),
	// WARNING: DON'T SET A2B_SLOTFMT WHILE PCM DEVICE OPEN, HORRIBLE NOISE
	SOC_ENUM("Downstream Slot Size", ad24xx_codec_dn_slot_size_enum),
	SOC_ENUM("Downstream Slot Format", ad24xx_codec_dn_slotfmt_enum),
	SOC_ENUM("Upstream Slot Size", ad24xx_codec_up_slot_size_enum),
	SOC_ENUM("Upstream Slot Format", ad24xx_codec_up_slotfmt_enum),
};

static const struct snd_kcontrol_new ad24xx_codec_controls_sub[] = {
	SOC_SINGLE("Broadcast Downstream Slots", A2B_BCDNSLOTS, 0, 32, 0),
	SOC_SINGLE("Downstream Broadcast Mask Enable", A2B_LDNSLOTS, 7, 1, 0),
	SOC_SINGLE("Downstream Slots Targeted", A2B_LDNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots Generated", A2B_LUPSLOTS, 0, 32, 0),
	SOC_SINGLE("Downstream Slots", A2B_DNSLOTS, 0, 32, 0),
	SOC_SINGLE("Upstream Slots", A2B_UPSLOTS, 0, 32, 0),
	SND_SOC_BYTES("Upstream Data RX Mask", A2B_UPMASK0, 4),
	SOC_SINGLE("Local Upstream Channel Offset", A2B_UPOFFSET, 0, 31, 0),
	SND_SOC_BYTES("Downstream Data RX Mask", A2B_DNMASK0, 4),
	SOC_SINGLE("Local Downstream Channel Offset", A2B_DNOFFSET, 0, 31, 0),
};

#define SND_SOC_DAPM_ENCODER(wname, stname, wreg, wshift, winvert) \
{	.id = snd_soc_dapm_encoder, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }

#define SND_SOC_DAPM_DECODER(wname, stname, wreg, wshift, winvert) \
{	.id = snd_soc_dapm_decoder, .name = wname, .sname = stname, \
	SND_SOC_DAPM_INIT_REG_VAL(wreg, wshift, winvert), }

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets_main[] = {
	SND_SOC_DAPM_AIF_IN("RX0", "I2S Playback", 0, A2B_I2SCFG, 4, 0),
	SND_SOC_DAPM_AIF_IN("RX1", "I2S Playback", 0, A2B_I2SCFG, 5, 0),
	SND_SOC_DAPM_AIF_OUT("TX0", "I2S Capture", 0, A2B_I2SCFG, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", "I2S Capture", 0, A2B_I2SCFG, 1, 0),
	SND_SOC_DAPM_ENCODER("ENC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DECODER("DEC", NULL, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets_sub[] = {
	SND_SOC_DAPM_AIF_OUT("TX0", "I2S Playback", 0, A2B_I2SCFG, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", "I2S Playback", 0, A2B_I2SCFG, 1, 0),
	SND_SOC_DAPM_AIF_IN("RX0", "I2S Capture", 0, A2B_I2SCFG, 4, 0),
	SND_SOC_DAPM_AIF_IN("RX1", "I2S Capture", 0, A2B_I2SCFG, 5, 0),
	SND_SOC_DAPM_ENCODER("ENC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DECODER("DEC", NULL, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_main[] = {
	{ "ENC", NULL, "RX0" },
	{ "ENC", NULL, "RX1" },
	{ "TX0", NULL, "DEC" },
	{ "TX1", NULL, "DEC" },
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes_sub[] = {
	{ "ENC", NULL, "RX0" },
	{ "ENC", NULL, "RX1" },
	{ "TX0", NULL, "DEC" },
	{ "TX1", NULL, "DEC" },
};

static int ad24xx_codec_read(void *context, const void *reg_buf,
			     size_t reg_size, void *val_buf, size_t val_size)
{
	struct ad24xx_codec *adc = context;
	unsigned int reg;
	u32 *val;
	int ret;
	int i;

	if (reg_size != 1 || val_size > 4)
		return -EINVAL;

	reg = *((u8 *)reg_buf);
	val = val_buf;
	*val = 0;

	for (i = 0; i < val_size; i++) {
		unsigned int tmp;

		ret = a2b_func_read(adc->func, reg + i, &tmp, 0);
		if (ret)
			return ret;

		*val |= tmp << (i * 8);
	}

	return 0;
}

static int ad24xx_codec_write(void *context, const void *data, size_t count)
{
	struct ad24xx_codec *adc = context;
	unsigned int reg;
	int ret;
	int i;

	if (count > 5)
		return -EINVAL;

	reg = ((u8 *)data)[0];

	for (i = 0; i < count - 1; i++) {
		unsigned int tmp = ((u8 *)data)[i + 1];

		ret = a2b_func_write(adc->func, reg + i, tmp, 0);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct regmap_config ad24xx_codec_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.write = ad24xx_codec_write,
	.read = ad24xx_codec_read,
	.cache_type = REGCACHE_RBTREE,
	.max_raw_read = 4,
	.max_raw_write = 4,
};

static int ad24xx_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	bool bclk_invert;
	unsigned int val;
	unsigned int mask;
	int ret;

	/* Main node must be BCLK/FSYNC consumer, subordinate node consumer */
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

	mask = A2B_I2SCFG_RXBCLKINV_MASK | A2B_I2SCFG_RXBCLKINV_MASK;
	val = bclk_invert ? mask : 0;
	ret = regmap_update_bits(adc->regmap, A2B_I2SCFG, mask, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_codec_startup(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	int direction = substream->stream;
	int ret;

	ret = a2b_node_request_slots_pre(
		adc->node, direction == SNDRV_PCM_STREAM_PLAYBACK ?
				   A2B_DIR_DOWN :
				   A2B_DIR_UP);
	if (ret)
		return ret;

	return 0;
}

static void ad24xx_codec_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	int direction = substream->stream;
	int ret;

	/*
	 * The assumption here is that all codecs of the A2B bus are part of the
	 * same PCM runtime. If not, then the RESPCYCS may get set wrong and the
	 * bus may malfunction.
	 */
	ret = a2b_node_request_slots(adc->node,
				     direction == SNDRV_PCM_STREAM_PLAYBACK ?
					     A2B_DIR_DOWN :
					     A2B_DIR_UP,
				     0);
	if (ret)
		dev_err(adc->dev, "failed to free slots: %d\n", ret);
}

static int ad24xx_codec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	int direction = substream->stream;
	unsigned int rate = params_rate(params);
	unsigned int num_dnslots;
	unsigned int num_upslots;
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

	ret = regmap_read(adc->regmap, A2B_DNSLOTS, &num_dnslots);
	if (ret)
		return ret;

	ret = regmap_read(adc->regmap, A2B_UPSLOTS, &num_upslots);
	if (ret)
		return ret;

	/* Finally, request slots */
	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		ret = a2b_node_request_slots(adc->node, A2B_DIR_DOWN,
					     num_dnslots);
	else
		ret = a2b_node_request_slots(adc->node, A2B_DIR_UP,
					     num_upslots);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_codec_hw_free(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	int direction = substream->stream;
	int ret;

	/* Prepare to (un)request slots */
	ret = a2b_node_request_slots_pre(
		adc->node, direction == SNDRV_PCM_STREAM_PLAYBACK ?
				   A2B_DIR_DOWN :
				   A2B_DIR_UP);
	if (ret)
		return ret;

	return 0;
}

static const struct snd_soc_dai_ops ad24xx_codec_dai_ops = {
	.set_fmt = ad24xx_codec_set_fmt,
	.startup = ad24xx_codec_startup,
	.shutdown = ad24xx_codec_shutdown,
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
	},
};

static int ad24xx_codec_component_probe(struct snd_soc_component *component)
{
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);

	snd_soc_component_init_regmap(component, adc->regmap);

	return 0;
}

static const struct snd_soc_component_driver ad24xx_codec_component_drv_main = {
	.probe = ad24xx_codec_component_probe,
	.controls = ad24xx_codec_controls_main,
	.num_controls = ARRAY_SIZE(ad24xx_codec_controls_main),
	.dapm_widgets = ad24xx_codec_dapm_widgets_main,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets_main),
	.dapm_routes = ad24xx_codec_dapm_routes_main,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_main),
	.endianness = 1,
};

static const struct snd_soc_component_driver ad24xx_codec_component_drv_sub = {
	.probe = ad24xx_codec_component_probe,
	.controls = ad24xx_codec_controls_sub,
	.num_controls = ARRAY_SIZE(ad24xx_codec_controls_sub),
	.dapm_widgets = ad24xx_codec_dapm_widgets_sub,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets_sub),
	.dapm_routes = ad24xx_codec_dapm_routes_sub,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes_sub),
	.endianness = 1,
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
		devm_regmap_init(dev, NULL, adc, &ad24xx_codec_regmap_config);
	if (IS_ERR(adc->regmap))
		return PTR_ERR(adc->regmap);

	adc->dai_drv = devm_kmemdup(dev, ad24xx_codec_dai_drv,
				    sizeof(ad24xx_codec_dai_drv), GFP_KERNEL);
	if (!adc->dai_drv)
		return -ENOMEM;

	i2s_dai = &adc->dai_drv[AD24XX_DAI_I2S];

	if (adc->node->tdm_slot_size == A2B_TDMSS_32)
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			SNDRV_PCM_FMTBIT_S32_LE;
	else
		i2s_dai->playback.formats = i2s_dai->capture.formats =
			SNDRV_PCM_FMTBIT_S16_LE;

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
	{
		.compatible = "adi,ad2403-codec",
	},
	{
		.compatible = "adi,ad2410-codec",
	},
	{
		.compatible = "adi,ad2425-codec",
	},
	{
		.compatible = "adi,ad2428-codec",
	},
	{
		.compatible = "adi,ad2429-codec",
	},
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
