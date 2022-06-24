// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
#include <linux/module.h>
#include <linux/string.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include <sound/soc-dai.h>
#include <sound/soc-dapm.h>
#include <uapi/sound/asound.h>
#include "sof_maxim_common.h"

#define MAX_98373_PIN_NAME 16

const struct snd_soc_dapm_route max_98373_dapm_routes[] = {
	/* speaker */
	{ "Left Spk", NULL, "Left BE_OUT" },
	{ "Right Spk", NULL, "Right BE_OUT" },
};
EXPORT_SYMBOL_NS(max_98373_dapm_routes, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static struct snd_soc_codec_conf max_98373_codec_conf[] = {
	{
		.dlc = COMP_CODEC_CONF(MAX_98373_DEV0_NAME),
		.name_prefix = "Right",
	},
	{
		.dlc = COMP_CODEC_CONF(MAX_98373_DEV1_NAME),
		.name_prefix = "Left",
	},
};

struct snd_soc_dai_link_component max_98373_components[] = {
	{  /* For Right */
		.name = MAX_98373_DEV0_NAME,
		.dai_name = MAX_98373_CODEC_DAI,
	},
	{  /* For Left */
		.name = MAX_98373_DEV1_NAME,
		.dai_name = MAX_98373_CODEC_DAI,
	},
};
EXPORT_SYMBOL_NS(max_98373_components, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static int max_98373_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int j;

	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		if (!strcmp(codec_dai->component->name, MAX_98373_DEV0_NAME)) {
			/* DEV0 tdm slot configuration */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x03, 3, 8, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98373_DEV1_NAME)) {
			/* DEV1 tdm slot configuration */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x0C, 3, 8, 32);
		}
	}
	return 0;
}

int max_98373_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	struct snd_soc_dai *cpu_dai;
	int j;
	int ret = 0;

	/* set spk pin by playback only */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	cpu_dai = asoc_rtd_to_cpu(rtd, 0);
	for_each_rtd_codec_dais(rtd, j, codec_dai) {
		struct snd_soc_dapm_context *dapm =
				snd_soc_component_get_dapm(cpu_dai->component);
		char pin_name[MAX_98373_PIN_NAME];

		snprintf(pin_name, ARRAY_SIZE(pin_name), "%s Spk",
			 codec_dai->component->name_prefix);

		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
			ret = snd_soc_dapm_enable_pin(dapm, pin_name);
			if (!ret)
				snd_soc_dapm_sync(dapm);
			break;
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
			ret = snd_soc_dapm_disable_pin(dapm, pin_name);
			if (!ret)
				snd_soc_dapm_sync(dapm);
			break;
		default:
			break;
		}
	}

	return ret;
}
EXPORT_SYMBOL_NS(max_98373_trigger, SND_SOC_INTEL_SOF_MAXIM_COMMON);

struct snd_soc_ops max_98373_ops = {
	.hw_params = max_98373_hw_params,
	.trigger = max_98373_trigger,
};
EXPORT_SYMBOL_NS(max_98373_ops, SND_SOC_INTEL_SOF_MAXIM_COMMON);

int max_98373_spk_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_add_routes(&card->dapm, max_98373_dapm_routes,
				      ARRAY_SIZE(max_98373_dapm_routes));
	if (ret)
		dev_err(rtd->dev, "Speaker map addition failed: %d\n", ret);
	return ret;
}
EXPORT_SYMBOL_NS(max_98373_spk_codec_init, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void max_98373_set_codec_conf(struct snd_soc_card *card)
{
	card->codec_conf = max_98373_codec_conf;
	card->num_configs = ARRAY_SIZE(max_98373_codec_conf);
}
EXPORT_SYMBOL_NS(max_98373_set_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

/*
 * Maxim MAX98390
 */
static const struct snd_soc_dapm_route max_98390_dapm_routes[] = {
	/* speaker */
	{ "Left Spk", NULL, "Left BE_OUT" },
	{ "Right Spk", NULL, "Right BE_OUT" },
};

static const struct snd_kcontrol_new max_98390_tt_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("TL Spk"),
	SOC_DAPM_PIN_SWITCH("TR Spk"),
};

static const struct snd_soc_dapm_widget max_98390_tt_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("TL Spk", NULL),
	SND_SOC_DAPM_SPK("TR Spk", NULL),
};

static const struct snd_soc_dapm_route max_98390_tt_dapm_routes[] = {
	/* Tweeter speaker */
	{ "TL Spk", NULL, "Tweeter Left BE_OUT" },
	{ "TR Spk", NULL, "Tweeter Right BE_OUT" },
};

static struct snd_soc_codec_conf max_98390_codec_conf[] = {
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV0_NAME),
		.name_prefix = "Right",
	},
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV1_NAME),
		.name_prefix = "Left",
	},
};

static struct snd_soc_codec_conf max_98390_4spk_codec_conf[] = {
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV0_NAME),
		.name_prefix = "Right",
	},
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV1_NAME),
		.name_prefix = "Left",
	},
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV2_NAME),
		.name_prefix = "Tweeter Right",
	},
	{
		.dlc = COMP_CODEC_CONF(MAX_98390_DEV3_NAME),
		.name_prefix = "Tweeter Left",
	},
};

struct snd_soc_dai_link_component max_98390_components[] = {
	{
		.name = MAX_98390_DEV0_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
	{
		.name = MAX_98390_DEV1_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
};
EXPORT_SYMBOL_NS(max_98390_components, SND_SOC_INTEL_SOF_MAXIM_COMMON);

struct snd_soc_dai_link_component max_98390_4spk_components[] = {
	{
		.name = MAX_98390_DEV0_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
	{
		.name = MAX_98390_DEV1_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
	{
		.name = MAX_98390_DEV2_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
	{
		.name = MAX_98390_DEV3_NAME,
		.dai_name = MAX_98390_CODEC_DAI,
	},
};
EXPORT_SYMBOL_NS(max_98390_4spk_components, SND_SOC_INTEL_SOF_MAXIM_COMMON);

static int max_98390_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		if (i >= ARRAY_SIZE(max_98390_4spk_components)) {
			dev_err(codec_dai->dev, "invalid codec index %d\n", i);
			return -ENODEV;
		}

		if (!strcmp(codec_dai->component->name, MAX_98390_DEV0_NAME)) {
			/* DEV0 tdm slot configuration Right */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x01, 3, 4, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98390_DEV1_NAME)) {
			/* DEV1 tdm slot configuration Left */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x02, 3, 4, 32);
		}

		if (!strcmp(codec_dai->component->name, MAX_98390_DEV2_NAME)) {
			/* DEVi2 tdm slot configuration Tweeter Right */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x04, 3, 4, 32);
		}
		if (!strcmp(codec_dai->component->name, MAX_98390_DEV3_NAME)) {
			/* DEV3 tdm slot configuration Tweeter Left */
			snd_soc_dai_set_tdm_slot(codec_dai, 0x08, 3, 4, 32);
		}
	}
	return 0;
}

int max_98390_spk_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	/* add regular speakers dapm route */
	ret = snd_soc_dapm_add_routes(&card->dapm, max_98390_dapm_routes,
				      ARRAY_SIZE(max_98390_dapm_routes));
	if (ret) {
		dev_err(rtd->dev, "unable to add Left/Right Speaker dapm, ret %d\n", ret);
		return ret;
	}

	/* add widgets/controls/dapm for tweeter speakers */
	if (acpi_dev_present("MX98390", "3", -1)) {
		ret = snd_soc_dapm_new_controls(&card->dapm, max_98390_tt_dapm_widgets,
						ARRAY_SIZE(max_98390_tt_dapm_widgets));

		if (ret) {
			dev_err(rtd->dev, "unable to add tweeter dapm controls, ret %d\n", ret);
			/* Don't need to add routes if widget addition failed */
			return ret;
		}

		ret = snd_soc_add_card_controls(card, max_98390_tt_kcontrols,
						ARRAY_SIZE(max_98390_tt_kcontrols));
		if (ret) {
			dev_err(rtd->dev, "unable to add tweeter card controls, ret %d\n", ret);
			return ret;
		}

		ret = snd_soc_dapm_add_routes(&card->dapm, max_98390_tt_dapm_routes,
					      ARRAY_SIZE(max_98390_tt_dapm_routes));
		if (ret)
			dev_err(rtd->dev,
				"unable to add Tweeter Left/Right Speaker dapm, ret %d\n", ret);
	}
	return ret;
}
EXPORT_SYMBOL_NS(max_98390_spk_codec_init, SND_SOC_INTEL_SOF_MAXIM_COMMON);

const struct snd_soc_ops max_98390_ops = {
	.hw_params = max_98390_hw_params,
};
EXPORT_SYMBOL_NS(max_98390_ops, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void max_98390_set_codec_conf(struct snd_soc_card *card, int ch)
{
	if (ch == ARRAY_SIZE(max_98390_4spk_codec_conf)) {
		card->codec_conf = max_98390_4spk_codec_conf;
		card->num_configs = ARRAY_SIZE(max_98390_4spk_codec_conf);
	} else {
		card->codec_conf = max_98390_codec_conf;
		card->num_configs = ARRAY_SIZE(max_98390_codec_conf);
	}
}
EXPORT_SYMBOL_NS(max_98390_set_codec_conf, SND_SOC_INTEL_SOF_MAXIM_COMMON);

/*
 * Maxim MAX98357A/MAX98360A
 */
static const struct snd_kcontrol_new max_98357a_kcontrols[] = {
	SOC_DAPM_PIN_SWITCH("Spk"),
};

static const struct snd_soc_dapm_widget max_98357a_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("Spk", NULL),
};

static const struct snd_soc_dapm_route max_98357a_dapm_routes[] = {
	/* speaker */
	{"Spk", NULL, "Speaker"},
};

static struct snd_soc_dai_link_component max_98357a_components[] = {
	{
		.name = MAX_98357A_DEV0_NAME,
		.dai_name = MAX_98357A_CODEC_DAI,
	}
};

static struct snd_soc_dai_link_component max_98360a_components[] = {
	{
		.name = MAX_98360A_DEV0_NAME,
		.dai_name = MAX_98357A_CODEC_DAI,
	}
};

static int max_98357a_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	int ret;

	ret = snd_soc_dapm_new_controls(&card->dapm, max_98357a_dapm_widgets,
					ARRAY_SIZE(max_98357a_dapm_widgets));
	if (ret) {
		dev_err(rtd->dev, "unable to add dapm controls, ret %d\n", ret);
		/* Don't need to add routes if widget addition failed */
		return ret;
	}

	ret = snd_soc_add_card_controls(card, max_98357a_kcontrols,
					ARRAY_SIZE(max_98357a_kcontrols));
	if (ret) {
		dev_err(rtd->dev, "unable to add card controls, ret %d\n", ret);
		return ret;
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, max_98357a_dapm_routes,
				      ARRAY_SIZE(max_98357a_dapm_routes));

	if (ret)
		dev_err(rtd->dev, "unable to add dapm routes, ret %d\n", ret);

	return ret;
}

void max_98357a_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = max_98357a_components;
	link->num_codecs = ARRAY_SIZE(max_98357a_components);
	link->init = max_98357a_init;
}
EXPORT_SYMBOL_NS(max_98357a_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

void max_98360a_dai_link(struct snd_soc_dai_link *link)
{
	link->codecs = max_98360a_components;
	link->num_codecs = ARRAY_SIZE(max_98360a_components);
	link->init = max_98357a_init;
}
EXPORT_SYMBOL_NS(max_98360a_dai_link, SND_SOC_INTEL_SOF_MAXIM_COMMON);

MODULE_DESCRIPTION("ASoC Intel SOF Maxim helpers");
MODULE_LICENSE("GPL");
