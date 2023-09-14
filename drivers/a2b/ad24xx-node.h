/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AD24xx A2B transceiver node driver extension header
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * Use this to derive your own custom A2B node driver.
 */
#ifndef _AD24XX_NODE_H_
#define _AD24XX_NODE_H_

#include <linux/a2b/a2b.h>

int ad24xx_node_set_respcycs(struct a2b_node *node, unsigned int respcycs);
int ad24xx_node_set_switching(struct a2b_node *node, bool enable,
			      enum a2b_swmode mode);
int ad24xx_node_discover(struct a2b_node *node, unsigned int respcycs);
int ad24xx_node_new_structure(struct a2b_node *node,
			      const struct a2b_slot_config *slot_config);
int ad24xx_node_is_last(struct a2b_node *node);
int ad24xx_node_setup(struct a2b_node *node);
void ad24xx_node_teardown(struct a2b_node *node);

#endif /* _AD24XX_NODE_H_ */
