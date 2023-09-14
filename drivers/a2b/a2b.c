// SPDX-License-Identifier: GPL-2.0-only
/*
 * A2B driver core
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

#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/a2b/a2b.h>
#include <linux/version.h>

static bool is_registered;
static DEFINE_IDA(a2b_ida);

/**
 * MISC
 **/

const struct a2b_chip_info a2b_chip_info[A2B_NUM_CHIPS] = {
	[A2B_AD2403] = {
		.caps = A2B_CHIP_CAP_MAIN,
		.max_subs = 8,
	},
	[A2B_AD2410] = {
		.caps = A2B_CHIP_CAP_MAIN,
		.max_subs = 8,
	},
	[A2B_AD2425] = {
		.caps = A2B_CHIP_CAP_MAIN,
		.max_subs = 10,
	},
	[A2B_AD2428] = {
		.caps = A2B_CHIP_CAP_MAIN,
		.max_subs = 10,
	},
	[A2B_AD2429] = {
		.caps = A2B_CHIP_CAP_MAIN,
		.max_subs = 2,
	},
};
EXPORT_SYMBOL_GPL(a2b_chip_info);

static const char *a2b_error_to_string(enum a2b_error error)
{
	switch (error) {
	case A2B_HDCNTERR:
		return "HDCNTERR (header count error)";
	case A2B_DDERR:
		return "DDERR (data decoding error)";
	case A2B_CRCERR:
		return "CRCERR (CRC error)";
	case A2B_DPERR:
		return "DPERR (data parity error)";
	case A2B_BECOVF:
		return "BECOVF (bit error counter overflow)";
	case A2B_SRFERR:
		return "SRFERR (SRF miss error)";
	case A2B_SRFCRCERR:
		return "SRFCRCERR (SRF CRC error)";
	case A2B_PWRERR_0:
		return "PWRERR (positive terminal BP shorted to GND)";
	case A2B_PWRERR_1:
		return "PWRERR (negative terminal BN shorted to VBAT)";
	case A2B_PWRERR_2:
		return "PWRERR (BP shorted to BN)";
	case A2B_PWRERR_3:
		return "PWRERR (cable disconnected/open circuit/wrong port)";
	case A2B_PWRERR_4:
		return "PWRERR (cable is reverse connected/wrong port)";
	case A2B_PWRERR_5:
		return "PWRERR (undetermined fault)";
	case A2B_I2CERR:
		return "I2CERR (I2C error)";
	case A2B_ICRCERR:
		return "ICRCERR (interrupt CRC error)";
	case A2B_PWRERR_6:
		return "PWRERR (non-localized negative terminal BN short to GND)";
	case A2B_PWRERR_7:
		return "PWRERR (non-localized positive terminal BP short to VBAT)";
	case A2B_IRQMSGERR:
		return "IRQMSGERR (interrupt messaging error)";
	case A2B_STARTUPERR:
		return "STARTUPERR (startup error - return to factory)";
	case A2B_SLVINTTYPERR:
		return "SLVINTTYPERR (slave INTTYPE read error)";
	default:
		return "unknown error";
	};
}

/**
 * A2B BUS
 **/

#define __a2b_bus_for_each_node(__bus, __node, __i) \
	__i = 0;                                    \
	while (__i < A2B_MAX_NODES && (__node = __bus->nodes[__i++]))

#define __a2b_bus_for_each_sub_node(__bus, __node, __i) \
	__i = A2B_MAIN_ADDR + 1;                        \
	while (__i < A2B_MAX_NODES && (__node = __bus->nodes[__i++]))

static struct a2b_node *__a2b_bus_main_node(struct a2b_bus *bus)
{
	return bus->nodes[A2B_MAIN_ADDR];
}

static struct a2b_node *__a2b_bus_last_node(struct a2b_bus *bus)
{
	struct a2b_node *last = NULL;
	struct a2b_node *node;
	int i;

	__a2b_bus_for_each_node(bus, node, i)
		last = node;

	return last;
}

/* From [1] Table 9-1: A2B Master Node Response Offset (RESPOFFS) */
static const unsigned int a2b_respoffs[A2B_TDMMODE_END][A2B_TDMSS_END] = {
	[A2B_TDMMODE_2] = { 245, 238 },
	[A2B_TDMMODE_4] = { 248, 245 },
	[A2B_TDMMODE_8] = { 248, 248 },
	[A2B_TDMMODE_12] = { 248, 248 },
	[A2B_TDMMODE_16] = { 248, 248 },
	[A2B_TDMMODE_20] = { 248, 248 },
	[A2B_TDMMODE_24] = { 248, 248 },
	[A2B_TDMMODE_32] = { 248, 248 },
};

/* Look-up table: [FMT][SIZE] -> A2B bus bits, cf. [1] Table 3-2 */
static const unsigned int a2b_slot_bits[2][8] = {
	[0] = {
		[0] =  9, /* 8-bit w/o compression; parity */
		[1] = 13, /* 12-bit w/o compression; parity */
		[2] = 17, /* 16-bit w/o compression; parity */
		[3] = 21, /* 20-bit w/o compression; parity */
		[4] = 25, /* 24-bit w/o compression; parity */
		[5] = 29, /* 28-bit w/o compression; parity */
		[6] = 33, /* 32-bit w/o compression; parity */
		[7] =  0, /* reserved */
	},
	[1] = {
		[0] =  0, /* reserved */
		[1] = 13, /* 16-bit w/ floating-point compression; parity */
		[2] = 17, /* 20-bit w/ floating-point compression; parity */
		[3] = 21, /* 24-bit w/ floating-point compression; parity */
		[4] = 30, /* 24-bit w/o compression; ECC protection */
		[5] =  0, /* reserved */
		[6] = 39, /* 32-bit w/o compression; ECC protection */
		[7] =  0, /* reserved */
	},
};

static void __a2b_bus_calc_respcycs(struct a2b_bus *bus)
{
	struct a2b_node *main = __a2b_bus_main_node(bus);
	struct a2b_node *node;
	unsigned int dnslot_activity[A2B_MAX_NODES] = { };
	unsigned int upslot_activity[A2B_MAX_NODES] = { };
	unsigned int respcycs_dn[A2B_MAX_NODES] = { };
	unsigned int respcycs_up[A2B_MAX_NODES] = { };
	enum a2b_slot_format slot_format_dn = bus->slot_config.format[A2B_DIR_DOWN];
	enum a2b_slot_format slot_format_up = bus->slot_config.format[A2B_DIR_UP];
	enum a2b_slot_size slot_size_dn = bus->slot_config.size[A2B_DIR_DOWN];
	enum a2b_slot_size slot_size_up = bus->slot_config.size[A2B_DIR_UP];
	unsigned int dnslot_size = a2b_slot_bits[slot_format_dn][slot_size_dn];
	unsigned int upslot_size = a2b_slot_bits[slot_format_up][slot_size_up];
	unsigned int respoffs =
		a2b_respoffs[main->tdm_mode][main->tdm_slot_size];
	unsigned int min_respcycs_up = 0xFF;
	unsigned int max_respcycs_dn = 0;
	int i;

	/*
	 * More information about the RESPCYCS formula can be found in the
	 * Technical Reference [1] Appendix B "Response Cycle Formula".
	 */

	__a2b_bus_for_each_node(bus, node, i) {
		unsigned int num_dnslots = node->num_dnslots;
		unsigned int num_upslots = node->num_upslots;
		int addr = node->addr;

		dnslot_activity[addr] = num_dnslots * dnslot_size;
		upslot_activity[addr] = num_upslots * upslot_size;
		respcycs_dn[addr] =
			((64 + dnslot_activity[addr]) / 4) + (4 * addr) + 2;
		respcycs_up[addr] =
			respoffs - (((64 + upslot_activity[addr]) / 4) + 1);

		if (respcycs_dn[addr] > max_respcycs_dn)
			max_respcycs_dn = respcycs_dn[addr];

		if (respcycs_up[addr] < min_respcycs_up)
			min_respcycs_up = respcycs_up[addr];
	}

	bus->main_respcycs = (max_respcycs_dn + min_respcycs_up) / 2;
}

static unsigned int __a2b_bus_respcycs(struct a2b_bus *bus, int addr)
{
	unsigned int main_respcycs = bus->main_respcycs;

	if (addr == A2B_MAIN_ADDR)
		return main_respcycs;

	/*
	 * This formula is taken from [1] section 9-4 "Configuring Slave Node
	 * Response Cycles". Note that the driver indexes subordinate node
	 * addresses starting from 1.
	 */
	return main_respcycs - (4 * (addr - 1));
}

static int __a2b_bus_new_structure(struct a2b_bus *bus)
{
	struct a2b_node *main = __a2b_bus_main_node(bus);
	struct a2b_node *node;
	int ret;
	int i;

	__a2b_bus_calc_respcycs(bus);

	__a2b_bus_for_each_node(bus, node, i) {
		unsigned int respcycs = __a2b_bus_respcycs(bus, node->addr);

		ret = node->ops->set_respcycs(node, respcycs);
		if (ret)
			return ret;
	}

	ret = main->ops->new_structure(main, &bus->slot_config);
	if (ret)
		return ret;

	return 0;
}

static int a2b_bus_new_structure(struct a2b_bus *bus)
{
	int ret;

	mutex_lock(&bus->mutex);
	ret = __a2b_bus_new_structure(bus);
	mutex_unlock(&bus->mutex);

	return ret;
}

unsigned long a2b_bus_status(struct a2b_bus *bus)
{
	unsigned long status;

	mutex_lock(&bus->mutex);
	status = bus->status;
	mutex_unlock(&bus->mutex);

	return status;
}
EXPORT_SYMBOL_GPL(a2b_bus_status);

static unsigned int __a2b_bus_num_subs(struct a2b_bus *bus)
{
	struct a2b_node *node;
	unsigned int num = 0;
	int i;

	__a2b_bus_for_each_sub_node(bus, node, i)
		num++;

	return num;
}

unsigned int a2b_bus_num_subs(struct a2b_bus *bus)
{
	unsigned int n;

	mutex_lock(&bus->mutex);
	n = __a2b_bus_num_subs(bus);
	mutex_unlock(&bus->mutex);

	return n;
}
EXPORT_SYMBOL_GPL(a2b_bus_num_subs);

static unsigned int __a2b_bus_num_nodes(struct a2b_bus *bus)
{
	return __a2b_bus_num_subs(bus) + 1;
}

unsigned int a2b_bus_num_nodes(struct a2b_bus *bus)
{
	unsigned int n;

	mutex_lock(&bus->mutex);
	n = __a2b_bus_num_nodes(bus);
	mutex_unlock(&bus->mutex);

	return n;
}
EXPORT_SYMBOL_GPL(a2b_bus_num_nodes);

static int a2b_bus_del_node(struct device *dev, void *data)
{
	unsigned int *stop_addr = data;
	struct a2b_node *node;

	if (dev->type != &a2b_node_type)
		return 0;

	node = to_a2b_node(dev);

	/* Break out early if this is the node to stop at */
	if (stop_addr && node->addr <= *stop_addr)
		return 1;

	device_unregister(dev);

	return 0;
}

static void a2b_bus_del_nodes_until(struct a2b_bus *bus, unsigned int stop_addr)
{
	device_for_each_child_reverse(&bus->dev, &stop_addr, a2b_bus_del_node);
}

static void a2b_bus_del_nodes(struct a2b_bus *bus)
{
	device_for_each_child_reverse(&bus->dev, NULL, a2b_bus_del_node);
}

static int a2b_bus_of_add_node(struct a2b_bus *bus, struct device_node *np,
			       unsigned int addr)
{
	struct a2b_node *node;
	int ret = 0;

	if (!bus || !np)
		return -EINVAL;

	if (addr >= A2B_MAX_NODES)
		return -EINVAL;

	if (!of_device_is_available(np))
		return -ENODEV;

	if (of_node_test_and_set_flag(np, OF_POPULATED))
		return -EBUSY;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (IS_ERR(node))
		return -ENOMEM;

	node->dev.bus = &a2b_bus;
	node->dev.type = &a2b_node_type;
	node->dev.parent = &bus->dev;
	node->dev.of_node = np;
	node->dev.fwnode = of_fwnode_handle(np);
	dev_set_name(&node->dev, "a2b-%d.%d", bus->id, addr);

	node->bus = bus;
	node->addr = addr;

	/*
	 * Register the node device. Note that due to asynchronous probing,
	 * there is no guarantee that the node driver's probe function has been
	 * called just yet. The synchronization point is a2b_register_node(),
	 * which should be called unconditionally by node drivers.
	 */
	ret = device_register(&node->dev);
	if (ret)
		goto err_put_device;

	return 0;

err_put_device:
	put_device(&node->dev);

	return ret;
}

static struct device_node *a2b_bus_of_get_node_of_node(struct a2b_bus *bus,
						       unsigned int addr)
{
	struct device_node *np = NULL;
	bool found = false;
	u32 val;

	for_each_available_child_of_node(bus->dev.of_node, np) {
		if (of_property_read_u32(np, "reg", &val))
			continue;

		if (val == addr) {
			found = true;
			break;
		}
	}

	return found ? np : NULL;
}

static void a2b_bus_event_discovery_done(struct a2b_bus *bus)
{
	bool done;

	mutex_lock(&bus->mutex);
	done = test_and_clear_bit(A2B_BUS_STATUS_DISCOVERY, &bus->status);
	mutex_unlock(&bus->mutex);

	if (!done)
		return;

	dev_info(&bus->dev, "discovered %d subordinate nodes\n",
		 __a2b_bus_num_subs(bus));
	blocking_notifier_call_chain(&bus->notifier,
				     A2B_BUS_EVENT_DISCOVERY_DONE, NULL);
}

static void a2b_bus_discovery_work(struct work_struct *work)
{
	struct delayed_work *discovery_work = to_delayed_work(work);
	struct device_node *np = NULL;
	struct a2b_bus *bus =
		container_of(discovery_work, struct a2b_bus, discovery_work);
	struct a2b_node *main;
	struct a2b_node *last;
	struct a2b_node *node;
	unsigned int new_addr;
	int ret;
	int i;

	mutex_lock(&bus->mutex);

	main = __a2b_bus_main_node(bus);
	last = __a2b_bus_last_node(bus);
	new_addr = last->addr + 1;

	if (new_addr > main->chip_info->max_subs)
		goto out;

	set_bit(A2B_BUS_STATUS_DISCOVERY, &bus->status);
	set_bit(A2B_BUS_STATUS_DISCOVERING, &bus->status);

	/*
	 * Enable switching on the last currently discovered node. All preceding
	 * nodes continue switching and have their External Switch Mode set to 2
	 * as prescribed in [1] Figure 8-3 "Advanced Discovery Flow".
	 */
	__a2b_bus_for_each_node(bus, node, i) {
		ret = last->ops->set_switching(
			node, true, node == last ? A2B_SWMODE_0 : A2B_SWMODE_2);
		if (ret) {
			dev_err(&last->dev, "failed to disable switching: %d\n", ret);
			goto out;
		}
	}

	/*
	 * Apply a new structure, which generally ensures that the RESPCYCS are
	 * sane before the discovery process begins. Failure to do so may result
	 * in bus errors.
	 */
	__a2b_bus_new_structure(bus);

	/* Begin discovery with the expected RESPCYCS value for the new node */
	ret = main->ops->discover(main, __a2b_bus_respcycs(bus, new_addr));
	if (ret < 0) {
		dev_err(&bus->dev, "discovery error: %d\n", ret);
		goto out;
	} else if (ret) {
		/*
		 * Discovery timed out, presumably meaning that there are no
		 * nodes left to discover. Disable switching on the last node to
		 * prevent spurious bus errors. All other nodes ought to revert
		 * to a normal External Switching Mode, cf. [1] Figure 8-32.
		 */
		__a2b_bus_for_each_node(bus, node, i)
		{
			ret = last->ops->set_switching(node, node != last,
						       A2B_SWMODE_0);
			if (ret) {
				dev_err(&last->dev,
					"failed to disable switching: %d\n",
					ret);
				goto out;
			}
		}

		goto out;
	}

	np = a2b_bus_of_get_node_of_node(bus, new_addr);
	if (!np) {
		dev_warn(&bus->dev, "missing OF child node for %d\n", i);
		goto out;
	}

	ret = a2b_bus_of_add_node(bus, np, new_addr);
	of_node_put(np);
	if (ret)
		dev_err(&bus->dev, "failed to add new node %d: %d\n", i, ret);

out:
	clear_bit(A2B_BUS_STATUS_DISCOVERING, &bus->status);
	mutex_unlock(&bus->mutex);

	/*
	 * If there is no new node after this discovery, then the discovery
	 * process is finished. Signal the event.
	 */
	if (!np || ret)
		a2b_bus_event_discovery_done(bus);

	return;
}

static void a2b_bus_discover(struct a2b_bus *bus)
{
	schedule_delayed_work(&bus->discovery_work, msecs_to_jiffies(100));
}

int a2b_register_bus(struct a2b_bus *bus)
{
	struct device_node *np;
	int ret;

	if (!bus->parent || !bus->ops)
		return -EINVAL;

	/* Initialize private bus data */
	mutex_init(&bus->mutex);
	INIT_DELAYED_WORK(&bus->discovery_work, a2b_bus_discovery_work);
	BLOCKING_INIT_NOTIFIER_HEAD(&bus->notifier);
	set_bit(A2B_BUS_STATUS_DISCOVERY, &bus->status);
	bus->id = ida_alloc(&a2b_ida, GFP_KERNEL);
	if (bus->id < 0)
		return -ENOMEM;

	/* Initialize bus device data and register it */
	bus->dev.class = &a2b_bus_class;
	bus->dev.parent = bus->parent;
	device_set_of_node_from_dev(&bus->dev, bus->parent);
	bus->dev.type = &a2b_bus_type;
	dev_set_name(&bus->dev, "a2b-%d", bus->id);

	ret = device_register(&bus->dev);
	if (ret) {
		put_device(&bus->dev);
		return ret;
	}

	/* It is mandatory to specify an OF node for the main node */
	np = a2b_bus_of_get_node_of_node(bus, A2B_MAIN_ADDR);
	if (!np) {
		ret = -EINVAL;
		goto err_device_unregister;
	}

	ret = a2b_bus_of_add_node(bus, np, A2B_MAIN_ADDR);
	of_node_put(np);
	if (ret)
		goto err_device_unregister;

	return 0;

err_device_unregister:
	device_unregister(&bus->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(a2b_register_bus);

void a2b_unregister_bus(struct a2b_bus *bus)
{
	cancel_delayed_work_sync(&bus->discovery_work);

	a2b_bus_del_nodes(bus);

	device_unregister(&bus->dev);
}
EXPORT_SYMBOL_GPL(a2b_unregister_bus);

struct a2b_bus *a2b_find_bus_by_of_node(struct device_node *np)
{
	struct device *dev = class_find_device_by_of_node(&a2b_bus_class, np);

	return dev ? to_a2b_bus(dev) : NULL;
}
EXPORT_SYMBOL_GPL(a2b_find_bus_by_of_node);

void a2b_put_bus(struct a2b_bus *bus)
{
	put_device(&bus->dev);
}
EXPORT_SYMBOL_GPL(a2b_put_bus);

int a2b_bus_register_notifier(struct a2b_bus *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&bus->notifier, nb);
}
EXPORT_SYMBOL_GPL(a2b_bus_register_notifier);

int a2b_bus_unregister_notifier(struct a2b_bus *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&bus->notifier, nb);
}
EXPORT_SYMBOL_GPL(a2b_bus_unregister_notifier);

/**
 * A2B NODE
 **/

int a2b_node_read(struct a2b_node *node, unsigned int reg, unsigned int *val)
{
	struct a2b_bus *bus = node->bus;

	return bus->ops->read(bus, node, reg, val);
}
EXPORT_SYMBOL_GPL(a2b_node_read);

int a2b_node_write(struct a2b_node *node, unsigned int reg, unsigned int val)
{
	struct a2b_bus *bus = node->bus;

	return bus->ops->write(bus, node, reg, val);
}
EXPORT_SYMBOL_GPL(a2b_node_write);

int a2b_node_i2c_xfer(struct a2b_node *node, struct i2c_msg *msgs, int num)
{
	struct a2b_bus *bus = node->bus;

	return bus->ops->i2c_xfer(bus, node, msgs, num);
}
EXPORT_SYMBOL_GPL(a2b_node_i2c_xfer);

int a2b_node_get_inttype(struct a2b_node *node, unsigned int *val)
{
	struct a2b_bus *bus = node->bus;

	/*
	 * Obviously, this function should only be used if the node in question
	 * received an IRQ
	 */

	return bus->ops->get_inttype(bus, val);
}
EXPORT_SYMBOL_GPL(a2b_node_get_inttype);

static void a2b_node_bus_drop_work(struct work_struct *work)
{
	struct a2b_node *node =
		container_of(work, struct a2b_node, bus_drop_work);
	struct a2b_bus *bus = node->bus;
	int ret;

	ret = node->ops->set_switching(node, false, A2B_SWMODE_0);
	if (ret)
		dev_err_ratelimited(&node->dev,
				    "failed to disable switching: %d\n", ret);

	/* Delete the nodes that have left the bus */
	a2b_bus_del_nodes_until(bus, node->addr);

	/* Schedule a rediscovery attempt in case the error is transient */
	schedule_delayed_work(&bus->discovery_work, msecs_to_jiffies(1000));
}

void a2b_node_report_error(struct a2b_node *node, enum a2b_error error)
{
	struct a2b_bus *bus = node->bus;

	/*
	 * According to [1] section 3-14 "Slave Node Response Cycles", the
	 * following errors can be observed during discovery: CRCERR, SRFERR,
	 * SRFCRCERR. Additionally a PWRERR_3 has been observed in practice when
	 * enabling switching on a node whose B-Side is not connected. The
	 * DISCOVERING status bit covers these cases - don't bother warning
	 * about them.
	 */
	if (test_bit(A2B_BUS_STATUS_DISCOVERING, &bus->status)) {
		switch (error) {
		case A2B_CRCERR:
		case A2B_SRFERR:
		case A2B_SRFCRCERR:
		case A2B_PWRERR_3:
			dev_dbg_ratelimited(
				&node->dev,
				"A2B bus error %d during discovery: %s\n",
				error, a2b_error_to_string(error));
			return;
		default:
			break;
		}
	}

	/*
	 * An SRF miss error normally indicates that the next downstream node
	 * has dropped off the bus. When a node detects this error in 32
	 * consecutive superframes, it assumes a bus drop, signals an SRF miss
	 * error, and asserts itself as the last node on the bus, cf. [1]
	 * section 5-5 "Line Diagnostics After Discovery".
	 */
	if (error == A2B_SRFERR) {
		int last = node->ops->is_last(node);

		if (last < 0) {
			dev_err_ratelimited(
				&node->dev,
				"failed to determine lastness of node: %d\n",
				last);
			return;
		}

		if (last)
			schedule_work(&node->bus_drop_work);

		return;
	}

	dev_warn_ratelimited(&node->dev, "A2B bus error %d: %s\n", error,
			     a2b_error_to_string(error));
}
EXPORT_SYMBOL_GPL(a2b_node_report_error);

int a2b_node_request_slots_pre(struct a2b_node *node,
			       enum a2b_direction direction)
{
	struct a2b_bus *bus = node->bus;
	int ret = 0;

	mutex_lock(&bus->mutex);

	if (bus->slotreqs[direction] & BIT(node->addr)) {
		ret = -EBUSY;
		goto out;
	}

	bus->slotreqs[direction] |= BIT(node->addr);

out:
	mutex_unlock(&bus->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(a2b_node_request_slots_pre);

int a2b_node_request_slots(struct a2b_node *node, enum a2b_direction direction,
			   unsigned int slots, enum a2b_slot_size slot_size,
			   enum a2b_slot_format slot_format)
{
	struct a2b_bus *bus = node->bus;
	int ret = 0;

	mutex_lock(&bus->mutex);

	if (!(bus->slotreqs[direction] & BIT(node->addr))) {
		ret = -EINVAL;
		goto out;
	}

	if (direction == A2B_DIR_UP)
		node->num_upslots = slots;
	else
		node->num_dnslots = slots;

	/* The main node decides the slot sizes and formats */
	if (is_a2b_main(node)) {
		bus->slot_config.size[direction] = slot_size;
		bus->slot_config.format[direction] = slot_format;
	}

	bus->slotreqs[direction] &= ~BIT(node->addr);

	/* Last requestor - apply new structure */
	if (!bus->slotreqs[direction])
		__a2b_bus_new_structure(bus);

out:
	mutex_unlock(&bus->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(a2b_node_request_slots);

int a2b_register_node(struct a2b_node *node)
{
	struct a2b_bus *bus = node->bus;
	int ret;

	/* Obligatory */
	if (!node->chip_info || !node->ops || !node->ops->setup ||
	    !node->ops->set_respcycs || !node->ops->set_switching ||
	    !node->ops->is_last)
		return -EINVAL;

	/* Main obligatory */
	if (is_a2b_main(node) &&
	    (!node->ops->discover || !node->ops->new_structure))
		return -EINVAL;

	if (node->setup)
		return 0;

	ret = node->ops->setup(node);
	if (ret == -EPROBE_DEFER)
		return ret;
	else if (ret) {
		dev_err(&node->dev, "failed to setup node: %d\n", ret);
		goto err_discovery_done;
	}

	node->setup = true;

	INIT_WORK(&node->bus_drop_work, a2b_node_bus_drop_work);

	/* The node is now ready and can be used by other parts of the core */
	mutex_lock(&bus->mutex);
	bus->nodes[node->addr] = node;
	mutex_unlock(&bus->mutex);

	dev_info(&node->dev,
		 "registered %s node vendor 0x%02x prod 0x%02x ver 0x%02x\n",
		 is_a2b_main(node) ? "main" : "subordinate", node->vendor,
		 node->product, node->version);

	/*
	 * Before kicking off the discovery process, ensure that the default
	 * RESPCYCS value is programmed into the main node. This isn't needed
	 * for subordinate nodes because their default RESPCYCS value is
	 * automatically programmed when they are discovered.
	 */
	if (is_a2b_main(node)) {
		ret = a2b_bus_new_structure(bus);
		if (ret)
			dev_err(&bus->dev,
				"failed to apply new structure: %d\n", ret);
	}

	a2b_bus_discover(node->bus);

	return 0;

err_discovery_done:
	a2b_bus_event_discovery_done(bus);

	return ret;
}
EXPORT_SYMBOL_GPL(a2b_register_node);

void a2b_unregister_node(struct a2b_node *node)
{
	struct a2b_bus *bus = node->bus;

	if (!node->setup)
		return;

	/*
	 * Only hold the mutex to remove the node from the bus node list. It is
	 * safe to teardown the node once it is removed.
	 */
	mutex_lock(&bus->mutex);
	bus->nodes[node->addr] = NULL;
	mutex_unlock(&bus->mutex);

	cancel_work_sync(&node->bus_drop_work);

	if (node->ops->teardown)
		node->ops->teardown(node);

	node->priv = NULL;
	node->setup = false;

	dev_info(&node->dev, "unregistered node\n");
}
EXPORT_SYMBOL_GPL(a2b_unregister_node);

/**
 * A2B FUNC
 **/

struct a2b_func *a2b_node_of_add_func(struct a2b_node *node,
				      struct device_node *np)
{
	struct a2b_func *func;
	int ret = 0;

	if (!node || !np)
		return ERR_PTR(-EINVAL);

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	if (of_node_test_and_set_flag(np, OF_POPULATED))
		return ERR_PTR(-EBUSY);

	func = kzalloc(sizeof(*func), GFP_KERNEL);
	if (IS_ERR(func))
		return ERR_PTR(-ENOMEM);

	func->dev.bus = &a2b_bus;
	func->dev.type = &a2b_func_type;
	func->dev.parent = &node->dev;
	func->dev.of_node = np;
	func->dev.fwnode = of_fwnode_handle(np);
	dev_set_name(&func->dev, "%s-%s", dev_name(&node->dev), np->name);

	func->node = node;

	ret = device_register(&func->dev);
	if (ret)
		goto err_put_device;

	return func;

err_put_device:
	put_device(&func->dev);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(a2b_node_of_add_func);

/**
 * A2B BUS CLASS
 **/

static void a2b_bus_class_dev_release(struct device *dev)
{
	struct a2b_bus *bus = to_a2b_bus(dev);

	ida_free(&a2b_ida, bus->id);
}

const struct class a2b_bus_class = {
	.name = "a2b",
	.dev_release = a2b_bus_class_dev_release,
};

const struct device_type a2b_bus_type = {
	.name = "a2b-bus",
};

/**
 * BUS DRIVER
 **/

static void a2b_node_release(struct device *dev)
{
	struct a2b_node *a2b_node = to_a2b_node(dev);

	of_node_clear_flag(dev->of_node, OF_POPULATED);
	kfree(a2b_node);
}

const struct device_type a2b_node_type = {
	.name = "a2b-node",
	.release = a2b_node_release,
};

static void a2b_func_release(struct device *dev)
{
	struct a2b_func *a2b_func = to_a2b_func(dev);

	of_node_clear_flag(dev->of_node, OF_POPULATED);
	kfree(a2b_func);
}

const struct device_type a2b_func_type = {
	.name = "a2b-func",
	.release = a2b_func_release,
};

int __a2b_driver_register(struct a2b_driver *a2b_drv, struct module *owner)
{
	if (WARN_ON(!is_registered))
		return -EAGAIN;

	a2b_drv->driver.bus = &a2b_bus;
	a2b_drv->driver.owner = owner;

	return driver_register(&a2b_drv->driver);
}
EXPORT_SYMBOL_GPL(__a2b_driver_register);

void a2b_driver_unregister(struct a2b_driver *a2b_drv)
{
	if (a2b_drv)
		driver_unregister(&a2b_drv->driver);
}
EXPORT_SYMBOL_GPL(a2b_driver_unregister);

static int a2b_bus_match(struct device *dev,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,11,0)
			 const struct device_driver *drv)
#else
			 struct device_driver *drv)
#endif
{
	if (of_driver_match_device(dev, drv))
		return 1;

	return 0;
}

static int a2b_bus_probe(struct device *dev)
{
	struct a2b_driver *a2b_drv = to_a2b_driver(dev->driver);

	return a2b_drv->probe(dev);
}

static void a2b_bus_remove(struct device *dev)
{
	struct a2b_driver *a2b_drv = to_a2b_driver(dev->driver);

	if (dev->type == &a2b_node_type) {
		struct a2b_node *node = to_a2b_node(dev);

		/*
		 * Remove all nodes downstream from this one, because proper bus
		 * functionality cannot be guaranteed if an upstream node is not
		 * registered with the core.
		 */
		a2b_bus_del_nodes_until(node->bus, node->addr);
	}

	if (a2b_drv->remove)
		a2b_drv->remove(dev);
}

static void a2b_bus_shutdown(struct device *dev)
{
	struct a2b_driver *a2b_drv = to_a2b_driver(dev->driver);

	if (!dev || !a2b_drv)
		return;

	if (a2b_drv->shutdown)
		a2b_drv->shutdown(dev);
}

static int a2b_bus_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	int ret;

	ret = of_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	return 0;
}

const struct bus_type a2b_bus = {
	.name = "a2b",
	.match = a2b_bus_match,
	.probe = a2b_bus_probe,
	.remove = a2b_bus_remove,
	.shutdown = a2b_bus_shutdown,
	.uevent = a2b_bus_uevent,
};
EXPORT_SYMBOL_GPL(a2b_bus);

static int __init a2b_bus_init(void)
{
	int ret;

	ret = bus_register(&a2b_bus);
	if (ret)
		return ret;

	ret = class_register(&a2b_bus_class);
	if (ret)
		goto err_unregister_bus;

	is_registered = true;

	return 0;

err_unregister_bus:
	bus_unregister(&a2b_bus);

	return ret;
}

static void __exit a2b_bus_exit(void)
{
	class_unregister(&a2b_bus_class);
	bus_unregister(&a2b_bus);
}

subsys_initcall(a2b_bus_init);
module_exit(a2b_bus_exit);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("A2B driver core");
MODULE_LICENSE("GPL");
