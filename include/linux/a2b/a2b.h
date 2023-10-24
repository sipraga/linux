/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A2B driver core
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */
#ifndef _A2B_H_
#define _A2B_H_

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/notifier.h>

struct i2c_msg;

/**
 * MISC
 **/

enum a2b_chip_caps {
	A2B_CHIP_CAP_MAIN = (1 << 0),
};

struct a2b_chip_info {
	int caps;
	unsigned int max_subs;
};

enum a2b_chips {
	A2B_AD2403,
	A2B_AD2410,
	A2B_AD2425,
	A2B_AD2428,
	A2B_AD2429,
	A2B_NUM_CHIPS,
};

extern const struct a2b_chip_info a2b_chip_info[A2B_NUM_CHIPS];

enum a2b_superframe_freq {
	A2B_SFF_48000,
	A2B_SFF_44100,
};

enum a2b_tdm_mode {
	A2B_TDMMODE_2,
	A2B_TDMMODE_4,
	A2B_TDMMODE_8,
	A2B_TDMMODE_12,
	A2B_TDMMODE_16,
	A2B_TDMMODE_20,
	A2B_TDMMODE_24,
	A2B_TDMMODE_32,
	A2B_TDMMODE_END,
};

enum a2b_tdm_slot_size {
	A2B_TDMSS_32,
	A2B_TDMSS_16,
	A2B_TDMSS_END,
};

/**
 * enum a2b_swmode - A2B transceiver External Switch Mode
 *
 * For more information about the meaning of these modes, see the Technical
 * Reference [1] Table 7-8 A2B_SWCTL Register Fields.
 */
enum a2b_swmode {
	A2B_SWMODE_0 = 0,
	A2B_SWMODE_1 = 1,
	A2B_SWMODE_2 = 2,
};

enum a2b_direction {
	A2B_DIR_UP,
	A2B_DIR_DOWN,
};

enum a2b_slot_size {
	A2B_SLOT_SIZE_8 = 0,
	A2B_SLOT_SIZE_12 = 1,
	A2B_SLOT_SIZE_16 = 2,
	A2B_SLOT_SIZE_20 = 3,
	A2B_SLOT_SIZE_24 = 4,
	A2B_SLOT_SIZE_28 = 5,
	A2B_SLOT_SIZE_32 = 6,
};

enum a2b_slot_format {
	A2B_SLOT_FORMAT_NORMAL = 0,
	A2B_SLOT_FORMAT_ALT = 1,
};

struct a2b_slot_config {
	enum a2b_slot_size size[2];
	enum a2b_slot_format format[2];
};

/**
 * A2B NODE
 **/

/*
 * Per the specification of the Interrupt Source Register in the reference
 * manual, cf. [1] Figure 7-20, the maximum number of nodes is hard-coded to 17,
 * because the register supports signalling of interrupts from up to 16
 * subordinate nodes through the 4-bit INODE field.
 *
 *  A2B_INTSRC: Interrupt Source Register (Main Only)
 *     _______________________________
 *    | 7 | 6 |   |   | 3   2   1   0 |
 *     -v---v-----------v-------------
 *      |   |           |
 *      |   |           `-> INODE (Interrupt Node ID)
 *      |   |
 *      |   `-------------> SLVINT (Slave/Subordinate Interrupt)
 *      |
 *      `-----------------> MSTINT (Master/Main Interrupt)
 *
 * In practice many A2B main mode transceivers support discovery of far fewer
 * subordinate nodes.
 *
 * Note that unlike in this driver, the A2B hardware itself indexes subordinate
 * nodes starting at zero, i.e. A2B_INTSRC.INODE=0 means that the first
 * (nearest) subordinate node is signalling an interrupt. The reference manual
 * also uses this convention. Here, the main node is zero and the first
 * subordinate node is 1. The difference only needs to be accounted for in a few
 * places such as interrupt handling and indirect register access to subordinate
 * nodes.
 */
#define A2B_MAX_NODES 17
#define A2B_MAIN_ADDR 0

struct a2b_node;

/**
 * struct a2b_node_ops - node driver ops
 *
 * @set_respcycs: invoked by the core to configure the RESPCYCS register
 * @set_switching: invoked by the core to configure the switch control register
 * @discover: (main only) invoked by the core to initiate the discovery process;
 *            the respcycs argument is automatically programmed into the newly
 *            discovered node's RESPCYCS register on success; the node driver
 *            must ensure that DISCVRY.DSCACT=0 before this function retruns;
 *            return 0 on success or non-zero on discovery timeout
 * @new_structure: (main only) invoked by the core to program a new structure
 * @is_last: invoked by the core to query whether the target node thinks it is
 *           the last node on the bus
 * @setup: the A2B core invokes this function when the node is registered by the
 *         node driver; setup of any peripheral functions (cf. &struct a2b_func)
 *         should happen here
 * @teardown: (optional) invoked by the core when the node is unregistered; the
 *            node driver should undo whatever it may have done in setup
 */
struct a2b_node_ops {
	int (*set_respcycs)(struct a2b_node *node, unsigned int respcycs);
	int (*set_switching)(struct a2b_node *node, bool enable, enum a2b_swmode mode);
	int (*discover)(struct a2b_node *node, unsigned int respcycs);
	int (*new_structure)(struct a2b_node *node,
			     const struct a2b_slot_config *slot_config);
	int (*is_last)(struct a2b_node *node);
	int (*setup)(struct a2b_node *node);
	void (*teardown)(struct a2b_node *node);
};

struct a2b_node {
	/* A2B node driver fills this in */
	const struct a2b_node_ops *ops;
	const struct a2b_chip_info *chip_info;
	enum a2b_superframe_freq sff;
	unsigned int vendor;
	unsigned int product;
	unsigned int version;
	unsigned int invert_sync : 1;
	unsigned int early_sync : 1;
	unsigned int alternating_sync : 1;
	unsigned int rx_on_dtx1 : 1;
	enum a2b_tdm_mode tdm_mode;
	enum a2b_tdm_slot_size tdm_slot_size;
	void *priv;
	
	/* A2B core only */
	struct device dev;
	bool setup;
	struct a2b_bus *bus;
	struct work_struct bus_drop_work;
	unsigned int addr;
	unsigned int num_dnslots;
	unsigned int num_upslots;
};

static inline bool is_a2b_main(const struct a2b_node *node)
{
	return node->addr == A2B_MAIN_ADDR;
}

enum a2b_inttype {
	A2B_INTTYPE_HDCNTERR = 0,
	A2B_INTTYPE_DDERR = 1,
	A2B_INTTYPE_CRCERR = 2,
	A2B_INTTYPE_DPERR = 3,
	A2B_INTTYPE_BECOVF = 4,
	A2B_INTTYPE_SRFERR = 5,
	A2B_INTTYPE_SRFCRCERR = 6,
	/* 7~8 reserved */
	A2B_INTTYPE_PWRERR_0 = 9,
	A2B_INTTYPE_PWRERR_1 = 10,
	A2B_INTTYPE_PWRERR_2 = 11,
	A2B_INTTYPE_PWRERR_3 = 12,
	A2B_INTTYPE_PWRERR_4 = 13,
	/* 14 reserved */
	A2B_INTTYPE_PWRERR_5 = 15,
	A2B_INTTYPE_IO0PND = 16,
	A2B_INTTYPE_IO1PND = 17,
	A2B_INTTYPE_IO2PND = 18,
	A2B_INTTYPE_IO3PND = 19,
	A2B_INTTYPE_IO4PND = 20,
	A2B_INTTYPE_IO5PND = 21,
	A2B_INTTYPE_IO6PND = 22,
	A2B_INTTYPE_IO7PND = 23,
	A2B_INTTYPE_DSCDONE = 24,
	A2B_INTTYPE_I2CERR = 25,
	A2B_INTTYPE_ICRCERR = 26,
	/* 27~40 reserved */
	A2B_INTTYPE_PWRERR_6 = 41,
	A2B_INTTYPE_PWRERR_7 = 42,
	/* 42~47 reserved */
	A2B_INTTYPE_MBOX0FULL = 48,
	A2B_INTTYPE_MBOX0EMPTY = 49,
	A2B_INTTYPE_MBOX1FULL = 50,
	A2B_INTTYPE_MBOX1EMPTY = 51,
	/* 52~127 reserved */
	A2B_INTTYPE_IRQMSGERR = 128,
	/* 129~251 reserved */
	A2B_INTTYPE_STARTUPERR = 252,
	A2B_INTTYPE_SLVINTTYPERR = 253,
	A2B_INTTYPE_STBYDONE = 254,
	A2B_INTTYPE_MSTR_RUNNING = 255,
};

enum a2b_error {
	A2B_HDCNTERR = 0,
	A2B_DDERR = 1,
	A2B_CRCERR = 2,
	A2B_DPERR = 3,
	A2B_BECOVF = 4,
	A2B_SRFERR = 5,
	A2B_SRFCRCERR = 6,
	/* 7~8 reserved */
	A2B_PWRERR_0 = 9,
	A2B_PWRERR_1 = 10,
	A2B_PWRERR_2 = 11,
	A2B_PWRERR_3 = 12,
	A2B_PWRERR_4 = 13,
	/* 14 reserved */
	A2B_PWRERR_5 = 15,
	/* non-error interrupt type codes */
	A2B_I2CERR = 25,
	A2B_ICRCERR = 26,
	/* 27~40 reserved */
	A2B_PWRERR_6 = 41,
	A2B_PWRERR_7 = 42,
	/* 42~47 reserved */
	/* non-error interrupt type codes */
	/* 52~127 reserved */
	A2B_IRQMSGERR = 128,
	/* 129~251 reserved */
	A2B_STARTUPERR = 252,
	A2B_SLVINTTYPERR = 253,
	/* non-error interrupt type codes */
};

int a2b_node_read(struct a2b_node *node, unsigned int reg, unsigned int *val);
int a2b_node_write(struct a2b_node *node, unsigned int reg, unsigned int val);
int a2b_node_i2c_xfer(struct a2b_node *node, struct i2c_msg *msgs, int num);
int a2b_node_get_inttype(struct a2b_node *node, unsigned int *val);

void a2b_node_report_error(struct a2b_node *node, enum a2b_error error);

int a2b_node_request_slots_pre(struct a2b_node *node,
			       enum a2b_direction direction);
int a2b_node_request_slots(struct a2b_node *node, enum a2b_direction direction,
			   unsigned int slots, enum a2b_slot_size slot_size,
			   enum a2b_slot_format slot_format);

int a2b_register_node(struct a2b_node *node);
void a2b_unregister_node(struct a2b_node *node);

/**
 * A2B FUNC
 **/

struct a2b_func {
	struct device dev;
	struct a2b_node *node;
};

struct a2b_func *a2b_node_of_add_func(struct a2b_node *node,
				      struct device_node *np);

/**
 * A2B BUS
 **/

struct a2b_bus_ops;

/**
 * enum a2b_bus_status - A2B bus status bits
 *
 * @A2B_BUS_STATUS_DISCOVERING - the main node is currently in discovery mode,
 * i.e. DISCSTAT.DSCACT=1; used internally to ignore spurious bus errors
 * @A2B_BUS_STATUS_DISCOVERY - discovery (read: enumeration) of the whole bus is
 * in progress and the number of available nodes is not yet determined
 */
enum a2b_bus_status {
	A2B_BUS_STATUS_DISCOVERY,
	A2B_BUS_STATUS_DISCOVERING,
	A2B_BUS_STATUS_END,
};

struct a2b_bus_event_data {
	union {
		struct {
			unsigned int num_nodes;
		} discovery_done;
	};
};

/**
 * enum a2b_bus_event - events that are sent on the bus' blocking notifier chain
 *
 * @A2B_BUS_EVENT_DISCOVERY_DONE - discovery has finished
 */
enum a2b_bus_event {
	A2B_BUS_EVENT_DISCOVERY_DONE,
};

struct a2b_bus {
	/* A2B interface driver fills this in */
	const struct a2b_bus_ops *ops;
	struct device *parent;
	void *priv;

	/* A2B core only */
	struct device dev;
	int id;
	struct mutex mutex;
	unsigned int slotreqs[2];
	struct a2b_slot_config slot_config;
	struct a2b_node *nodes[A2B_MAX_NODES];
	unsigned int main_respcycs;
	unsigned long status;
	struct delayed_work discovery_work;
	struct blocking_notifier_head notifier;
};

int a2b_register_bus(struct a2b_bus *bus);
void a2b_unregister_bus(struct a2b_bus *bus);
struct a2b_bus *a2b_find_bus_by_of_node(struct device_node *np);
void a2b_put_bus(struct a2b_bus *bus);
unsigned long a2b_bus_status(struct a2b_bus *bus);
unsigned int a2b_bus_num_subs(struct a2b_bus *bus);
unsigned int a2b_bus_num_nodes(struct a2b_bus *bus);
int a2b_bus_register_notifier(struct a2b_bus *bus, struct notifier_block *nb);
int a2b_bus_unregister_notifier(struct a2b_bus *bus, struct notifier_block *nb);

/**
 * a2b_bus_ops - A2B host bus operations
 *
 * @read: register read from the address on the target node
 * @write: write with same semantics as @read
 * @i2c_xfer: perform a raw I2C transfer from a subordinate node's I2C interface
 * @get_inttype: in the event of an interrupt on a node, the node must use this
 *               function to determine what type of interrupt it has received
 */
struct a2b_bus_ops {
	int (*read)(struct a2b_bus *bus, const struct a2b_node *node,
		    unsigned int reg, unsigned int *val);
	int (*write)(struct a2b_bus *bus, const struct a2b_node *node,
		     unsigned int reg, unsigned int val);
	int (*i2c_xfer)(struct a2b_bus *bus, const struct a2b_node *node,
			struct i2c_msg *msgs, int num);
	int (*get_inttype)(struct a2b_bus *bus, unsigned int *val);
};

/**
 * BUS DRIVER
 **/

struct a2b_driver {
	struct device_driver driver;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
};

#define to_a2b_driver(drv) container_of(drv, struct a2b_driver, driver)

int __a2b_driver_register(struct a2b_driver *a2b_drv, struct module *owner);
void a2b_driver_unregister(struct a2b_driver *a2b_drv);

#define a2b_driver_register(a2b_drv) __a2b_driver_register(a2b_drv, THIS_MODULE)
#define module_a2b_driver(__a2b_driver) \
	module_driver(__a2b_driver, a2b_driver_register, a2b_driver_unregister)

#define to_a2b_node(dev) container_of_const(dev, struct a2b_node, dev)
#define to_a2b_func(dev) container_of_const(dev, struct a2b_func, dev)

extern const struct device_type a2b_node_type;
extern const struct device_type a2b_func_type;
extern const struct bus_type a2b_bus;

static inline struct a2b_bus *to_a2b_bus(struct device *dev)
{
	return container_of(dev, struct a2b_bus, dev);
}

extern const struct device_type a2b_bus_type;
extern const struct class a2b_bus_class;

#endif /* _A2B_H_ */
