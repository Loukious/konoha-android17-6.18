// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

struct qfprom_sys {
	int cell_count;
	struct nvmem_cell **cells;
	struct bin_attribute **attrs;
};

static ssize_t qfprom_sys_cell_read(struct file *filp, struct kobject *kobj,
				const struct bin_attribute *attr,
				char *buf, loff_t pos, size_t count)
{
	struct nvmem_cell *cell;
	size_t len;
	u8 *data;

	cell = attr->private;
	if (!cell)
		return -EINVAL;

	if (pos < 0)
		return -EINVAL;
	data = nvmem_cell_read(cell, &len);
	if (IS_ERR(data))
		return PTR_ERR(data);

	if (pos >= len) {
		kfree(data);
		return 0;
	}
	len = min_t(size_t, len - pos, count);
	memcpy(buf, data + pos, len);
	kfree(data);
	return len;
}

static int qfprom_sys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qfprom_sys *priv;
	struct nvmem_cell *cell;
	int cell_count;
	int i, ret;
	const char **cells;

	cell_count = of_property_count_strings(dev->of_node, "nvmem-cell-names");
	if (cell_count <= 0)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(struct qfprom_sys), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cells = devm_kcalloc(dev, cell_count, sizeof(*priv->cells), GFP_KERNEL);
	if (!priv->cells)
		return -ENOMEM;

	cells = devm_kcalloc(dev, cell_count, sizeof(*cells), GFP_KERNEL);
	if (!cells)
		return -ENOMEM;

	priv->attrs = devm_kcalloc(dev, cell_count, sizeof(*priv->attrs), GFP_KERNEL);
	if (!priv->attrs)
		return -ENOMEM;

	ret = of_property_read_string_array(dev->of_node, "nvmem-cell-names",
						cells, cell_count);
	if (ret < 0)
		return ret;
	if (ret != cell_count)
		return -EINVAL;

	for (i = 0; i < cell_count; i++) {
		cell = nvmem_cell_get(dev, cells[i]);
		if (IS_ERR(cell)) {
			ret = PTR_ERR(cell);
			dev_err(dev, "cell corresponding to %s not found\n",
					cells[i]);
			goto remove_cells;
		}

		priv->attrs[i] = devm_kzalloc(dev, sizeof(struct bin_attribute),
						GFP_KERNEL);
		if (!priv->attrs[i]) {
			ret = -ENOMEM;
			nvmem_cell_put(cell);
			goto remove_cells;
		}

		priv->cells[i] = cell;
		sysfs_bin_attr_init(priv->attrs[i]);
		priv->attrs[i]->attr.name = cells[i];
		priv->attrs[i]->attr.mode = 0444;
		priv->attrs[i]->private = cell;
		priv->attrs[i]->size = 4;
		priv->attrs[i]->read = qfprom_sys_cell_read;
		ret = sysfs_create_bin_file(&dev->kobj, priv->attrs[i]);
		if (ret) {
			dev_err(dev, "cell corresponding to %s\n", cells[i]);
			nvmem_cell_put(cell);
			goto remove_cells;
		}
	}

	priv->cell_count = cell_count;
	platform_set_drvdata(pdev, priv);
	return 0;

remove_cells:
	for (; i > 0; i--) {
		sysfs_remove_bin_file(&dev->kobj, priv->attrs[i-1]);
		nvmem_cell_put(priv->cells[i-1]);
	}
	priv->cell_count = 0;
	return ret;
}

static void qfprom_sys_remove(struct platform_device *pdev)
{
	struct qfprom_sys *priv;
	int i;

	priv = platform_get_drvdata(pdev);
	for (i = 0; i < priv->cell_count; i++) {
		sysfs_remove_bin_file(&pdev->dev.kobj, priv->attrs[i]);
		nvmem_cell_put(priv->cells[i]);
	}
}

static const struct of_device_id qfprom_sys_of_match[] = {
	{ .compatible = "qcom,qfprom-sys",},
	{},
};

MODULE_DEVICE_TABLE(of, qfprom_sys_of_match);

static struct platform_driver qfprom_sys_driver = {
	.probe = qfprom_sys_probe,
	.remove = qfprom_sys_remove,
	.driver = {
		.name = "qcom,qfprom-sys",
		.of_match_table = qfprom_sys_of_match,
	},
};

module_platform_driver(qfprom_sys_driver);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. QFPROM_SYS driver");
MODULE_LICENSE("GPL");
