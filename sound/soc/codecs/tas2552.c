/*
 * TI TAS2552 loudspeaker amplifier, Nokia Martini playback support.
 *
 * Register definitions and sequencing follow TI SLAS898B and the upstream
 * TAS2552 driver by Dan Murphy, Copyright (C) 2014 Texas Instruments.
 * This implementation uses the Linux 3.4 ASoC codec API.
 *
 * Licensed under the GNU General Public License, version 2.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define TAS2552_STATUS		0x00
#define TAS2552_CFG1		0x01
#define TAS2552_CFG2		0x02
#define TAS2552_CFG3		0x03
#define TAS2552_DOUT		0x04
#define TAS2552_SERIAL1		0x05
#define TAS2552_SERIAL2		0x06
#define TAS2552_OUTPUT_DATA	0x07
#define TAS2552_PLL1		0x08
#define TAS2552_PLL2		0x09
#define TAS2552_PLL3		0x0a
#define TAS2552_RESERVED_0D	0x0d
#define TAS2552_LIMIT_RATE	0x0e
#define TAS2552_GAIN		0x12
#define TAS2552_VERSION		0x16

#define TAS2552_RESET		BIT(0)
#define TAS2552_SWS		BIT(1)
#define TAS2552_MUTE		BIT(2)
#define TAS2552_PLL_BCLK		BIT(4)
#define TAS2552_CLASSD_EN		BIT(7)
#define TAS2552_BOOST_EN		BIT(6)
#define TAS2552_APT_EN		BIT(5)
#define TAS2552_PLL_EN		BIT(3)
#define TAS2552_LIMIT_EN		BIT(2)
#define TAS2552_CFG2_RESERVED	BIT(0)

struct tas2552_priv {
	struct regmap *regmap;
	struct regulator *vdd;
	int enable_gpio;
};

static int tas2552_power_down(struct regmap *map)
{
	int ret, err;

	/* TI shutdown sequence: SWS before restoring the reserved bits. */
	ret = regmap_update_bits(map, TAS2552_CFG1,
				 TAS2552_SWS | TAS2552_MUTE,
				 TAS2552_SWS | TAS2552_MUTE);
	err = regmap_write(map, TAS2552_CFG2, TAS2552_CFG2_RESERVED);
	if (!ret)
		ret = err;
	err = regmap_update_bits(map, TAS2552_LIMIT_RATE, BIT(5), 0);
	if (!ret)
		ret = err;
	err = regmap_write(map, TAS2552_RESERVED_0D, 0xbe);
	return ret ? ret : err;
}

static int tas2552_power_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct tas2552_priv *tas = snd_soc_codec_get_drvdata(w->codec);
	struct regmap *map = tas->regmap;
	int ret;

	if (event == SND_SOC_DAPM_PRE_PMD)
		return tas2552_power_down(map);

	/* Clocks are enabled by the machine startup before DAPM powers us. */
	ret = regmap_write(map, TAS2552_CFG2, TAS2552_CLASSD_EN |
			   TAS2552_BOOST_EN | TAS2552_APT_EN |
			   TAS2552_PLL_EN | TAS2552_LIMIT_EN |
			   TAS2552_CFG2_RESERVED);
	if (ret)
		goto fail;
	ret = regmap_write(map, TAS2552_RESERVED_0D, 0xc0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(map, TAS2552_LIMIT_RATE, BIT(5), BIT(5));
	if (ret)
		goto fail;
	ret = regmap_update_bits(map, TAS2552_CFG2, TAS2552_CFG2_RESERVED, 0);
	if (ret)
		goto fail;
	ret = regmap_update_bits(map, TAS2552_CFG1,
				 TAS2552_SWS | TAS2552_MUTE, 0);
	if (ret)
		goto fail;
	/* Allow the Class-D channel to start before digital_mute unmutes it. */
	msleep(8);
	return 0;

fail:
	dev_err(w->codec->dev, "amplifier power-up failed: %d\n", ret);
	tas2552_power_down(map);
	return ret;
}

static const struct snd_kcontrol_new tas2552_amp_switch =
	SOC_DAPM_SINGLE("Switch", SND_SOC_NOPM, 0, 1, 0);

static const struct snd_soc_dapm_widget tas2552_widgets[] = {
	SND_SOC_DAPM_AIF_IN("TAS2552 DAC IN", "TAS2552 Playback", 0,
			    SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_SWITCH("TAS2552 Amp", SND_SOC_NOPM, 0, 0,
			    &tas2552_amp_switch),
	SND_SOC_DAPM_OUT_DRV_E("TAS2552 ClassD", SND_SOC_NOPM, 0, 0,
			       NULL, 0, tas2552_power_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("TAS2552 OUT"),
};

static const struct snd_soc_dapm_route tas2552_routes[] = {
	{ "TAS2552 Amp", "Switch", "TAS2552 DAC IN" },
	{ "TAS2552 ClassD", NULL, "TAS2552 Amp" },
	{ "TAS2552 OUT", NULL, "TAS2552 ClassD" },
};

static const DECLARE_TLV_DB_SCALE(tas2552_gain_tlv, -700, 100, 0);

static const struct snd_kcontrol_new tas2552_controls[] = {
	SOC_SINGLE_TLV("TAS2552 Playback Volume", TAS2552_GAIN, 0, 31, 0,
		       tas2552_gain_tlv),
};

static int tas2552_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	/* PLL coefficients below are for BCLK = 48000 * 2 * 16. */
	if (params_rate(params) != 48000 || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;
	return 0;
}

static int tas2552_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBS_CFS ||
	    (fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	return 0;
}

static int tas2552_mute(struct snd_soc_dai *dai, int mute)
{
	struct tas2552_priv *tas = snd_soc_codec_get_drvdata(dai->codec);

	return regmap_update_bits(tas->regmap, TAS2552_CFG1, TAS2552_MUTE,
				 mute ? TAS2552_MUTE : 0);
}

static void tas2552_shutdown(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct tas2552_priv *tas = snd_soc_codec_get_drvdata(dai->codec);
	int ret;

	/* soc_pcm_close calls codec shutdown before machine clock shutdown. */
	ret = tas2552_power_down(tas->regmap);
	if (ret)
		dev_err(dai->codec->dev, "amplifier shutdown failed: %d\n", ret);
}

static const struct snd_soc_dai_ops tas2552_dai_ops = {
	.hw_params = tas2552_hw_params,
	.set_fmt = tas2552_set_fmt,
	.digital_mute = tas2552_mute,
	.shutdown = tas2552_shutdown,
};

static struct snd_soc_dai_driver tas2552_dai = {
	.name = "tas2552-amplifier",
	.playback = {
		.stream_name = "TAS2552 Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &tas2552_dai_ops,
};

static int tas2552_codec_probe(struct snd_soc_codec *codec)
{
	struct tas2552_priv *tas = snd_soc_codec_get_drvdata(codec);

	codec->control_data = tas->regmap;
	return snd_soc_codec_set_cache_io(codec, 8, 8, SND_SOC_REGMAP);
}

static struct snd_soc_codec_driver tas2552_codec = {
	.probe = tas2552_codec_probe,
	.controls = tas2552_controls,
	.num_controls = ARRAY_SIZE(tas2552_controls),
	.dapm_widgets = tas2552_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tas2552_widgets),
	.dapm_routes = tas2552_routes,
	.num_dapm_routes = ARRAY_SIZE(tas2552_routes),
};

static const struct regmap_config tas2552_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x19,
	.cache_type = REGCACHE_NONE,
};

/* Configure while muted and in software shutdown. See SLAS898B section 8.3. */
static int tas2552_init(struct regmap *map)
{
	static const struct reg_default regs[] = {
		{ TAS2552_CFG1, TAS2552_PLL_BCLK | TAS2552_SWS | TAS2552_MUTE },
		{ TAS2552_CFG2, TAS2552_CFG2_RESERVED },
		/* Digital input, average L/R, 48 kHz; I/V output in I2S mode. */
		{ TAS2552_CFG3, 0x40 | 0x18 | 0x05 },
		{ TAS2552_DOUT, 0x04 },
		{ TAS2552_OUTPUT_DATA, 0x3f }, /* Feedback output disabled (Hi-Z). */
		{ TAS2552_SERIAL1, 0x00 }, /* I2S slave, 16-bit, 32 BCLK/frame. */
		{ TAS2552_SERIAL2, 0x00 },
		/* PLL: 0.5 * 1.536 MHz * J(32) / 2^P(0) = 24.576 MHz. */
		{ TAS2552_PLL1, 32 },
		{ TAS2552_PLL2, 0 },
		{ TAS2552_PLL3, 0 },
		{ TAS2552_GAIN, 7 }, /* 0 dB initial gain. */
	};
	int i, ret;

	ret = regmap_write(map, TAS2552_CFG1, TAS2552_RESET);
	if (ret)
		return ret;
	msleep(2);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = regmap_write(map, regs[i].reg, regs[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

static int __devinit tas2552_probe(struct i2c_client *client,
				   const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct tas2552_priv *tas;
	unsigned int version;
	int ret;

	tas = devm_kzalloc(dev, sizeof(*tas), GFP_KERNEL);
	if (!tas)
		return -ENOMEM;
	i2c_set_clientdata(client, tas);
	tas->enable_gpio = of_get_named_gpio(dev->of_node, "enable-gpios", 0);
	if (tas->enable_gpio < 0)
		return tas->enable_gpio;
	tas->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(tas->vdd))
		return PTR_ERR(tas->vdd);
	ret = gpio_request_one(tas->enable_gpio, GPIOF_OUT_INIT_LOW,
			       "tas2552 IHF_EN");
	if (ret)
		return ret;
	ret = regulator_set_voltage(tas->vdd, 1800000, 1800000);
	if (ret)
		goto free_gpio;
	ret = regulator_enable(tas->vdd);
	if (ret)
		goto free_gpio;
	gpio_set_value_cansleep(tas->enable_gpio, 1);
	msleep(2); /* EN high to first I2C access: minimum 1 ms. */

	tas->regmap = devm_regmap_init_i2c(client, &tas2552_regmap);
	if (IS_ERR(tas->regmap)) {
		ret = PTR_ERR(tas->regmap);
		goto disable;
	}
	ret = regmap_read(tas->regmap, TAS2552_VERSION, &version);
	if (ret) {
		dev_err(dev, "no amplifier response at 0x%02x: %d\n",
			client->addr, ret);
		goto disable;
	}
	if (version != 0x08) {
		dev_err(dev, "unexpected TAS2552 version 0x%02x\n", version);
		ret = -ENODEV;
		goto disable;
	}
	ret = tas2552_init(tas->regmap);
	if (ret)
		goto disable;
	ret = snd_soc_register_codec(dev, &tas2552_codec, &tas2552_dai, 1);
	if (ret)
		goto disable;
	dev_info(dev, "TAS2552 version 0x%02x, 48 kHz I2S/BCLK playback ready\n",
		 version);
	return 0;

disable:
	gpio_set_value_cansleep(tas->enable_gpio, 0);
	regulator_disable(tas->vdd);
free_gpio:
	gpio_free(tas->enable_gpio);
	return ret;
}

static int __devexit tas2552_remove(struct i2c_client *client)
{
	struct tas2552_priv *tas = i2c_get_clientdata(client);

	snd_soc_unregister_codec(&client->dev);
	tas2552_power_down(tas->regmap);
	gpio_set_value_cansleep(tas->enable_gpio, 0);
	regulator_disable(tas->vdd);
	gpio_free(tas->enable_gpio);
	return 0;
}

static const struct i2c_device_id tas2552_ids[] = {
	{ "tas2552", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tas2552_ids);

static const struct of_device_id tas2552_of_match[] = {
	{ .compatible = "ti,tas2552" },
	{ }
};
MODULE_DEVICE_TABLE(of, tas2552_of_match);

static struct i2c_driver tas2552_driver = {
	.driver = {
		.name = "tas2552",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(tas2552_of_match),
	},
	.probe = tas2552_probe,
	.remove = __devexit_p(tas2552_remove),
	.id_table = tas2552_ids,
};
module_i2c_driver(tas2552_driver);

MODULE_DESCRIPTION("TI TAS2552 Martini loudspeaker codec");
MODULE_LICENSE("GPL v2");
