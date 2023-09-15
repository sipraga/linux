// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx I2C master driver
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_irq.h>

struct ad24xx_i2c_master {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct i2c_adapter adap;
};

static int ad24xx_i2c_master_xfer(struct i2c_adapter *adap,
				  struct i2c_msg *msgs, int num)
{
	struct ad24xx_i2c_master *adim = i2c_get_adapdata(adap);
	struct a2b_node *node = adim->node;

	return a2b_node_i2c_xfer(node, msgs, num);
}

static u32 ad24xx_i2c_master_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_adapter_quirks ad24xx_i2c_master_quirks = {
	.flags = I2C_AQ_COMB | I2C_AQ_COMB_SAME_ADDR,
};

static const struct i2c_algorithm ad24xx_i2c_master_algo = {
	.master_xfer = ad24xx_i2c_master_xfer,
	.functionality = ad24xx_i2c_master_functionality,
};

static int ad24xx_i2c_master_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	struct device_node *np = dev->of_node;
	struct ad24xx_i2c_master *adim;
	unsigned int val = 0;
	u32 bus_speed;
	int ret;

	adim = devm_kzalloc(dev, sizeof(*adim), GFP_KERNEL);
	if (!adim)
		return -ENOMEM;

	adim->dev = dev;
	adim->func = func;
	adim->node = func->node;

	adim->adap.owner = THIS_MODULE;
	adim->adap.algo = &ad24xx_i2c_master_algo;
	/*
	 * FIXME/HELP WANTED: This horrible parent assignment fixes a lockdep
	 * warning. Namely, if we were to set adap.dev.parent = dev; then the
	 * calculation in i2c_adapter_depth() would be incorrect due to the
	 * adapter parent not being an i2c_adapter. Instead, set the adapter
	 * parent to be the i2c_adapter on which the main node is
	 * connected. Surely there's a more elegant solution...
	 */
	adim->adap.dev.parent = dev->parent->parent->parent->parent;
	adim->adap.dev.of_node = dev->of_node;
	adim->adap.quirks = &ad24xx_i2c_master_quirks;
	strscpy(adim->adap.name, dev_name(dev), sizeof(adim->adap.name));
	i2c_set_adapdata(&adim->adap, adim);

	ret = of_property_read_u32(np, "clock-frequency", &bus_speed);
	if (ret)
		bus_speed = I2C_MAX_STANDARD_MODE_FREQ;

	if (bus_speed != I2C_MAX_STANDARD_MODE_FREQ &&
	    bus_speed != I2C_MAX_FAST_MODE_FREQ)
		return -EINVAL;

	val |= FIELD_PREP(A2B_I2CCFG_DATARATE_MASK,
			  bus_speed == I2C_MAX_FAST_MODE_FREQ ? 1 : 0);
	val |= FIELD_PREP(A2B_I2CCFG_FRAMERATE_MASK,
			  func->node->sff == A2B_SFF_44100 ? 1 : 0);

	ret = a2b_node_write(func->node, A2B_I2CCFG, val);
	if (ret)
		return ret;

	ret = devm_i2c_add_adapter(dev, &adim->adap);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_i2c_master_of_match_table[] = {
	{
		.compatible = "adi,ad2403-i2c-master",
	},
	{
		.compatible = "adi,ad2410-i2c-master",
	},
	{
		.compatible = "adi,ad2425-i2c-master",
	},
	{
		.compatible = "adi,ad2428-i2c-master",
	},
	{
		.compatible = "adi,ad2429-i2c-master",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_i2c_master_of_match_table);

static struct a2b_driver ad24xx_i2c_master_driver = {
	.driver = {
		.name = "ad24xx-i2c-master",
		.of_match_table = ad24xx_i2c_master_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_i2c_master_probe,
};
module_a2b_driver(ad24xx_i2c_master_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx I2C master driver");
MODULE_LICENSE("GPL");
