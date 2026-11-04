// SPDX-License-Identifier: GPL-2.0-only
/* Temporary opt-in property for testing the exact production driver. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

static struct device_node *node;
static struct property *property;

static int __init onyx_cpuss_dt_diag_init(void)
{
    int ret;

    node = of_find_node_by_path("/soc/cpuss-sleep-stats@17800054");
    if (!node)
        return -ENODEV;
    if (!of_device_is_compatible(node, "qcom,cpuss-sleep-stats") ||
        of_find_property(node, "qcom,config-mmio", NULL)) {
        of_node_put(node);
        return -EINVAL;
    }
    property = kzalloc(sizeof(*property), GFP_KERNEL);
    if (!property) {
        of_node_put(node);
        return -ENOMEM;
    }
    property->name = kstrdup("qcom,config-mmio", GFP_KERNEL);
    if (!property->name) {
        kfree(property);
        of_node_put(node);
        return -ENOMEM;
    }
    ret = of_add_property(node, property);
    if (ret) {
        kfree(property->name);
        kfree(property);
        of_node_put(node);
        return ret;
    }
    pr_info("ONYX_CPUSS_DT_DIAG temporary qcom,config-mmio property added; no MMIO writes\n");
    return 0;
}

static void __exit onyx_cpuss_dt_diag_exit(void)
{
    int ret = of_remove_property(node, property);

    pr_info("ONYX_CPUSS_DT_DIAG property removal ret=%d\n", ret);
    /* OF keeps removed properties on node->deadprops; do not free them. */
    of_node_put(node);
}

module_init(onyx_cpuss_dt_diag_init);
module_exit(onyx_cpuss_dt_diag_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temporary Onyx CPUSS DT opt-in diagnostic");
