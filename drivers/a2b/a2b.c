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
 * A2B NODE
 **/

void a2b_node_report_error(struct a2b_node *node, enum a2b_error error)
{
	dev_err_ratelimited(&node->dev, "A2B bus error %d: %s\n", error,
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

static int a2b_bus_new_structure(struct a2b_bus *bus);

int a2b_node_request_slots(struct a2b_node *node, enum a2b_direction direction,
			   unsigned int slots)
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

	bus->slotreqs[direction] &= ~BIT(node->addr);

	/* Last requestor - apply new structure */
	if (!bus->slotreqs[direction])
		a2b_bus_new_structure(bus);

out:
	mutex_unlock(&bus->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(a2b_node_request_slots);

struct a2b_node *a2b_bus_of_add_node(struct a2b_bus *bus,
				     struct device_node *np, unsigned int addr)
{
	struct a2b_node *node;
	int ret = 0;

	if (!bus || !np)
		return ERR_PTR(-EINVAL);

	if (addr >= A2B_MAX_NODES)
		return ERR_PTR(-EINVAL);

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	/* if (of_node_test_and_set_flag(np, OF_POPULATED)) */
	/* 	return ERR_PTR(-EBUSY); */

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (IS_ERR(node))
		return ERR_PTR(-ENOMEM);

	mutex_lock(&bus->mutex);

	/* Add node to the bus node list, or generate an error if occupied */
	if (bus->nodes[addr])
		ret = -EBUSY;
	else
		bus->nodes[addr] = node;
	if (ret) {
		kfree(node);
		mutex_unlock(&bus->mutex);
		goto err_clear_flag;
	}

	node->dev.bus = &a2b_bus_type;
	node->dev.type = &a2b_node_type;
	node->dev.parent = bus->dev;
	node->dev.of_node = of_node_get(np);
	if (addr == A2B_MAIN_ADDR)
		dev_set_name(&node->dev, "a2b-%d", bus->id);
	else
		dev_set_name(&node->dev, "a2b-%d.%d", bus->id, addr);

	node->bus = bus;
	node->addr = addr;

	ret = device_register(&node->dev);
	if (ret) {
		goto err_put_device;
	}

	mutex_unlock(&bus->mutex);

	return node;

err_put_device:
	/* Remove node from the bus node list before putting */
	bus->nodes[addr] = NULL;
	mutex_unlock(&bus->mutex);

	put_device(&node->dev);
err_clear_flag:
	of_node_clear_flag(np, OF_POPULATED);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(a2b_bus_of_add_node);

static unsigned int a2b_bus_num_subs(struct a2b_bus *bus)
{
	int i;

	for (i = 0; i < A2B_MAX_NODES; i++)
		if (!bus->nodes[i])
			return i;

	return 15;
}

static struct a2b_node *a2b_bus_last_node(struct a2b_bus *bus)
{
	struct a2b_node *node = bus->nodes[A2B_MAIN_ADDR];
	int i;

	for (i = 0; i < A2B_MAX_NODES - 1; i++) {
		if (bus->nodes[i])
			node = bus->nodes[i];
		else
			break;
	}

	return node;
}

static void a2b_bus_discover(struct a2b_bus *bus)
{
	unsigned int num_subs = a2b_bus_num_subs(bus);
	unsigned int expected_subs = 0;
	struct device_node *child;

	/*
	 * Run the discovery for exactly the number of child nodes described in
	 * the device tree. If further nodes are connected, they will not be
	 * able to be probed unless there is a corresponding OF node. And
	 * conversely, running the discovery process when all nodes have already
	 * been discovered results in a PWRERR (cable disconnected/open
	 * circuit/wrong port) which would result in a spurious error message
	 * every time the A2B bus is enumerated.
	 */
	for_each_child_of_node(bus->dev->of_node, child) {
		if (of_node_name_eq(child, "node"))
			expected_subs++;
	}

	if (num_subs < expected_subs)
		schedule_delayed_work(&bus->discovery_work,
				      msecs_to_jiffies(100));
}

int a2b_register_node(struct a2b_node *node)
{
	int ret;

	if (node->setup)
		return 0;

	/* Obligatory */
	if (!node->ops || !node->ops->setup || !node->ops->set_respcycs ||
	    !node->ops->set_switching)
		return -EINVAL;

	/* Main obligatory */
	if (node->addr == A2B_MAIN_ADDR && !node->ops->discover)
		return -EINVAL;

	ret = node->ops->setup(node);
	if (ret)
		return ret;

	node->setup = true;

	a2b_bus_discover(node->bus);

	return 0;
}
EXPORT_SYMBOL_GPL(a2b_register_node);

void a2b_unregister_node(struct a2b_node *node)
{
	if (!node->setup)
		return;

	if (node->ops->teardown)
		node->ops->teardown(node);

	node->setup = false;
}
EXPORT_SYMBOL_GPL(a2b_unregister_node);

/**
 * A2B FUNC
 **/

int a2b_func_read(struct a2b_func *func, unsigned int reg, unsigned int *val,
		  int flags)
{
	struct a2b_node *node = func->node;
	struct a2b_bus *bus = node->bus;

	return bus->ops->read(bus, node, reg, val, flags);
}
EXPORT_SYMBOL_GPL(a2b_func_read);

int a2b_func_write(struct a2b_func *func, unsigned int reg, unsigned int val,
		   int flags)
{
	struct a2b_node *node = func->node;
	struct a2b_bus *bus = node->bus;

	return bus->ops->write(bus, node, reg, val, flags);
}
EXPORT_SYMBOL_GPL(a2b_func_write);

struct a2b_func *a2b_node_of_add_func(struct a2b_node *node,
				      struct device_node *np)
{
	struct a2b_func *func;
	int ret = 0;

	if (!node || !np)
		return ERR_PTR(-EINVAL);

	if (!of_device_is_available(np))
		return ERR_PTR(-ENODEV);

	/* if (of_node_test_and_set_flag(np, OF_POPULATED)) */
	/* 	return ERR_PTR(-EBUSY); */

	func = kzalloc(sizeof(*func), GFP_KERNEL);
	if (IS_ERR(func))
		return ERR_PTR(-ENOMEM);

	func->dev.bus = &a2b_bus_type;
	func->dev.type = &a2b_func_type;
	func->dev.parent = &node->dev;
	func->dev.of_node = of_node_get(np);
	dev_set_name(&func->dev, "%s-%s", dev_name(&node->dev), np->name);

	func->node = node;

	ret = device_register(&func->dev);
	if (ret)
		goto err_put_device;

	return func;

err_put_device:
	put_device(&func->dev);
/* err_clear_flag: */ // TODO FIXME
	of_node_clear_flag(np, OF_POPULATED);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(a2b_node_of_add_func);

/**
 * A2B BUS
 **/

/* From [1] Table 9-1: A2B Master Node Response Offset (RESPOFFS) */
static const unsigned int a2b_respoffs[A2B_TDMMODE_END][A2B_TDMSS_END] = {
	[A2B_TDMMODE_2] = { 245, 238 },
	[A2B_TDMMODE_4] = { 248, 245 },
	[A2B_TDMMODE_8] = { 248, 248 },
	[A2B_TDMMODE_12] = { 248, 248 },
	[A2B_TDMMODE_16] = { 248, 248 },
	[A2B_TDMMODE_20] = { 248, 248 }, // TODO: check N/A remarks in table
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

static void a2b_bus_calc_respcycs(struct a2b_bus *bus)
{
	struct a2b_node *main = bus->nodes[A2B_MAIN_ADDR];
	unsigned int dnslot_activity[A2B_MAX_NODES];
	unsigned int upslot_activity[A2B_MAX_NODES];
	unsigned int respcycs_dn[A2B_MAX_NODES];
	unsigned int respcycs_up[A2B_MAX_NODES];
	unsigned int dnslot_size = a2b_slot_bits[main->dnfmt][main->dnss];
	unsigned int upslot_size = a2b_slot_bits[main->upfmt][main->upss];
	unsigned int respoffs =
		a2b_respoffs[main->tdm_mode][main->tdm_slot_size];
	unsigned int min_respcycs_up = 0xFF;
	unsigned int max_respcycs_dn = 0;
	unsigned int main_respcycs;
	int i;

	for (i = 0; i < A2B_MAX_NODES; i++) {
		struct a2b_node *node = bus->nodes[i];
		unsigned int num_dnslots = 0;
		unsigned int num_upslots = 0;

		if (node) {
			num_dnslots = node->num_dnslots;
			num_upslots = node->num_upslots;
		}

		dnslot_activity[i] = num_dnslots * (dnslot_size + 1);
		upslot_activity[i] = num_upslots * (upslot_size + 1);
		respcycs_dn[i] = ((64 + dnslot_activity[i]) / 4) + (4 * i) + 2;
		respcycs_up[i] =
			respoffs - (((64 + upslot_activity[i]) / 4) + 1);

		if (respcycs_dn[i] > max_respcycs_dn)
			max_respcycs_dn = respcycs_dn[i];

		if (respcycs_up[i] < min_respcycs_up)
			min_respcycs_up = respcycs_up[i];
	}

	main_respcycs = (max_respcycs_dn + min_respcycs_up) / 2;

	for (i = 0; i < A2B_MAX_NODES; i++) {
		if (i == A2B_MAIN_ADDR)
			bus->respcycs[i] = bus->respcycs[0];
		else
			bus->respcycs[i] = main_respcycs - (4 * i);
	}
}

static int a2b_bus_new_structure(struct a2b_bus *bus)
{
	int ret;
	int i;

	a2b_bus_calc_respcycs(bus);

	for (i = 0; i < A2B_MAX_NODES; i++) {
		if (!bus->nodes[i])
			continue;

		if (bus->nodes[i]->setup)
			ret = bus->nodes[i]->ops->set_respcycs(
				bus->nodes[i], bus->respcycs[i]);
		if (ret)
			goto out;
	}

	ret = bus->nodes[A2B_MAIN_ADDR]->ops->new_structure(
		bus->nodes[A2B_MAIN_ADDR]);

out:
	return ret;
}

static void a2b_bus_discovery_work(struct work_struct *work)
{
	struct delayed_work *discovery_work = to_delayed_work(work);
	struct a2b_bus *bus =
		container_of(discovery_work, struct a2b_bus, discovery_work);
	struct a2b_node *main_node = bus->nodes[A2B_MAIN_ADDR];
	struct a2b_node *node;
	struct a2b_node **new_node = NULL;
	struct device_node *np;
	bool found = false;
	int ret;
	int i;

	mutex_lock(&bus->mutex);
	ret = a2b_bus_new_structure(bus);
	mutex_unlock(&bus->mutex);
	if (ret) {
		dev_err(bus->dev, "failed to apply new structure: %d\n", ret);
		return;
	}

	for (i = 0; i < A2B_MAX_NODES; i++) {
		unsigned int value = 0x01; // ENSW=1

		node = bus->nodes[i];
		if (!node)
			continue;

		ret = node->ops->set_switching(node, value);
		if (ret) {
			dev_err(&node->dev,
				"failed to enable switching: %d\n", ret);
		}
	}

	ret = main_node->ops->discover(main_node,
				       bus->respcycs[a2b_bus_num_subs(bus)]);
	if (ret < 0) {
		dev_err(bus->dev, "discovery error: %d\n", ret);
		return;
	} else if (ret) {
		struct a2b_node *last = a2b_bus_last_node(bus);

		ret = node->ops->set_switching(last, 0);
		if (ret) {
			dev_err(&node->dev, "failed to disable switching: %d\n",
				ret);
		}

		return;
	}

	/* Find where to place the new node */
	for (i = 0; i < A2B_MAIN_ADDR; i++) {
		if (bus->nodes[i])
			continue;
		new_node = &bus->nodes[i];
		break;
	}

	if (!new_node)
		return;

	for_each_available_child_of_node(bus->dev->of_node, np) {
		u32 addr;

		if (of_property_read_u32(np, "reg", &addr))
			continue;

		if (addr == i) {
			found = true;
			break;
		}
	}

	if (!found) {
		dev_warn(bus->dev, "missing OF child node for %d\n", i);
		return;
	}

	*new_node = a2b_bus_of_add_node(bus, np, i);
	of_node_put(np);
	if (IS_ERR(*new_node)) {
		dev_err(bus->dev, "failed to add new node %d: %pe\n", i,
			*new_node);
		*new_node = NULL;
		return;
	}

	return;
}

int a2b_register_bus(struct a2b_bus *bus)
{
	if (!bus->ops)
		return -EINVAL;

	// TODO: subsystem-wide mutex here
	bus->id = ida_alloc(&a2b_ida, GFP_KERNEL);
	if (bus->id < 0)
		return -ENOMEM;

	mutex_init(&bus->mutex);
	INIT_DELAYED_WORK(&bus->discovery_work, a2b_bus_discovery_work);

	return 0;
}
EXPORT_SYMBOL_GPL(a2b_register_bus);

void a2b_unregister_bus(struct a2b_bus *bus)
{
	cancel_delayed_work_sync(&bus->discovery_work);

	a2b_unregister_node(bus->nodes[A2B_MAIN_ADDR]);
	device_unregister(&bus->nodes[A2B_MAIN_ADDR]->dev);
	bus->nodes[A2B_MAIN_ADDR] = NULL;
	ida_free(&a2b_ida, bus->id);
	bus->id = 0;
}
EXPORT_SYMBOL_GPL(a2b_unregister_bus);

/**
 * BUS DRIVER
 **/

static void a2b_node_release(struct device *dev)
{
	struct a2b_node *a2b_node = to_a2b_node(dev);

	of_node_put(dev->of_node);
	kfree(a2b_node);
}

struct device_type a2b_node_type = {
	.name = "a2b-node",
	.release = a2b_node_release,
};

static void a2b_func_release(struct device *dev)
{
	struct a2b_func *a2b_func = to_a2b_func(dev);

	of_node_put(dev->of_node);
	kfree(a2b_func);
}

struct device_type a2b_func_type = {
	.name = "a2b-func",
	.release = a2b_func_release,
};

int __a2b_driver_register(struct a2b_driver *a2b_drv, struct module *owner)
{
	if (WARN_ON(!is_registered))
		return -EAGAIN;

	a2b_drv->driver.bus = &a2b_bus_type;
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
		struct a2b_node *next_node = NULL;
		struct a2b_bus *bus = node->bus;

		/* Recursively remove downstream nodes */
		// TODO: This is really ugly. Revisit.
		if (node->addr == A2B_MAIN_ADDR)
			next_node = bus->nodes[0];
		else if (node->addr < A2B_MAIN_ADDR - 1)
			next_node = bus->nodes[node->addr + 1];

		if (next_node) {
			int addr = next_node->addr;
			a2b_unregister_node(next_node);
			device_unregister(&next_node->dev);
			bus->nodes[addr] = NULL;
		}
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

struct bus_type a2b_bus_type = {
	.name = "a2b",
	.match = a2b_bus_match,
	.probe = a2b_bus_probe,
	.remove = a2b_bus_remove,
	.shutdown = a2b_bus_shutdown,
	.uevent = a2b_bus_uevent,
};
EXPORT_SYMBOL_GPL(a2b_bus_type);

static int __init a2b_bus_init(void)
{
	int ret;

	ret = bus_register(&a2b_bus_type);
	if (ret)
		return ret;

	is_registered = true;

	return 0;
}

static void __exit a2b_bus_exit(void)
{
	bus_unregister(&a2b_bus_type);
}

subsys_initcall(a2b_bus_init);
module_exit(a2b_bus_exit);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("A2B driver core");
MODULE_LICENSE("GPL");
