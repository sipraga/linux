// SPDX-License-Identifier: GPL-2.0-only
/*
 * A custom audio-graph-card2 driver for A2B sound cards
 *
 * The card will defer its probe until the A2B bus has finished its discovery
 * process, after which any absent codecs will be skipped without error. It will
 * also automatically glue the A2B fabric together in DAPM to ensure seamless
 * DPCM operation.
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <sound/graph_card.h>
#include <sound/simple_card_utils.h>
#include <linux/version.h>

/* Assume nobody uses more than 5 A2B buses at once */
#define MAX_A2BS 5

struct a2b_graph_priv {
	struct device *dev;
	struct simple_util_priv simple_priv;
	unsigned int num_a2bs;
	unsigned int num_a2b_nodes[MAX_A2BS];
};

#define simple_to_a2b_graph(simple) \
	container_of((simple), struct a2b_graph_priv, simple_priv)

static int a2b_graph_dpcm_set_dailink_name(struct a2b_graph_priv *priv,
					   struct snd_soc_dai_link *dai_link)
{
	struct simple_util_priv *simple_priv = &priv->simple_priv;
	struct snd_soc_card *card = simple_priv_to_card(simple_priv);
	struct device *dev = priv->dev;
	int i;

	if (!dai_link->name)
		return -EINVAL;

	/* HACK: Add a numerical index to prevent naming collisions. */

	for (i = 0; i < card->num_links; i++)
		if (&card->dai_link[i] == dai_link)
			break;

	WARN_ON(i == card->num_links);

	return simple_util_set_dailink_name(dev, dai_link, "%s.%d",
					    dai_link->name, i);
}

static int a2b_graph_parse_routes(struct a2b_graph_priv *priv,
				  struct device_node *lnk)
{
	struct simple_util_priv *simple_priv = &priv->simple_priv;
	struct snd_soc_card *card = simple_priv_to_card(simple_priv);
	struct device_node *np = of_node_get(lnk);
	struct snd_soc_dapm_route *routes;
	int num_routes;
	int ret = 0;
	int i;

	num_routes = of_property_count_strings(np, "routing");
	if (num_routes < 0)
		goto out;
	else if (num_routes & 1) {
		ret = -EINVAL;
		goto out;
	}
	num_routes /= 2;

	/*
	 * HACK: The (void *) cast is necessary to discard the const-ness of
	 * snd_soc_card::of_dapm_routes. This is safe because we know it is also
	 * allocated dyanmically via devm_kcalloc() in
	 * snd_soc_of_parse_audio_routing().
	 */
	routes = devm_krealloc(card->dev, (void *)card->of_dapm_routes,
			       sizeof(*routes) *
				       (card->num_of_dapm_routes + num_routes),
			       GFP_KERNEL);
	if (!routes) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < num_routes; i++) {
		int j = card->num_of_dapm_routes + i; /* offset into routes */

		ret = of_property_read_string_index(np, "routing", 2 * i,
						    &routes[j].sink);
		if (ret)
			goto out;

		ret = of_property_read_string_index(np, "routing", (2 * i) + 1,
						    &routes[j].source);
		if (ret)
			goto out;
	}

	card->of_dapm_routes = routes;
	card->num_of_dapm_routes += num_routes;

out:
	of_node_put(np);

	return ret;
}

static int a2b_graph_parse_aux_devs(struct a2b_graph_priv *priv,
				    struct device_node *lnk)
{
	struct simple_util_priv *simple_priv = &priv->simple_priv;
	struct snd_soc_card *card = simple_priv_to_card(simple_priv);
	struct device_node *np = of_node_get(lnk);
	struct snd_soc_aux_dev *aux;
	int num;
	int ret = 0;
	int i;

	num = of_count_phandle_with_args(np, "aux-devs", NULL);
	if (num == -ENOENT) {
		goto out;
	} else if (num < 0) {
		ret = -EINVAL;
		goto out;
	}

	aux = devm_krealloc(card->dev, (void *)card->aux_dev,
			       sizeof(*aux) * (card->num_aux_devs + num),
			       GFP_KERNEL);
	if (!aux) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < num; i++) {
		int j = card->num_aux_devs + i; /* offset into routes */

		aux[j].dlc.of_node = of_parse_phandle(np, "aux-devs", i);
		if (!aux[j].dlc.of_node) {
			ret = -EINVAL;
			goto out;
		}
	}

	card->aux_dev = aux;
	card->num_aux_devs += num;

out:
	of_node_put(np);

	return ret;
}

static int a2b_graph_dpcm(struct simple_util_priv *simple_priv,
			  struct device_node *lnk, struct link_info *li)
{
	struct a2b_graph_priv *priv = simple_to_a2b_graph(simple_priv);
	struct snd_soc_dai_link *dai_link =
		simple_priv_to_link(simple_priv, li->link);
	int ret;

	ret = audio_graph2_link_dpcm(simple_priv, lnk, li);
	if (ret)
		return ret;

	/* Add any conditional routes necessary for this link */
	ret = a2b_graph_parse_routes(priv, lnk);
	if (ret)
		return ret;

	/* And aux devs too */
	ret = a2b_graph_parse_aux_devs(priv, lnk);
	if (ret)
		return ret;

	/* Name the link uniquely to avoid collisions */
	ret = a2b_graph_dpcm_set_dailink_name(priv, dai_link);
	if (ret)
		return ret;

	dai_link->ignore_pmdown_time = 1;

	return 0;
}

static int a2b_graph_hook_skip_link(struct simple_util_priv *simple_priv,
				    struct device_node *lnk, int index)
{
	struct a2b_graph_priv *priv = simple_to_a2b_graph(simple_priv);
	struct device_node *ep;
	struct device_node *np;
	u32 reg = U32_MAX;
	bool skip = false;
	int i;

	/*
	 * Check which A2B node the codec lives on. If that node has not been
	 * discovered, the link will be skipped. A2B node 0 is the main node.
	 *
	 * Consider this example where two links l0 and l1 refer to either the
	 * codec of the 5th A2B subordinate node (node@5), or to a codec
	 * connected to that node's I2C bus. For l0, start at (A0) and resolve
	 * the remote-endpoint to (B0). Traverse the parents upwards; before
	 * reaching the bus node (D), the reg value will be stored at (C).
	 * This also works for (A0) -> (B0) -> (C) -> (D).
	 *
	 *   a2b-card {
	 *     links = <&l0 &l1 ...>;
	 *     adi,a2b-bus = <&a2b>;
	 *
	 *     ... {
	 * (A0)  l0: port@0 { l0_ep: endpoint { remote-endpoint = <&c0_ep> }; };
	 * (A1)  l1: port@1 { l1_ep: endpoint { remote-endpoint = <&c1_ep> }; };
	 *       ...
	 *     };
	 *   };
	 *
	 *   i2c {
	 * (D) a2b: a2b@68 {
	 *       ...
	 *       node@5 {
	 * (C)     reg = <5>;
	 *
	 *         codec {
	 * (B0)      c0_ep: endpoint { remote-endpoint = <&l0_ep>; };
	 *         };
	 *
	 *         i2c {
	 *           codec {
	 * (B1)        c1_ep: endpoint { remote-endpoint = <&l1_ep>; };
	 *           };
	 *         };
	 *       };
	 *     };
	 *   };
	 *
	 * If the A2B bus node is not a parent, the link is not on the bus and
	 * will not be skipped.
	 */

	ep = of_get_child_by_name(lnk, "endpoint");
	np = of_graph_get_remote_endpoint(ep);
	of_node_put(ep);
	while (np) {
		bool is_a2b = false;

		for (i = 0; i < priv->num_a2bs; i++) {
			struct device_node *a2b_np = of_parse_phandle(
				priv->dev->of_node, "adi,a2b-bus", i);

			if (np == a2b_np) {
				of_node_put(a2b_np);
				is_a2b = true;
				break;
			}

			of_node_put(a2b_np);
		}

		if (is_a2b) {
			of_node_put(np);
			if (reg >= priv->num_a2b_nodes[i])
				skip = true;
			break;
		}

		if (of_property_read_u32(np, "reg", &reg))
			reg = U32_MAX;

		np = of_get_next_parent(np);
	}

	return skip ? 1 : 0;
}

static int a2b_graph_link_widgets(struct snd_soc_card *card,
				  struct snd_soc_component *c1, const char *s1,
				  struct snd_soc_component *c2, const char *s2)
{
	struct snd_soc_dapm_route route = {};
	int ret;

	route.source = kasprintf(GFP_KERNEL, "%s %s", c1->name_prefix, s1);
	route.sink = kasprintf(GFP_KERNEL, "%s %s", c2->name_prefix, s2);

	if (!route.source || !route.sink)
		ret = -ENOMEM;
	else
		ret = snd_soc_dapm_add_routes(&card->dapm, &route, 1);

	kfree(route.source);
	kfree(route.sink);

	return ret;
}

static int a2b_graph_card_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_component *c1, *c2;
	struct a2b_node *n1, *n2;
	int ret;

	/*
	 * Connect A2B transceiver widgets together to ensure a coherent DPCM
	 * topology. This ensures that hw_params will get set on the entire A2B
	 * chain.
	 */
	for_each_card_components(card, c1) {
		if (!c1->driver->name ||
		    strcmp(c1->driver->name, "ad24xx-codec"))
			continue;

		for_each_card_components(card, c2) {
			if (!c2->driver->name ||
			    strcmp(c2->driver->name, "ad24xx-codec"))
				continue;

			n1 = to_a2b_func(c1->dev)->node;
			n2 = to_a2b_func(c2->dev)->node;

			/* Ensure nodes are on the same bus */
			if (n1->bus != n2->bus)
				continue;

			/* Ensure n1 immediately precedes n2 in the A2B bus */
			if (n1->addr > n2->addr || (n2->addr - n1->addr != 1))
				continue;

			/* Connect downstream and upstream transceivers */
			ret = a2b_graph_link_widgets(card, c1, "TRXB DN", c2,
						     "TRXA DN");
			if (ret)
				return ret;

			ret = a2b_graph_link_widgets(card, c2, "TRXA UP", c1,
						     "TRXB UP");
			if (ret)
				return ret;
		}
	}

	return graph_util_card_probe(card);
}

static int a2b_graph_hook_post(struct simple_util_priv *priv)
{
	struct snd_soc_card *card = simple_priv_to_card(priv);

	card->late_probe = a2b_graph_card_late_probe;

	return 0;
}

static struct graph2_custom_hooks a2b_graph_hooks = {
	.custom_dpcm = a2b_graph_dpcm,
	.hook_skip_link = a2b_graph_hook_skip_link,
	.hook_post = a2b_graph_hook_post,
};

static int a2b_graph_get_a2bs(struct a2b_graph_priv *priv)
{
	struct device *dev = priv->dev;
	int num_a2bs;
	int i;

	num_a2bs =
		of_count_phandle_with_args(dev->of_node, "adi,a2b-bus", NULL);
	if (num_a2bs <= 0)
		return 0;
	else if (num_a2bs > MAX_A2BS)
		return -E2BIG;

	for (i = 0; i < num_a2bs; i++) {
		struct device_node *np;
		struct a2b_bus *a2b_bus;
		unsigned int num_a2b_nodes;
		unsigned int min_a2b_nodes;

		np = of_parse_phandle(dev->of_node, "adi,a2b-bus", i);
		if (!np)
			return -EINVAL;

		a2b_bus = a2b_find_bus_by_of_node(np);
		if (!a2b_bus) {
			of_node_put(np);
			return -EPROBE_DEFER;
		}

		if (a2b_bus_status(a2b_bus) & BIT(A2B_BUS_STATUS_ENUMERATION)) {
			a2b_put_bus(a2b_bus);
			of_node_put(np);
			return -EPROBE_DEFER;
		}

		num_a2b_nodes = a2b_bus_num_nodes(a2b_bus);
		a2b_put_bus(a2b_bus);
		of_node_put(np);

		if (of_property_read_u32_index(dev->of_node,
					       "adi,minimum-a2b-nodes", i,
					       &min_a2b_nodes))
			min_a2b_nodes = 1;
		else if (min_a2b_nodes == 0)
			return -EINVAL;

		if (num_a2b_nodes < min_a2b_nodes)
			return -ENODEV;

		priv->num_a2b_nodes[i] = num_a2b_nodes;
	}

	priv->num_a2bs = num_a2bs;

	return 0;
}

static int a2b_graph_probe(struct platform_device *pdev)
{
	struct a2b_graph_priv *priv;
	struct simple_util_priv *simple_priv;
	struct snd_soc_card *card;
	struct device *dev = &pdev->dev;
	struct device_node *np;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	platform_set_drvdata(pdev, priv);
	priv->dev = dev;
	simple_priv = &priv->simple_priv;

	/*
	 * Use component chaining, since it is likely that the BE DAIs will be
	 * connected together.
	 */
	card = simple_priv_to_card(simple_priv);
	card->component_chaining = 1;
	card->fully_routed = 1;

	/*
	 * HACK: To allow probing even with fw_devlink=on, purge unwanted device
	 * links. In reality fw_devlink should be able to resolve the cyclic
	 * dependency, but due to the fact that the top-level A2B I2C device
	 * finishes its binding before its child devices are even necessarily
	 * created - think codecs in particular - fw_devlink=on purges one half
	 * of the dependency graph before it can detect cycles. For more
	 * information, see the comment at the top of
	 * device_links_driver_bound().
	 *
	 * TODO: Reach out to Saravana for help with this when A2B is
	 * upstreamed.
	 */
	for_each_child_of_node(dev->of_node, np)
		fw_devlink_purge_absent_suppliers(&np->fwnode);

	ret = a2b_graph_get_a2bs(priv);
	if (ret)
		return ret;

	/* Start the audio-graph-card2 probe */
	ret = audio_graph2_parse_of(simple_priv, dev, &a2b_graph_hooks);
	if (ret)
		return ret;

	return 0;
}

static void a2b_graph_remove(struct platform_device *pdev)
{
	simple_util_remove(pdev);
}

static const struct of_device_id a2b_graph_of_match[] = {
	{
		.compatible = "adi,a2b-audio-graph-card2",
	},
	{},
};
MODULE_DEVICE_TABLE(of, a2b_graph_of_match);

static struct platform_driver a2b_graph_card = {
	.driver = {
		.name = "a2b-audio-graph-card2",
		.of_match_table = a2b_graph_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe	= a2b_graph_probe,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,13,0)
	.remove = a2b_graph_remove,
#else
	.remove_new = a2b_graph_remove,
#endif
};
module_platform_driver(a2b_graph_card);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ASoC A2B Audio Graph Card2");
MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
