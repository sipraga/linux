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

// TODO: Populate this properly
#define AD24XX_RATES (SNDRV_PCM_RATE_8000_192000)
#define AD24XX_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE)

// TODO: Really two DAIs? Maybe it can be handled better with just some
// kcontrols...
enum ad24xx_codec_dai_id {
	AD24XX_DAI0,
	AD24XX_DAI1,
};

struct ad24xx_codec {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	bool i2s_active[2];
	bool tx_bclk_invert;
	bool rx_bclk_invert;
};

static const struct snd_soc_dapm_widget ad24xx_codec_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("RX0", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("RX1", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX0", NULL, 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("TX1", NULL, 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route ad24xx_codec_dapm_routes[] = {
	{ "DAI0 Capture", NULL, "RX0" },
	{ "TX0", NULL, "DAI0 Playback" },
	{ "DAI1 Capture", NULL, "RX1" },
	{ "TX1", NULL, "DAI1 Playback" },
};

static int ad24xx_codec_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct ad24xx_codec *adc = snd_soc_component_get_drvdata(component);
	bool bclk_invert;
	unsigned int val;
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

	ret = a2b_func_read(adc->func, A2B_I2SCFG, &val, 0);
	if (ret)
		return ret;

	if (bclk_invert)
		val |= A2B_I2SCFG_RXBCLKINV_MASK | A2B_I2SCFG_TXBCLKINV_MASK;
	else
		val &= ~(A2B_I2SCFG_RXBCLKINV_MASK | A2B_I2SCFG_TXBCLKINV_MASK);

	ret = a2b_func_write(adc->func, A2B_I2SCFG, val, 0);
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
	unsigned int val;
	int ret;

	/* Configure I2S/TDM rate */
	// TODO: This is shared between DAIs, need to enforce symmetry!
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

		ret = a2b_func_read(adc->func, A2B_I2SRATE, &val, 0);
		if (ret)
			return ret;

		val &= ~A2B_I2SRATE_I2SRATE_MASK;

		if (rate == sff_rate / 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 2);
		else if (rate == sff_rate / 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 1);
		else if (rate == sff_rate)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 0);
		// TODO: Implement support for A2B_I2SRRATE.RRDIV
		else if (rate == sff_rate * 2)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 5);
		else if (rate == sff_rate * 4)
			val |= FIELD_PREP(A2B_I2SRATE_I2SRATE_MASK, 6);
		else
			return -EINVAL;

		ret = a2b_func_write(adc->func, A2B_I2SRATE, val, 0);
		if (ret)
			return ret;
	}

	/* Set enable bit */
	ret = a2b_func_read(adc->func, A2B_I2SCFG, &val, 0);
	if (ret)
		return ret;

	if (!is_a2b_main(adc->node)) {
		if (direction == SNDRV_PCM_STREAM_PLAYBACK)
			val |= 1 << dai->id;
		else
			val |= (1 << dai->id) << 4;
	} else {
		if (direction == SNDRV_PCM_STREAM_PLAYBACK)
			val |= (1 << dai->id) << 4;
		else
			val |= 1 << dai->id;
	}

	ret = a2b_func_write(adc->func, A2B_I2SCFG, val, 0);
	if (ret)
		return ret;

	/* Set up slots stuff */
	if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
		if (!is_a2b_main(adc->node)) {
			ret = a2b_func_write(adc->func, A2B_LDNSLOTS,
					     A2B_LDNSLOTS_DNMASKEN_MASK, 0);
			if (ret)
				return ret;

			// Always forward 16 broadcast slots
			// TODO: Maybe this should be part of the core
			ret = a2b_func_write(adc->func, A2B_DNSLOTS, 16, 0);
			if (ret)
				return ret;

			ret = a2b_func_write(adc->func, A2B_DNMASK0, 0xFF,
					     0); // 8 slots
			if (ret)
				return ret;

			ret = a2b_func_write(adc->func, A2B_DNMASK1, 0xFF,
					     0); // 8 slots
			if (ret)
				return ret;
		} else {
			// FIXME: SLOTFMT hard-coded to uncompressed 32 bit
			ret = a2b_func_write(adc->func, A2B_SLOTFMT, 0x66, 0);
			if (ret)
				return ret;

			ret = a2b_func_write(adc->func, A2B_DNSLOTS, 16, 0);
			if (ret)
				return ret;

			ret = a2b_func_read(adc->func, A2B_DATCTL, &val, 0);
			if (ret)
				return ret;

			val |= A2B_DATCTL_DNS_MASK;

			ret = a2b_func_write(adc->func, A2B_DATCTL, val, 0);
			if (ret)
				return ret;
		}
	} else {
		return -EINVAL; // TODO
	}

	/* Finally, request slots */
	ret = a2b_node_request_slots(adc->node,
				     direction == SNDRV_PCM_STREAM_PLAYBACK ?
					     A2B_DIR_DOWN :
					     A2B_DIR_UP,
				     params_channels(params));
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
	unsigned int val;
	int ret;

	/* Unwind slot parameters */
	if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
		if (!is_a2b_main(adc->node)) {
			ret = a2b_func_write(adc->func, A2B_DNSLOTS, 0, 0);
			if (ret)
				return ret;

			ret = a2b_func_write(adc->func, A2B_DNMASK0, 0,
					     0);
			if (ret)
				return ret;

			ret = a2b_func_write(adc->func, A2B_DNMASK1, 0,
					     0);
			if (ret)
				return ret;
		} else {
			ret = a2b_func_write(adc->func, A2B_DNSLOTS, 0, 0);
			if (ret)
				return ret;

			ret = a2b_func_read(adc->func, A2B_DATCTL, &val, 0);
			if (ret)
				return ret;

			val &= ~A2B_DATCTL_DNS_MASK;

			ret = a2b_func_write(adc->func, A2B_DATCTL, val, 0);
			if (ret)
				return ret;
		}
	} else {
		return -EINVAL; // TODO
	}

	/* Prepare to (un)request slots */
	ret = a2b_node_request_slots_pre(
		adc->node, direction == SNDRV_PCM_STREAM_PLAYBACK ?
				   A2B_DIR_DOWN :
				   A2B_DIR_UP);
	if (ret)
		return ret;

	/* Unset enable bit */
	// TODO: move to mute_stream?
	ret = a2b_func_read(adc->func, A2B_I2SCFG, &val, 0);
	if (ret)
		return ret;

	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		val &= ~(1 << dai->id);
	else
		val &= ~((1 << dai->id) << 4);

	ret = a2b_func_write(adc->func, A2B_I2SCFG, val, 0);
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

static struct snd_soc_dai_driver ad24xx_codec_dai_drv[] = {
	{
		.id = AD24XX_DAI0,
		.name = "ad24xx-dai0",
		.playback = {
			.stream_name = "DAI0 Playback",
			.rates = AD24XX_RATES,
			.channels_min = 1,
			.channels_max = 32,
			.formats = AD24XX_FORMATS,
			// TODO: set formats depending on node->tdm_ss?
		},
		.capture = {
			.stream_name = "DAI0 Capture",
			.rates = AD24XX_RATES,
			.channels_min = 1,
			.channels_max = 32,
			.formats = AD24XX_FORMATS,
		},
		.ops = &ad24xx_codec_dai_ops,
		.symmetric_rate = 1,
	},
	{
		.id = AD24XX_DAI1,
		.name = "ad24xx-dai1",
		.playback = {
			.stream_name = "DAI1 Playback",
			.rates = AD24XX_RATES,
			.channels_min = 1,
			.channels_max = 32,
			.formats = AD24XX_FORMATS,
		},
		.capture = {
			.stream_name = "DAI1 Capture",
			.rates = AD24XX_RATES,
			.channels_min = 1,
			.channels_max = 32,
			.formats = AD24XX_FORMATS,
		},
		.ops = &ad24xx_codec_dai_ops,
		.symmetric_rate = 1,
	},
};

static int ad24xx_codec_component_probe(struct snd_soc_component *component)
{
	return 0;
}

static const struct snd_soc_component_driver ad24xx_codec_component_driver = {
	.probe = ad24xx_codec_component_probe,
	.dapm_widgets = ad24xx_codec_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(ad24xx_codec_dapm_widgets),
	.dapm_routes = ad24xx_codec_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(ad24xx_codec_dapm_routes),
	.idle_bias_on = 1,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static int ad24xx_codec_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	struct ad24xx_codec *adc;
	int ret;

	adc = devm_kzalloc(dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return -ENOMEM;

	adc->dev = dev;
	adc->func = func;
	adc->node = func->node;
	dev_set_drvdata(dev, adc);

	ret = devm_snd_soc_register_component(dev,
					      &ad24xx_codec_component_driver,
					      ad24xx_codec_dai_drv,
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
