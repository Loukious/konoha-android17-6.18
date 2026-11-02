// SPDX-License-Identifier: GPL-2.0
/*
 * mca_sysfs.c
 *
 * mca sysfs driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#include <mca/common/mca_sysfs.h>

#define MCA_SYSFS_DEFAULT_DEV_NUM 5

#define MCA_DBG_ROOT_DIR "charger_debug"

static LIST_HEAD(g_mca_sysfs_list_header);
static DEFINE_MUTEX(g_mca_sysfs_class_lock);

static const char * const g_mca_sysfs_dev_name_list[MCA_SYSFS_DEFAULT_DEV_NUM] = {
	"charger", "fuelgauge", "typec", "battery", "hw_monitor",
};

static struct mca_sysfs_class_node *
mca_sysfs_get_class_node(const char *cls_name)
{
	struct list_head *header = &g_mca_sysfs_list_header;
	struct mca_sysfs_class_node *l_node = NULL;

	if (list_empty(header))
		return NULL;

	list_for_each_entry(l_node, header, node) {
		if (!strcmp(l_node->class_name, cls_name))
			return l_node;
	}

	return NULL;
}

/* Caller holds g_mca_sysfs_class_lock. Classes live until module exit. */
static struct mca_sysfs_class_node *mca_sysfs_get_or_create_class(const char *cls_name)
{
	struct mca_sysfs_class_node *node;

	node = mca_sysfs_get_class_node(cls_name);
	if (node)
		return node;
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return NULL;
	node->class_name = kstrdup(cls_name, GFP_KERNEL);
	if (!node->class_name)
		goto free_node;
	node->mca_class = class_create(cls_name);
	if (IS_ERR(node->mca_class))
		goto free_name;
	INIT_LIST_HEAD(&node->dev_list_header);
	list_add_tail(&node->node, &g_mca_sysfs_list_header);
	return node;
free_name:
	kfree(node->class_name);
free_node:
	kfree(node);
	return NULL;
}

static struct device *
mca_sysfs_get_device(struct mca_sysfs_class_node *class_node, const char *name)
{
	struct mca_sysfs_dev_node *dev_node = NULL;

	if (list_empty(&class_node->dev_list_header))
		return NULL;

	list_for_each_entry(dev_node, &class_node->dev_list_header, node) {
		if (!strcmp(dev_node->name, name))
			return get_device(dev_node->dev);
	}

	return NULL;
}

struct device *mca_sysfs_create_group(const char *cls_name,
				    const char *dev_name,
				    const struct attribute_group *group)
{
	struct mca_sysfs_class_node *class_node;
	struct mca_sysfs_dev_node *dev_node;
	struct device *dev, *existing;

	if (!cls_name || !dev_name || !group)
		return NULL;
	dev_node = kzalloc(sizeof(*dev_node), GFP_KERNEL);
	if (!dev_node)
		return NULL;
	dev_node->name = kstrdup(dev_name, GFP_KERNEL);
	if (!dev_node->name)
		goto free_node;
	mutex_lock(&g_mca_sysfs_class_lock);
	class_node = mca_sysfs_get_or_create_class(cls_name);
	if (!class_node)
		goto unlock;
	existing = mca_sysfs_get_device(class_node, dev_name);
	if (existing) {
		put_device(existing);
		goto unlock;
	}
	dev = device_create(class_node->mca_class, NULL, 0, NULL, "%s", dev_name);
	if (IS_ERR(dev))
		goto unlock;
	if (sysfs_create_group(&dev->kobj, group)) {
		device_unregister(dev);
		goto unlock;
	}
	dev_node->dev = dev;
	list_add_tail(&dev_node->node, &class_node->dev_list_header);
	mutex_unlock(&g_mca_sysfs_class_lock);
	return dev;
unlock:
	mutex_unlock(&g_mca_sysfs_class_lock);
	kfree(dev_node->name);
free_node:
	kfree(dev_node);
	return NULL;
}
EXPORT_SYMBOL(mca_sysfs_create_group);

void mca_sysfs_remove_group(const char *cls_name, struct device *dev,
			    const struct attribute_group *group)
{
	struct mca_sysfs_class_node *class_node;
	struct mca_sysfs_dev_node *node, *found = NULL;

	if (!cls_name || !dev || !group)
		return;
	mutex_lock(&g_mca_sysfs_class_lock);
	class_node = mca_sysfs_get_class_node(cls_name);
	if (class_node) {
		list_for_each_entry(node, &class_node->dev_list_header, node) {
			if (node->dev == dev) {
				list_del(&node->node);
				found = node;
				break;
			}
		}
	}
	mutex_unlock(&g_mca_sysfs_class_lock);
	if (!found)
		return;
	sysfs_remove_group(&dev->kobj, group);
	device_unregister(dev);
	kfree(found->name);
	kfree(found);
}
EXPORT_SYMBOL(mca_sysfs_remove_group);

static struct device *mca_sysfs_get_power_device(const char *name)
{
	struct mca_sysfs_class_node *class_node;
	struct device *dev = NULL;

	mutex_lock(&g_mca_sysfs_class_lock);
	class_node = mca_sysfs_get_class_node("xm_power");
	if (class_node)
		dev = mca_sysfs_get_device(class_node, name);
	mutex_unlock(&g_mca_sysfs_class_lock);
	return dev;
}

int mca_sysfs_create_link_group(const char *dev_name, const char *link_name,
				struct device *target_dev,
				const struct attribute_group *group)
{
	struct device *dev;
	int ret;

	if (!dev_name || !link_name || !target_dev || !group)
		return -EINVAL;
	dev = mca_sysfs_get_power_device(dev_name);
	if (!dev)
		return -ENODEV;
	ret = sysfs_create_group(&target_dev->kobj, group);
	if (!ret) {
		ret = sysfs_create_link(&dev->kobj, &target_dev->kobj, link_name);
		if (ret)
			sysfs_remove_group(&target_dev->kobj, group);
	}
	put_device(dev);
	return ret;
}
EXPORT_SYMBOL(mca_sysfs_create_link_group);

void mca_sysfs_remove_link_group(const char *dev_name, const char *link_name,
				 struct device *target_dev,
				 const struct attribute_group *group)
{
	struct device *dev;

	if (!dev_name || !link_name || !target_dev || !group)
		return;
	dev = mca_sysfs_get_power_device(dev_name);
	if (dev) {
		sysfs_remove_link(&dev->kobj, link_name);
		put_device(dev);
	}
	sysfs_remove_group(&target_dev->kobj, group);
}
EXPORT_SYMBOL(mca_sysfs_remove_link_group);

int mca_sysfs_create_files(const char *dev_name,
			   struct mca_sysfs_attr_info *attr, const int attr_size)
{
	struct device *dev;
	int i, ret = 0;

	if (!dev_name || !attr || attr_size <= 0)
		return -EINVAL;
	dev = mca_sysfs_get_power_device(dev_name);
	if (!dev)
		return -ENODEV;
	for (i = 0; i < attr_size; i++) {
		ret = sysfs_create_file(&dev->kobj, &attr[i].attr.attr);
		if (ret) {
			while (i--)
				sysfs_remove_file(&dev->kobj, &attr[i].attr.attr);
			break;
		}
	}
	put_device(dev);
	return ret;
}
EXPORT_SYMBOL(mca_sysfs_create_files);

void mca_sysfs_remove_files(const char *dev_name,
			    struct mca_sysfs_attr_info *attr, int attr_size)
{
	struct device *dev;
	int i;

	if (!dev_name || !attr || attr_size <= 0)
		return;
	dev = mca_sysfs_get_power_device(dev_name);
	if (!dev)
		return;
	for (i = 0; i < attr_size; i++)
		sysfs_remove_file(&dev->kobj, &attr[i].attr.attr);
	put_device(dev);
}
EXPORT_SYMBOL(mca_sysfs_remove_files);

void mca_sysfs_init_attrs(struct attribute **attrs,
			  struct mca_sysfs_attr_info *attr_info, int size)
{
	int i;

	if (!attrs || !attr_info || size < 0)
		return;

	for (i = 0; i < size; i++)
		attrs[i] = &attr_info[i].attr.attr;

	attrs[size] = NULL;
}
EXPORT_SYMBOL(mca_sysfs_init_attrs);

struct mca_sysfs_attr_info *
mca_sysfs_lookup_attr(const char *name, struct mca_sysfs_attr_info *attr_info,
		      int size)
{
	int i;

	if (!name || !attr_info)
		return NULL;

	for (i = 0; i < size; i++) {
		if (!strcmp(name, attr_info[i].attr.attr.name))
			return &attr_info[i];
	}

	return NULL;
}
EXPORT_SYMBOL(mca_sysfs_lookup_attr);

static void mca_sysfs_cleanup(void)
{
	struct mca_sysfs_class_node *cls, *cls_tmp;
	struct mca_sysfs_dev_node *dev, *dev_tmp;

	list_for_each_entry_safe(cls, cls_tmp, &g_mca_sysfs_list_header, node) {
		list_del(&cls->node);
		list_for_each_entry_safe(dev, dev_tmp, &cls->dev_list_header, node) {
			list_del(&dev->node);
			device_unregister(dev->dev);
			kfree(dev->name);
			kfree(dev);
		}
		class_destroy(cls->mca_class);
		kfree(cls->class_name);
		kfree(cls);
	}
}

static int mca_sysfs_init(void)
{
	struct mca_sysfs_class_node *cls;
	struct mca_sysfs_dev_node *node;
	int i, ret = -ENOMEM;

	mutex_lock(&g_mca_sysfs_class_lock);
	cls = mca_sysfs_get_or_create_class("xm_power");
	if (!cls)
		goto error;
	for (i = 0; i < MCA_SYSFS_DEFAULT_DEV_NUM; i++) {
		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (!node)
			goto error;
		node->name = kstrdup(g_mca_sysfs_dev_name_list[i], GFP_KERNEL);
		if (!node->name) {
			kfree(node);
			goto error;
		}
		node->dev = device_create(cls->mca_class, NULL, 0, NULL, "%s", node->name);
		if (IS_ERR(node->dev)) {
			ret = PTR_ERR(node->dev);
			kfree(node->name);
			kfree(node);
			goto error;
		}
		list_add_tail(&node->node, &cls->dev_list_header);
	}
	mutex_unlock(&g_mca_sysfs_class_lock);
	return 0;
error:
	mutex_unlock(&g_mca_sysfs_class_lock);
	mca_sysfs_cleanup();
	return ret;
}

static struct dentry *g_mca_dbg_dir;
static DEFINE_MUTEX(g_mca_debugfs_dir_lock);

#ifdef CONFIG_DEBUG_FS
static int mca_debugfs_template_show(struct seq_file *s, void *d)
{
	struct mca_debugfs_attr_data *pattr = s->private;
	char *buf = NULL;
	int ret;

	if (!pattr || !pattr->attr_info->show) {
		pr_err("invalid show\n");
		return -EINVAL;
	}

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = pattr->attr_info->show(pattr, buf);
	if (ret >= 0 && ret <= PAGE_SIZE)
		seq_write(s, buf, ret);
	else if (ret > PAGE_SIZE)
		ret = -EOVERFLOW;
	kfree(buf);

	return ret < 0 ? ret : 0;
}

static ssize_t mca_debugfs_template_write(struct file *file,
					  const char __user *data, size_t size,
					  loff_t *ppos)
{
	struct mca_debugfs_attr_data *pattr = NULL;
	char *buf = NULL;
	int ret;

	pattr = ((struct seq_file *)file->private_data)->private;
	if (!pattr || !pattr->attr_info->store) {
		pr_err("invalid store\n");
		return -EINVAL;
	}

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (size >= PAGE_SIZE) {
		pr_err("input too long\n");
		kfree(buf);
		return -E2BIG;
	}

	if (copy_from_user(buf, data, size)) {
		pr_err("can not copy data form user to kernel\n");
		kfree(buf);
		return -EFAULT;
	}
	buf[size] = '\0';

	ret = pattr->attr_info->store(pattr, buf, size);
	kfree(buf);

	pr_err("store: %s ret=%d\n", pattr->attr_info->file, ret);
	return ret;
}

static int mca_debugfs_template_open(struct inode *inode, struct file *file)
{
	return single_open(file, mca_debugfs_template_show, inode->i_private);
}

static const struct file_operations mca_debugfs_template_fops = {
	.owner = THIS_MODULE,
	.llseek = seq_lseek,
	.open = mca_debugfs_template_open,
	.read = seq_read,
	.write = mca_debugfs_template_write,
	.release = single_release,
};

struct mca_debugfs_group {
	struct list_head node;
	char *name;
	struct dentry **files;
	struct mca_debugfs_attr_data *attrs;
	int count;
};

static LIST_HEAD(mca_debugfs_groups);

static void mca_debugfs_free_group(struct mca_debugfs_group *group)
{
	int i;

	/* debugfs_remove waits for active operations on each protected file. */
	for (i = 0; i < group->count; i++)
		debugfs_remove(group->files[i]);
	kfree(group->attrs);
	kfree(group->files);
	kfree(group->name);
	kfree(group);
}

int mca_debugfs_create_group(const char *dir_name,
			     struct mca_debugfs_attr_info *attr_info,
			     const int attr_size, void *dev_data)
{
	struct mca_debugfs_group *group;
	struct dentry *dir, *file;
	int i, ret = -ENOMEM;
	bool lookup_ref;

	if (!dir_name || !attr_info || attr_size <= 0)
		return -EINVAL;
	if (IS_ERR_OR_NULL(g_mca_dbg_dir))
		return -ENODEV;
	group = kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	group->name = kstrdup(dir_name, GFP_KERNEL);
	group->attrs = kcalloc(attr_size, sizeof(*group->attrs), GFP_KERNEL);
	group->files = kcalloc(attr_size, sizeof(*group->files), GFP_KERNEL);
	if (!group->name || !group->attrs || !group->files)
		goto free_group;

	mutex_lock(&g_mca_debugfs_dir_lock);
	dir = debugfs_lookup(dir_name, g_mca_dbg_dir);
	lookup_ref = !IS_ERR_OR_NULL(dir);
	if (!lookup_ref)
		dir = debugfs_create_dir(dir_name, g_mca_dbg_dir);
	if (IS_ERR_OR_NULL(dir)) {
		ret = dir ? PTR_ERR(dir) : -ENOMEM;
		goto unlock;
	}
	for (i = 0; i < attr_size; i++) {
		group->attrs[i].attr_info = &attr_info[i];
		group->attrs[i].private = dev_data;
		file = debugfs_create_file(attr_info[i].file, attr_info[i].mode,
					   dir, &group->attrs[i], &mca_debugfs_template_fops);
		if (IS_ERR_OR_NULL(file)) {
			ret = file ? PTR_ERR(file) : -ENOMEM;
			if (lookup_ref)
				dput(dir);
			goto unlock;
		}
		group->files[group->count++] = file;
	}
	if (lookup_ref)
		dput(dir);
	list_add_tail(&group->node, &mca_debugfs_groups);
	mutex_unlock(&g_mca_debugfs_dir_lock);
	return 0;
unlock:
	mutex_unlock(&g_mca_debugfs_dir_lock);
free_group:
	mca_debugfs_free_group(group);
	return ret;
}
EXPORT_SYMBOL(mca_debugfs_create_group);

void mca_debugfs_remove_group(const char *dir_name)
{
	struct mca_debugfs_group *group, *tmp;
	LIST_HEAD(removed);

	if (!dir_name)
		return;
	mutex_lock(&g_mca_debugfs_dir_lock);
	list_for_each_entry_safe(group, tmp, &mca_debugfs_groups, node) {
		if (!strcmp(group->name, dir_name))
			list_move_tail(&group->node, &removed);
	}
	mutex_unlock(&g_mca_debugfs_dir_lock);
	list_for_each_entry_safe(group, tmp, &removed, node) {
		list_del(&group->node);
		mca_debugfs_free_group(group);
	}
}
EXPORT_SYMBOL(mca_debugfs_remove_group);

static void mca_debugfs_cleanup(void)
{
	struct mca_debugfs_group *group, *tmp;

	list_for_each_entry_safe(group, tmp, &mca_debugfs_groups, node) {
		list_del(&group->node);
		mca_debugfs_free_group(group);
	}
	debugfs_remove_recursive(g_mca_dbg_dir);
}

static int mca_debugfs_init(void)
{
	int rc = 0;

	g_mca_dbg_dir = debugfs_create_dir(MCA_DBG_ROOT_DIR, NULL);
	if (IS_ERR(g_mca_dbg_dir)) {
		rc = PTR_ERR(g_mca_dbg_dir);
		pr_err("Failed to create charger debugfs root directory: %d\n",
		       rc);
	}
	return rc;
}
#else
int mca_debugfs_create_group(const char *dir_name,
			     struct mca_debugfs_attr_info *attr_info,
			     const int attr_size, void *dev_data)
{
	return 0;
}
EXPORT_SYMBOL(mca_debugfs_create_group);

void mca_debugfs_remove_group(const char *dir_name)
{
}
EXPORT_SYMBOL(mca_debugfs_remove_group);

static void mca_debugfs_cleanup(void)
{
}

static int mca_debugfs_init(void)
{
	return 0;
}
#endif

static int __init mca_fs_init(void)
{
	int ret;

	ret = mca_sysfs_init();
	if (ret)
		return ret;
	mca_debugfs_init();
	return 0;
}

static void __exit mca_fs_exit(void)
{
	mca_debugfs_cleanup();
	mca_sysfs_cleanup();
}

module_init(mca_fs_init);
module_exit(mca_fs_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("sysfs for xiaomi power driver");
MODULE_AUTHOR("liyuze1@xiaomi.com");
