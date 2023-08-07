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
 * A2B NODE
 **/

#define A2B_MAX_NODES 16
#define A2B_MAIN_ADDR (A2B_MAX_NODES - 1)

struct a2b_node;

struct a2b_node_ops {
	int (*setup)(struct a2b_node *node);
	void (*teardown)(struct a2b_node *node);
	int (*set_respcycs)(struct a2b_node *node, unsigned int respcycs);
	int (*set_switching)(struct a2b_node *node, unsigned int value);
	int (*discover)(struct a2b_node *node, unsigned int respcycs);
	int (*new_structure)(struct a2b_node *node);
};

struct a2b_node {
	struct device dev;

	/* A2B node driver fills this in */
	const struct a2b_node_ops *ops;
	enum a2b_tdm_mode tdm_mode;
	enum a2b_tdm_slot_size tdm_slot_size;
	enum a2b_superframe_freq sff;
	unsigned int invert_sync : 1;
	unsigned int early_sync : 1;
	unsigned int alternating_sync : 1;
	unsigned int rx_on_dtx1 : 1;
	unsigned int upfmt;
	unsigned int dnfmt;
	unsigned int dnss;
	unsigned int upss;
	void *priv;
	
	/* A2B core only */
	bool setup;
	struct a2b_bus *bus;
	unsigned int addr;
	unsigned int num_dnslots;
	unsigned int num_upslots;
};

static inline bool is_a2b_main(struct a2b_node *node)
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

void a2b_node_report_error(struct a2b_node *node, enum a2b_error error);

enum a2b_direction {
	A2B_DIR_UP,
	A2B_DIR_DOWN,
	A2B_DIR_END,
};

int a2b_node_request_slots_pre(struct a2b_node *node,
			       enum a2b_direction direction);
int a2b_node_request_slots(struct a2b_node *node, enum a2b_direction direction,
			   unsigned int slots);
/* int a2b_node_request_slots_post(struct a2b_node *node); */

struct a2b_node *a2b_bus_of_add_node(struct a2b_bus *bus,
				     struct device_node *np, unsigned int addr);

int a2b_register_node(struct a2b_node *node);
void a2b_unregister_node(struct a2b_node *node);

/**
 * A2B FUNC
 **/

struct a2b_func {
	struct device dev;
	struct a2b_node *node;
};

int a2b_func_read(struct a2b_func *func, unsigned int reg, unsigned int *val,
		  int flags);
int a2b_func_write(struct a2b_func *func, unsigned int reg, unsigned int val,
		   int flags);

struct a2b_func *a2b_node_of_add_func(struct a2b_node *node,
				      struct device_node *np);

/**
 * A2B BUS
 **/

struct a2b_bus_ops;

/**
 * enum a2b_bus_status - A2B bus status bits
 *
 * @A2B_BUS_STATUS_DISCOVERING - discovery of the bus is in progress and the
 * number of available nodes is not yet determined
 */
enum a2b_bus_status {
	A2B_BUS_STATUS_DISCOVERING,
	A2B_BUS_STATUS_END,
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
	/* A2B bus driver fills this in */
	struct device *dev;
	const struct a2b_bus_ops *ops;
	void *priv;

	/* A2B core only */
	int id;
	struct list_head list;
	struct mutex mutex;
	unsigned int use_count;
	unsigned int slotreqs[2];
	struct a2b_node *nodes[A2B_MAX_NODES];
	unsigned int respcycs[A2B_MAX_NODES];
	unsigned long status;
	struct delayed_work discovery_work;
	enum a2b_tdm_mode tdm_mode;
	enum a2b_tdm_slot_size tdm_slot_size;
	struct blocking_notifier_head notifier;
};

int a2b_register_bus(struct a2b_bus *bus);
void a2b_unregister_bus(struct a2b_bus *bus);
struct a2b_bus *a2b_get_bus(struct device_node *np);
void a2b_put_bus(struct a2b_bus *bus);
unsigned long a2b_bus_status(struct a2b_bus *bus);
unsigned int a2b_bus_num_subs(struct a2b_bus *bus);
unsigned int a2b_bus_num_nodes(struct a2b_bus *bus);
int a2b_bus_register_notifier(struct a2b_bus *bus, struct notifier_block *nb);
int a2b_bus_unregister_notifier(struct a2b_bus *bus, struct notifier_block *nb);

/**
 * enum a2b_rw_flags - A2B register access flags
 * @A2B_RW_I2CPERIPHERAL: the register being accessed resides on an attached I2C
 *                        peripheral
 * @A2B_RW_BROADCAST: write access only; rather than writing to a register in a
 *                    single node, the write is written to the same register in
 *                    all nodes simultaneously
 */
enum a2b_rw_flags {
	A2B_RW_I2CPERIPHERAL = BIT(0),
	A2B_RW_BROADCAST = BIT(1),
};

struct i2c_msg;

/**
 * a2b_bus_ops - A2B host bus operations
 * @lock: lock the bus to prevent concurrent access
 * @unlock: unlock the bus
 * @read: read from the address on the given node
 * @write: write with same semantics as @read
 * @get_inttype: in the event of an interrupt on a node, the node must use this
 *               function to determine what type of interrupt it has received
 */
struct a2b_bus_ops {
	void (*lock)(struct a2b_bus *bus);
	void (*unlock)(struct a2b_bus *bus);
	int (*read)(struct a2b_bus *bus, const struct a2b_node *node,
		    unsigned int reg, unsigned int *val, int flags);
	int (*write)(struct a2b_bus *bus, const struct a2b_node *node,
		     unsigned int reg, unsigned int val, int flags);
	int (*i2c_xfer)(struct a2b_bus *bus, const struct a2b_node *node,
			struct i2c_msg *msgs, int num);
	int (*get_inttype)(struct a2b_bus *bus, unsigned int *val);

	// TODO review this logic
	int (*read_nolock)(struct a2b_bus *bus, const struct a2b_node *node,
		    unsigned int reg, unsigned int *val, int flags);
	int (*write_nolock)(struct a2b_bus *bus, const struct a2b_node *node,
		     unsigned int reg, unsigned int val, int flags);
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

static inline struct a2b_node *to_a2b_node(struct device *dev)
{
	return container_of(dev, struct a2b_node, dev);
}

static inline struct a2b_func *to_a2b_func(struct device *dev)
{
	return container_of(dev, struct a2b_func, dev);
}

extern struct bus_type a2b_bus_type;

extern struct device_type a2b_node_type;
extern struct device_type a2b_func_type;

#endif /* _A2B_H_ */
