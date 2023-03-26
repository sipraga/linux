// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C interface driver for AD24xx A2B transceivers
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/clk.h>

#include <linux/a2b/ad24xx.h>
#include <linux/a2b/a2b.h>

struct ad24xx_i2c {
	struct device *dev;
	struct i2c_client *base_client;
	struct i2c_client *bus_client;
	struct regmap *base_regmap;
	struct regmap *bus_regmap;
	struct a2b_bus a2b_bus;
	struct a2b_node *a2b_node;
	struct mutex mutex;
	struct irq_domain *irqdomain;
	int irq;

	struct clk *sync_clk;
};

#define to_ad24xx_i2c(iface) container_of(iface, struct ad24xx_i2c, a2b_bus)

static bool ad24xx_i2c_private_reg(unsigned int reg)
{
	/*
	 * "Private" registers which are owned by this interface driver should
	 * not be accessed by the constituent A2B drivers.
	 */
	switch (reg) {
	case A2B_NODEADR:
	case A2B_INTSRC:
	case A2B_INTTYPE:
		return true;
	default:
		return false;
	}
}

static int ad24xx_i2c_read_nolock(struct a2b_bus *a2b_bus,
				  const struct a2b_node *node, unsigned int reg,
				  unsigned int *val, int flags)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	unsigned int nodeadr;
	int ret;

	/* Broadcast read is not supported and would make no sense anyway */
	if (flags & A2B_RW_BROADCAST)
		return -EINVAL;

	if (flags & A2B_RW_I2CPERIPHERAL) {
		/* Mains have only one I2C bus and we're using it right now */
		if (node->addr == A2B_MAIN_ADDR)
			return -EINVAL;
	} else if (ad24xx_i2c_private_reg(reg)) {
		return -EACCES;
	}

	/* Main node access */
	if (node->addr == A2B_MAIN_ADDR)
		return regmap_read(ad->base_regmap, reg, val);

	/* Sub node access */
	nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr) |
		  FIELD_PREP(A2B_NODEADR_PERI_MASK,
			     flags & A2B_RW_I2CPERIPHERAL ? 1 : 0);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		return ret;

	ret = regmap_read(ad->bus_regmap, reg, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_read(struct a2b_bus *a2b_bus,
			   const struct a2b_node *node, unsigned int reg,
			   unsigned int *val, int flags)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;

	mutex_lock(&ad->mutex);
	ret = ad24xx_i2c_read_nolock(a2b_bus, node, reg, val, flags);
	mutex_unlock(&ad->mutex);
	return ret;
}

static int ad24xx_i2c_write_nolock(struct a2b_bus *a2b_bus,
			    const struct a2b_node *node, unsigned int reg,
			    unsigned int val, int flags)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	unsigned int nodeadr;
	int ret;

	if (flags & A2B_RW_I2CPERIPHERAL) {
		/* Mains have only one I2C bus and we're using it right now */
		if (!node)
			return -EINVAL;

		/* I2C broadcast writes are not supported by the hardware */
		if (flags & A2B_RW_BROADCAST)
			return -EINVAL;
	} else if (ad24xx_i2c_private_reg(reg)) {
		return -EACCES;
	}

	/* Main node access */
	if (node->addr == A2B_MAIN_ADDR)
		return regmap_write(ad->base_regmap, reg, val);

	/* Sub node access */
	nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr) |
		  FIELD_PREP(A2B_NODEADR_PERI_MASK,
			     flags & A2B_RW_I2CPERIPHERAL ? 1 : 0) |
		  FIELD_PREP(A2B_NODEADR_BRCST_MASK,
			     flags & A2B_RW_BROADCAST ? 1 : 0);

	ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
	if (ret)
		return ret;

	ret = regmap_write(ad->bus_regmap, reg, val);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_write(struct a2b_bus *a2b_bus,
			    const struct a2b_node *node, unsigned int reg,
			    unsigned int val, int flags)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;

	mutex_lock(&ad->mutex);
	ret = ad24xx_i2c_write_nolock(a2b_bus, node, reg, val, flags);
	mutex_unlock(&ad->mutex);
	return ret;
}

static int ad24xx_i2c_xfer(struct a2b_bus *a2b_bus, const struct a2b_node *node,
			   struct i2c_msg *msgs, int num)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	int ret;
	int i;

	/* Mains only have one I2C interface and it operates in slave mode */
	if (node->addr == A2B_MAIN_ADDR)
		return -EINVAL;

	mutex_lock(&ad->mutex);

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];
		unsigned int nodeadr;

		/* Set I2C peripheral address in subordinate node */
		nodeadr = FIELD_PREP(A2B_NODEADR_NODE_MASK, node->addr);

		ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
		if (ret)
			goto out;

		ret = regmap_write(ad->bus_regmap, A2B_CHIP, msg->addr);
		if (ret)
			goto out;

		/* Set peripheral bit */
		nodeadr |= FIELD_PREP(A2B_NODEADR_PERI_MASK, 1);

		ret = regmap_write(ad->base_regmap, A2B_NODEADR, nodeadr);
		if (ret)
			goto out;

		/* Execute raw I2C transfer on the dummy BUS client */
		ret = i2c_transfer_buffer_flags(ad->bus_client, msg->buf,
						msg->len, msg->flags);
		if (ret < 0)
			goto out;
	}

out:
	mutex_unlock(&ad->mutex);

	if (ret < 0)
		return ret;

	return num;
}

static void ad24xx_i2c_lock(struct a2b_bus *a2b_bus)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	mutex_lock(&ad->mutex);
}

static void ad24xx_i2c_unlock(struct a2b_bus *a2b_bus)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	mutex_unlock(&ad->mutex);
}

static int ad24xx_i2c_get_inttype(struct a2b_bus *a2b_bus,
				  unsigned int *val)
{
	struct ad24xx_i2c *ad = to_ad24xx_i2c(a2b_bus);
	return regmap_read(ad->base_regmap, A2B_INTTYPE, val);
}

struct a2b_bus_ops ad24xx_i2c_a2b_bus_ops = {
	.lock = ad24xx_i2c_lock,
	.unlock = ad24xx_i2c_unlock,
	.read = ad24xx_i2c_read,
	.write = ad24xx_i2c_write,
	.i2c_xfer = ad24xx_i2c_xfer,
	.get_inttype = ad24xx_i2c_get_inttype,
	.read_nolock = ad24xx_i2c_read_nolock,
	.write_nolock = ad24xx_i2c_write_nolock,
};

static irqreturn_t ad24xx_i2c_irq_handler(int irq, void *data)
{
	struct ad24xx_i2c *ad = data;
	unsigned int val;
	unsigned int virq = 0;
	int ret;

	ret = regmap_read(ad->base_regmap, A2B_INTSRC, &val);
	if (ret) {
		dev_err_ratelimited(
			ad->dev, "failed to read interrupt source: %d\n", ret);
		return IRQ_NONE;
	}


	if (val & A2B_INTSRC_MSTINT_MASK)
		virq = irq_find_mapping(ad->irqdomain, 15);
	else if (val & A2B_INTSRC_SLVINT_MASK)
		virq = irq_find_mapping(ad->irqdomain,
					val & A2B_INTSRC_INODE_MASK);

	if (!virq)
		return IRQ_NONE;

	handle_nested_irq(virq);

	return IRQ_HANDLED;
}

static const struct irq_chip ad24xx_i2c_irq_chip = {
	.name = "ad24xx-i2c",
};

static int ad24xx_i2c_irqdomain_map(struct irq_domain *irqdomain,
				    unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, irqdomain->host_data);
	irq_set_chip_and_handler(irq, &ad24xx_i2c_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static void ad24xx_i2c_irqdomain_unmap(struct irq_domain *irqdomain,
				       unsigned int irq)
{
	irq_set_nested_thread(irq, 0);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops ad24xx_i2c_irqdomain_ops = {
	.map = ad24xx_i2c_irqdomain_map,
	.unmap = ad24xx_i2c_irqdomain_unmap,
	.xlate = irq_domain_xlate_onecell,
};

static void devm_ad24xx_i2c_release_irqdomain(void *data)
{
	struct irq_domain *irqdomain = data;
	int virq;
	int i;

	for (i = 0; i < 17; i++) {
		virq = irq_find_mapping(irqdomain, i);
		if (virq)
			irq_dispose_mapping(virq);
	}

	irq_domain_remove(irqdomain);
}

static int ad24xx_i2c_irq_setup(struct ad24xx_i2c *ad)
{
	u32 intsize;
	int ret;

	if (!of_property_read_bool(ad->dev->of_node, "interrupt-controller") ||
	    of_property_read_u32(ad->dev->of_node, "#interrupt-cells",
				 &intsize) ||
	    intsize != 1)
		return -EINVAL;

	/* 17 - up to 16 subs and 1 main */
	ad->irqdomain = irq_domain_add_linear(ad->dev->of_node, 17,
					      &ad24xx_i2c_irqdomain_ops, ad);
	if (!ad->irqdomain)
		return -ENOMEM;

	ret = devm_add_action_or_reset(
		ad->dev, devm_ad24xx_i2c_release_irqdomain, ad->irqdomain);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(ad->dev, ad->irq, NULL,
					ad24xx_i2c_irq_handler, IRQF_ONESHOT,
					"ad24xx-i2c", ad);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_i2c_node_setup(struct ad24xx_i2c *ad)
{
	struct device *dev = ad->dev;
	struct a2b_node *node;
	struct device_node *np;
	int ret;

	ad->a2b_bus.dev = dev;
	ad->a2b_bus.ops = &ad24xx_i2c_a2b_bus_ops;
	ad->a2b_bus.priv = ad;

	ret = a2b_register_bus(&ad->a2b_bus);
	if (ret)
		return ret;

	np = of_get_child_by_name(ad->dev->of_node, "main");
	if (!np)
		return -EINVAL;

	node = a2b_bus_of_add_node(&ad->a2b_bus, np, A2B_MAIN_ADDR);
	of_node_put(np);
	if (IS_ERR(node))
		return PTR_ERR(node);

	ad->a2b_node = node;

	return 0;
}

static int ad24xx_i2c_reset(struct ad24xx_i2c *ad)
{
	return regmap_write(ad->base_regmap, A2B_CONTROL,
			    A2B_CONTROL_SOFTRST_MASK);
}

static const struct regmap_config ad24xx_i2c_base_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = 0x9B,
	/* .disable_locking = true, */
	/* .lock = _ad24xx_i2c_lock, */
	/* .unlock = _ad24xx_i2c_unlock, */
};

static const struct regmap_config ad24xx_i2c_bus_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_stride = 1,
	.max_register = 0xFF,
};

static void ad24xx_i2c_remove(struct i2c_client *client)
{
	struct ad24xx_i2c *ad = i2c_get_clientdata(client);
	a2b_unregister_bus(&ad->a2b_bus);
}

static int ad24xx_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np;
	struct ad24xx_i2c *ad;
	struct regmap_config *base_regmap_config;
	struct regmap_config *bus_regmap_config;
	u32 bus_addr;
	int ret;
	int i;

	ad = devm_kzalloc(dev, sizeof(*ad), GFP_KERNEL);
	if (!ad)
		return -ENOMEM;

	base_regmap_config = devm_kmemdup(dev, &ad24xx_i2c_base_regmap_config,
					  sizeof(*base_regmap_config),
					  GFP_KERNEL);
	if (!base_regmap_config)
		return -ENOMEM;

	bus_regmap_config = devm_kmemdup(dev, &ad24xx_i2c_bus_regmap_config,
					  sizeof(*bus_regmap_config),
					  GFP_KERNEL);
	if (!bus_regmap_config)
		return -ENOMEM;

	i2c_set_clientdata(client, ad);
	ad->dev = dev;
	ad->irq = client->irq;
	ad->base_client = client;
	mutex_init(&ad->mutex);

	ad->base_regmap =
		devm_regmap_init_i2c(ad->base_client, base_regmap_config);
	if (IS_ERR(ad->base_regmap))
		return PTR_ERR(ad->base_regmap);

	np = client->dev.of_node;
	if (!np)
		return -EINVAL;

	i = of_property_match_string(np, "reg-names", "bus");
	if (i < 0)
		return -EINVAL;

	ret = of_property_read_u32_index(np, "reg", i, &bus_addr);
	if (ret)
		return ret;

	ad->bus_client =
		devm_i2c_new_dummy_device(dev, client->adapter, bus_addr);
	if (IS_ERR(ad->bus_client))
		return PTR_ERR(ad->bus_client);

	ad->bus_regmap =
		devm_regmap_init_i2c(ad->bus_client, bus_regmap_config);
	if (IS_ERR(ad->bus_regmap))
		return PTR_ERR(ad->bus_regmap);

	ad->sync_clk = devm_clk_get_enabled(dev, "sync");
	if (IS_ERR(ad->sync_clk))
		return PTR_ERR(ad->sync_clk);

	ret = ad24xx_i2c_reset(ad);
	if (ret)
		return ret;

	ret = ad24xx_i2c_irq_setup(ad);
	if (ret)
		return ret;

	ret = ad24xx_i2c_node_setup(ad);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_i2c_of_match_table[] = {
	{ .compatible = "adi,ad2403", },
	{ .compatible = "adi,ad2410", },
	{ .compatible = "adi,ad2425", },
	{ .compatible = "adi,ad2428", },
	{ .compatible = "adi,ad2429", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_i2c_of_match_table);

static const struct i2c_device_id ad24xx_i2c_id_table[] = {
	{ .name = "ad24xx", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ad24xx_i2c_id_table);

static struct i2c_driver ad24xx_i2c_driver = {
	.driver = {
		.name = "ad24xx-i2c",
		.of_match_table = ad24xx_i2c_of_match_table,
	},
	.probe = ad24xx_i2c_probe,
	.remove = ad24xx_i2c_remove,
	.id_table = ad24xx_i2c_id_table,
};
module_i2c_driver(ad24xx_i2c_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx I2C driver");
MODULE_LICENSE("GPL");
