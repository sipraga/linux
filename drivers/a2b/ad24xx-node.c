// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx A2B transceiver node driver
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * Analog Devices Inc. documentation cited in some of the comments below:
 *
 * [1] AD2420(W)/6(W)/7(W)/8(W)/9(W) Automotive Audio Bus A2B Transceiver
 *     Technical Reference, Revision 1.1, October 2019, Part Number 82-100138-01
 *
 * [2] Datasheet for AD2420(W)/AD2426(W)/AD2427(W)/AD2428(W)/AD2429(W) Rev. C,
 *     July 2021
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_irq.h>

struct ad24xx_node {
	struct device *dev;
	struct a2b_node *node;
	struct irq_domain *irqdomain;
	int irq;
	struct completion running_completion;
	struct completion discovery_completion;
	struct a2b_func *func_gpio;
	struct a2b_func *func_codec;
	struct a2b_func *func_i2c;
};

static int of_a2b_parse_tdm_slot_size(struct device_node *np,
				      enum a2b_tdm_slot_size *tdm_slot_size)
{
	u32 slot_size;
	int ret;

	ret = of_property_read_u32(np, "adi,tdm-slot-size", &slot_size);
	if (ret)
		return ret;

	if (slot_size == 16)
		*tdm_slot_size = A2B_TDMSS_16;
	else if (slot_size == 32)
		*tdm_slot_size = A2B_TDMSS_32;
	else
		return -EINVAL;

	return 0;
}

static int of_a2b_parse_tdm_mode(struct device_node *np,
				 enum a2b_tdm_mode *tdm_mode)
{
	u32 mode;
	int ret;

	ret = of_property_read_u32(np, "adi,tdm-mode", &mode);
	if (ret)
		return ret;

	if (mode == 2)
		*tdm_mode = A2B_TDMMODE_2;
	else if (mode == 4)
		*tdm_mode = A2B_TDMMODE_4;
	else if (mode == 8)
		*tdm_mode = A2B_TDMMODE_8;
	else if (mode == 12)
		*tdm_mode = A2B_TDMMODE_12;
	else if (mode == 16)
		*tdm_mode = A2B_TDMMODE_16;
	else if (mode == 20)
		*tdm_mode = A2B_TDMMODE_20;
	else if (mode == 24)
		*tdm_mode = A2B_TDMMODE_24;
	else if (mode == 32)
		*tdm_mode = A2B_TDMMODE_32;
	else
		return -EINVAL;

	return 0;
}

static const struct irq_chip ad24xx_node_irq_chip = {
	.name = "ad24xx-node",
};

static int ad24xx_node_irqdomain_map(struct irq_domain *irqdomain,
				     unsigned int irq, irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, irqdomain->host_data);
	irq_set_chip_and_handler(irq, &ad24xx_node_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static void ad24xx_node_irqdomain_unmap(struct irq_domain *irqdomain,
					unsigned int irq)
{
	irq_set_nested_thread(irq, 0);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static int ad24xx_node_irqdomain_alloc(struct irq_domain *irqdomain,
				       unsigned int virq, unsigned int nr_irqs,
				       void *data)
{
	struct ad24xx_node *adn = irqdomain->host_data;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq = fwspec->param[0];

	if (nr_irqs != 1)
		return -EINVAL;

	if (hwirq > 8)
		return -EINVAL;

	return irq_domain_set_hwirq_and_chip(irqdomain, virq, hwirq,
					     &ad24xx_node_irq_chip, adn);
}

static const struct irq_domain_ops ad24xx_node_irqdomain_ops = {
	.alloc = ad24xx_node_irqdomain_alloc,
	.free = irq_domain_free_irqs_common,
	.map = ad24xx_node_irqdomain_map,
	.unmap = ad24xx_node_irqdomain_unmap,
	.xlate = irq_domain_xlate_onecell,
};

static void devm_ad24xx_node_release_irqdomain(void *data)
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

static irqreturn_t ad24xx_node_irq_handler(int irq, void *data)
{
	struct ad24xx_node *adn = data;
	struct a2b_node *node = adn->node;
	struct device *dev = adn->dev;
	unsigned int inttype;
	unsigned int virq;
	int ret;

	ret = node->bus->ops->get_inttype(node->bus, &inttype);
	if (ret) {
		dev_err_ratelimited(adn->dev,
				    "failed to get interrupt type: %d\n", ret);
		return IRQ_NONE;
	}

	dev_dbg_ratelimited(dev, "received interrupt of type %d\n", inttype);

	switch (inttype) {
	case A2B_INTTYPE_HDCNTERR:
	case A2B_INTTYPE_DDERR:
	case A2B_INTTYPE_CRCERR:
	case A2B_INTTYPE_DPERR:
	case A2B_INTTYPE_BECOVF:
	case A2B_INTTYPE_SRFERR:
	case A2B_INTTYPE_SRFCRCERR:
	case A2B_INTTYPE_PWRERR_0:
	case A2B_INTTYPE_PWRERR_1:
	case A2B_INTTYPE_PWRERR_2:
	case A2B_INTTYPE_PWRERR_3:
	case A2B_INTTYPE_PWRERR_4:
	case A2B_INTTYPE_PWRERR_5:
	case A2B_INTTYPE_I2CERR:
	case A2B_INTTYPE_ICRCERR:
	case A2B_INTTYPE_PWRERR_6:
	case A2B_INTTYPE_PWRERR_7:
	case A2B_INTTYPE_IRQMSGERR:
	case A2B_INTTYPE_STARTUPERR:
	case A2B_INTTYPE_SLVINTTYPERR:
		/* Error IRQ */
		a2b_node_report_error(node, inttype);
		return IRQ_HANDLED;
	case A2B_INTTYPE_IO0PND:
	case A2B_INTTYPE_IO1PND:
	case A2B_INTTYPE_IO2PND:
	case A2B_INTTYPE_IO3PND:
	case A2B_INTTYPE_IO4PND:
	case A2B_INTTYPE_IO5PND:
	case A2B_INTTYPE_IO6PND:
	case A2B_INTTYPE_IO7PND:
		/* GPIO IRQ */
		virq = irq_find_mapping(adn->irqdomain,
					inttype - A2B_INTTYPE_IO0PND);
		if (virq)
			handle_nested_irq(virq);
		return IRQ_NONE;
	case A2B_INTTYPE_DSCDONE:
		/* Discovery done IRQ */
		complete(&adn->discovery_completion);
		return IRQ_HANDLED;
	case A2B_INTTYPE_MBOX0FULL:
	case A2B_INTTYPE_MBOX0EMPTY:
	case A2B_INTTYPE_MBOX1FULL:
	case A2B_INTTYPE_MBOX1EMPTY:
		/* Mailbox IRQ - unimplemented */
		dev_info(dev, "unhandled mailbox interrupt %d\n", inttype);
		return IRQ_NONE;
	case A2B_INTTYPE_STBYDONE:
		/* Standby IRQ - unimplemented */
		dev_info(dev, "unhandled standby interrupt %d\n", inttype);
		return IRQ_NONE;
	case A2B_INTTYPE_MSTR_RUNNING:
		/* Master (main) running IRQ */
		complete(&adn->running_completion);
		return IRQ_HANDLED;
	default:
		dev_warn(dev, "unhandled unknown interrupt %d\n", inttype);
		return IRQ_NONE;
	}
}

static int ad24xx_node_setup_i2sgcfg(struct ad24xx_node *adn)
{
	struct a2b_node *node = adn->node;
	unsigned int val = 0;
	int ret;

	val |= FIELD_PREP(A2B_I2SGCFG_TDMMODE_MASK, node->tdm_mode);
	val |= FIELD_PREP(A2B_I2SGCFG_RXONDTX1_MASK, node->rx_on_dtx1);
	val |= FIELD_PREP(A2B_I2SGCFG_TDMSS_MASK, node->tdm_slot_size);
	val |= FIELD_PREP(A2B_I2SGCFG_ALT_MASK, node->alternating_sync);
	val |= FIELD_PREP(A2B_I2SGCFG_EARLY_MASK, node->early_sync);
	val |= FIELD_PREP(A2B_I2SGCFG_INV_MASK, node->invert_sync);

	ret = node->bus->ops->write(node->bus, node, A2B_I2SGCFG, val, 0);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_node_setup(struct a2b_node *node)
{
	struct ad24xx_node *adn = node->priv;
	struct device_node *np;
	unsigned int vendor, product, version;
	unsigned long timeout;
	int ret;

	/* Identify */
	ret = node->bus->ops->read(node->bus, node, A2B_VENDOR, &vendor, 0);
	if (ret)
		return ret;

	ret = node->bus->ops->read(node->bus, node, A2B_PRODUCT, &product, 0);
	if (ret)
		return ret;

	ret = node->bus->ops->read(node->bus, node, A2B_VERSION, &version, 0);
	if (ret)
		return ret;

	dev_err(&node->dev,
		"new %s node vendor 0x%02x prod 0x%02x ver 0x%02x\n",
		is_a2b_main(node) ? "main" : "subordinate", vendor, product,
		version);

	/* IRQ domain for GPIOs */
	adn->irqdomain = irq_domain_add_linear(adn->dev->of_node, 8,
					       &ad24xx_node_irqdomain_ops, adn);
	if (!adn->irqdomain)
		return -ENOMEM;

	ret = devm_add_action_or_reset(
		adn->dev, devm_ad24xx_node_release_irqdomain, adn->irqdomain);
	if (ret)
		return ret;

	/* IRQ */
	adn->irq = of_irq_get(adn->dev->of_node, 0);
	if (adn->irq <= 0)
		return -EINVAL;

	ret = devm_request_threaded_irq(adn->dev, adn->irq, NULL,
					ad24xx_node_irq_handler, IRQF_ONESHOT,
					"ad24xx-node", adn);
	if (ret)
		return ret;

	/*
	 * Perform a software reset - but only on the main node, as doing this
	 * on subordinate nodes will require them to be re-discovered.
	 */
	if (is_a2b_main(node)) {
		ret = node->bus->ops->write(node->bus, node, A2B_CONTROL,
					    A2B_CONTROL_SOFTRST_MASK, 0);
		if (ret)
			return ret;
	}

	/* Enable interrupts */
	ret = node->bus->ops->write(node->bus, node, A2B_INTMSK0, 0xFF, 0);
	if (ret)
		return ret;

	ret = node->bus->ops->write(node->bus, node, A2B_INTMSK1, 0xFF, 0);
	if (ret)
		return ret;

	if (is_a2b_main(node)) {
		/*
		 * Enable master (main) bit and wait for the transceiver to lock
		 * its PLL to the received SYNC signal.
		 */
		ret = node->bus->ops->write(node->bus, node, A2B_CONTROL,
					    A2B_CONTROL_MSTR_MASK, 0);
		if (ret)
			return ret;

		/*
		 * Per the datasheet [2] Table 3, "Clock and Reset Timing (A2B
		 * Master)", the typical PLL Lock Time t_PLK is 7.5 ms. Wait 10
		 * ms to be on the safe side and avoid spurious timeouts.
		 */
		timeout = wait_for_completion_interruptible_timeout(
			&adn->running_completion, msecs_to_jiffies(10));
		reinit_completion(&adn->running_completion);
		if (timeout < 0)
			return timeout;
		else if (timeout == 0)
			return -ETIMEDOUT;

		/*
		 * Enable main-node-only interrupts, ...
		 *
		 * but NOT I2C Error interrupts, as we should expect the error
		 * to be reported via the I2C adapter associated with the BUS
		 * client of the main node. This prevents many spurious
		 * interrupts during e.g. i2cdetect -r.
		 *
		 * TODO: Double check that the above is indeed OK.
		 */
		ret = node->bus->ops->write(node->bus, node, A2B_INTMSK2, 0x0D,
					    0);
		if (ret)
			return ret;
	}

	/*
	 * Set the global I2S cnofiguration. For main nodes, the Technical
	 * Reference [1] is clear that this register must be set before
	 * discovery and must not be modified thereafter. For subordinate nodes
	 * there is no such restriction.
	 */
	ret = ad24xx_node_setup_i2sgcfg(adn);
	if (ret)
		return ret;

	/* Register optional transceiver functions with the core */
	np = of_get_child_by_name(node->dev.of_node, "gpio");
	if (np)
		adn->func_gpio = a2b_node_of_add_func(node, np);
	of_node_put(np);
	if (IS_ERR(adn->func_gpio))
		return PTR_ERR(adn->func_gpio);

	np = of_get_child_by_name(node->dev.of_node, "codec");
	if (np)
		adn->func_codec = a2b_node_of_add_func(node, np);
	of_node_put(np);
	if (IS_ERR(adn->func_codec)) {
		ret = PTR_ERR(adn->func_codec);
		goto err_codec;
	}

	np = of_get_child_by_name(node->dev.of_node, "i2c");
	if (np)
		adn->func_i2c = a2b_node_of_add_func(node, np);
	of_node_put(np);
	if (IS_ERR(adn->func_i2c)) {
		ret = PTR_ERR(adn->func_i2c);
		goto err_i2c;
	}

	return 0;

	/* Unregister optional functions on error */
err_i2c:
	if (adn->func_codec)
		device_unregister(&adn->func_codec->dev);
err_codec:
	if (adn->func_gpio)
		device_unregister(&adn->func_gpio->dev);

	return ret;
}

static void ad24xx_node_teardown(struct a2b_node *node)
{
	struct ad24xx_node *adn = node->priv;

	// TODO: Ugly. Ought to be moved to the core.
	if (adn->func_i2c)
		device_unregister(&adn->func_i2c->dev);
	if (adn->func_codec)
		device_unregister(&adn->func_codec->dev);
	if (adn->func_gpio)
		device_unregister(&adn->func_gpio->dev);
}

static int ad24xx_node_set_respcycs(struct a2b_node *node,
				    unsigned int respcycs)
{
	int ret;

	dev_dbg(&node->dev, "set RESPCYCS %d\n", respcycs);

	ret = node->bus->ops->write(node->bus, node, A2B_RESPCYCS, respcycs, 0);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_node_set_switching(struct a2b_node *node, unsigned int value)
{
	int ret;

	ret = node->bus->ops->write(node->bus, node, A2B_SWCTL, value, 0);
	if (ret)
		return ret;

	return 0;
}

static int ad24xx_node_discover(struct a2b_node *node, unsigned int respcycs)
{
	struct ad24xx_node *adn = node->priv;
	int ret;
	unsigned long timeout;

	ret = node->bus->ops->write(node->bus, node, A2B_DISCVRY, respcycs, 0);
	if (ret)
		return ret;

	timeout = wait_for_completion_interruptible_timeout(
		&adn->discovery_completion, msecs_to_jiffies(350));
	reinit_completion(&adn->discovery_completion);
	if (timeout < 0)
		return timeout;
	else if (timeout == 0)
		return 1;

	return 0;
}

static int ad24xx_new_structure(struct a2b_node *node)
{
	unsigned int val;
	int ret;

	val = FIELD_PREP(A2B_CONTROL_MSTR_MASK, 1) |
	      FIELD_PREP(A2B_CONTROL_NEWSTRCT_MASK, 1);

	ret = node->bus->ops->write(node->bus, node, A2B_CONTROL, val, 0);
	if (ret)
		return ret;

	return 0;
}

static struct a2b_node_ops ad24xx_sub_ops = {
	.setup = ad24xx_node_setup,
	.teardown = ad24xx_node_teardown,
	.set_respcycs = ad24xx_node_set_respcycs,
	.set_switching = ad24xx_node_set_switching,
};

static struct a2b_node_ops ad24xx_main_ops = {
	.setup = ad24xx_node_setup,
	.teardown = ad24xx_node_teardown,
	.set_respcycs = ad24xx_node_set_respcycs,
	.set_switching = ad24xx_node_set_switching,
	.discover = ad24xx_node_discover,
	.new_structure = ad24xx_new_structure,
};

static int ad24xx_node_probe(struct device *dev)
{
	struct a2b_node *node = to_a2b_node(dev);
	struct ad24xx_node *adn;
	int ret;

	adn = devm_kzalloc(dev, sizeof(*adn), GFP_KERNEL);
	if (!adn)
		return -ENOMEM;

	if (node->addr == A2B_MAIN_ADDR) {
		struct device_node *np = dev->of_node;

		node->ops = &ad24xx_main_ops;

		ret = of_a2b_parse_tdm_mode(np, &node->tdm_mode);
		if (ret)
			return -EINVAL;

		ret = of_a2b_parse_tdm_slot_size(np, &node->tdm_slot_size);
		if (ret)
			return -EINVAL;

		if (of_find_property(np, "adi,invert-sync", NULL))
			node->invert_sync = 1;
		if (of_find_property(np, "adi,early-sync", NULL))
			node->early_sync = 1;
		if (of_find_property(np, "adi,alternating-sync", NULL))
			node->alternating_sync = 1;
		if (of_find_property(np, "adi,rx-on-dtx1", NULL))
			node->rx_on_dtx1 = 1;

		// TODO: SLOTFMT is hardcoded to 32 bit uncompressed up/down for
		// now. Should be made into kcontrols or DT properties, possibly
		// on the codec to be requested via the slots API?
		node->upfmt = 0;
		node->dnfmt = 0;
		node->upss = 6;
		node->dnss = 6;
	} else {
		struct device_node *np = dev->of_node;
		struct a2b_node *main = node->bus->nodes[A2B_MAIN_ADDR];

		node->ops = &ad24xx_sub_ops;

		/* Inherit main node TDM settings if not present */
		if (of_a2b_parse_tdm_mode(np, &node->tdm_mode))
			node->tdm_mode = main->tdm_mode;
		if (of_a2b_parse_tdm_slot_size(np, &node->tdm_slot_size))
			node->tdm_slot_size = main->tdm_slot_size;

		/* These configurations can vary on subordinate nodes */
		if (of_find_property(np, "adi,invert-sync", NULL))
			node->invert_sync = 1;
		if (of_find_property(np, "adi,early-sync", NULL))
			node->early_sync = 1;
		if (of_find_property(np, "adi,alternating-sync", NULL))
			node->alternating_sync = 1;
		if (of_find_property(np, "adi,rx-on-dtx1", NULL))
			node->rx_on_dtx1 = 1;
	}

	node->priv = adn;

	adn->dev = dev;
	adn->node = node;
	init_completion(&adn->running_completion);
	init_completion(&adn->discovery_completion);

	ret = a2b_register_node(node);
	if (ret)
		return ret;

	return 0;
}

static void ad24xx_node_remove(struct device *dev)
{
	struct a2b_node *node = to_a2b_node(dev);

	a2b_unregister_node(node);
}

static const struct of_device_id ad24xx_node_of_match_table[] = {
	{
		.compatible = "adi,ad2403",
		.data = &a2b_chip_info[A2B_AD2403],
	},
	{
		.compatible = "adi,ad2410",
		.data = &a2b_chip_info[A2B_AD2410],
	},
	{
		.compatible = "adi,ad2425",
		.data = &a2b_chip_info[A2B_AD2425],
	},
	{
		.compatible = "adi,ad2428",
		.data = &a2b_chip_info[A2B_AD2428],
	},
	{
		.compatible = "adi,ad2429",
		.data = &a2b_chip_info[A2B_AD2429],
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_node_of_match_table);

static struct a2b_driver ad24xx_node_driver = {
	.driver = {
		.name = "ad24xx-node",
		.of_match_table = ad24xx_node_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_node_probe,
	.remove = ad24xx_node_remove,
};
module_a2b_driver(ad24xx_node_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx A2B transceiver node driver");
MODULE_LICENSE("GPL");
