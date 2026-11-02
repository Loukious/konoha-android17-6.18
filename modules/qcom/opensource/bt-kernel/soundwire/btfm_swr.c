// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */


#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/debugfs.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include "btpower.h"
#include "btfm_swr.h"
#include "btfm_swr_hw_interface.h"
#include "btfm_swr_slave.h"

struct class *btfm_swr_class;
static int btfm_swr_major;
struct btfmswr *pbtfmswr;
static int btfm_num_ports_open;
static DEFINE_MUTEX(btfm_swr_lock);

#define BT_CMD_SWR_TEST	0xbfac
#define SWRS_SCP_STATUS 0x00000044
#define SWRS_SCP_CONTROL 0x00000044
#define DELAY_FOR_SWR_SLAVE_RESET 20
#define DELAY_BEFORE_SLAVE_RESET 10

static int btfm_swr_probe(struct swr_device *pdev);

int btfm_get_bt_soc_index(int chipset_ver)
{
	switch (chipset_ver) {
	case QCA_GANGES_SOC_ID_0100:
	case QCA_GANGES_SOC_ID_0200:
	case QCA_ORNE_SOC_ID_0100:
	case QCA_COLOGNE_SOC_ID_0100:
		return GANGES;
	case QCA_EVROS_SOC_ID_0100:
	case QCA_EVROS_SOC_ID_0101:
	case QCA_EVROS_SOC_ID_0102:
	case QCA_EVROS_SOC_ID_0103:
	case QCA_EVROS_SOC_ID_0104:
	case QCA_EVROS_SOC_ID_0105:
	case QCA_EVROS_SOC_ID_0200:
		return EVROS;
	default:
		BTFMSWR_ERR("no BT SOC id defined, returning GANGES");
		return GANGES;
	}
}

static int btfm_swr_hw_init_locked(void)
{
	uint8_t dev_num = 0;
	int ret = 0;
	int chipset_ver;
	uint8_t retry = 0;
	uint8_t reg_val = 0;

	BTFMSWR_DBG("");

	if (pbtfmswr->initialized)
		BTFMSWR_INFO("Already initialized");

	// get BT chipset version
	chipset_ver = btpower_get_chipset_version();

	// get BT/FM SOC slave port details
	pbtfmswr->soc_index = btfm_get_bt_soc_index(chipset_ver);

	BTFMSWR_INFO("chipset soc version:%x, soc index: %x", chipset_ver,
							pbtfmswr->soc_index);

	pbtfmswr->p_dai_port = &slave_port[pbtfmswr->soc_index];

	// get logical address
	/*
	 * Add delay to provide sufficient time for
	 * soundwire auto enumeration of slave devices as
	 * per HW requirement.
	 */
	for ( ; retry < MAX_GET_DEV_NUM_RETRY; retry++) {
		ret = swr_get_logical_dev_num(pbtfmswr->swr_slave,
						pbtfmswr->p_dai_port->ea,
						&dev_num);
		if (ret == 0)
			break;
		usleep_range(2000, 2100);
	}
	if (ret) {
		BTFMSWR_ERR("error getting logical device num after retry %u",
				retry);
		goto err;
	}

        // for first port open, reset SWR slave. This will trigger enumeration again.
	if (btfm_num_ports_open == 0) {
		BTFMSWR_INFO("Doing %d msec sleep before slave reset",
				DELAY_BEFORE_SLAVE_RESET);
		msleep(DELAY_BEFORE_SLAVE_RESET);
		BTFMSWR_INFO("Doing slave reset");
		reg_val = 0x80;
		ret = swr_write(pbtfmswr->swr_slave, dev_num,
				SWRS_SCP_CONTROL, &reg_val);
		if (ret)
			return ret;
		msleep(DELAY_FOR_SWR_SLAVE_RESET);
		BTFMSWR_INFO("Done writing %x to SWR slave register", reg_val);

		// get logical address again
		for (retry = 0; retry < MAX_GET_DEV_NUM_RETRY; retry++) {
		ret = swr_get_logical_dev_num(pbtfmswr->swr_slave,
						pbtfmswr->p_dai_port->ea,
							&dev_num);
			if (ret == 0)
				break;
			usleep_range(2000, 2100);
		}
		if (ret) {
			BTFMSWR_ERR("error getting logical dev num after slave reset retry %u",
					retry);
			goto err;
		}
	}

	pbtfmswr->swr_slave->dev_num = dev_num;
	pbtfmswr->initialized = true;

err:
	return ret;
}

int btfm_swr_hw_init(void)
{
	int ret;

	mutex_lock(&btfm_swr_lock);
	if (!pbtfmswr || pbtfmswr->stopping)
		ret = -ENODEV;
	else
		ret = btfm_swr_hw_init_locked();
	mutex_unlock(&btfm_swr_lock);
	return ret;
}

static int btfm_swr_enable_port_locked(u8 port_num, u8 ch_count,
				       u32 sample_rate, u8 usecase)
{
	u8 port_id = port_num - 1;
	u8 ch_mask = ch_count == 2 ? TWO_CHANNEL_MASK : ONE_CHANNEL_MASK;
	int ret;

	if (!port_num || port_num >= ARRAY_SIZE(pbtfmswr->ports) ||
	    !ch_count || ch_count > 2 || !sample_rate || !pbtfmswr->initialized)
		return -EINVAL;
	if (pbtfmswr->ports[port_num].disconnect_pending)
		return -EBUSY;
	if (pbtfmswr->ports[port_num].connected) {
		if (pbtfmswr->ports[port_num].channels != ch_count ||
		    pbtfmswr->ports[port_num].usecase != usecase ||
		    pbtfmswr->ports[port_num].sample_rate != sample_rate)
			return -EINVAL;
		if (pbtfmswr->ports[port_num].active)
			return -EISCONN;
	} else {
		ret = swr_connect_port(pbtfmswr->swr_slave, &port_id, 1,
				       &ch_mask, &sample_rate, &ch_count, &usecase);
		if (ret)
			return ret;
		pbtfmswr->ports[port_num].connected = true;
		pbtfmswr->ports[port_num].channels = ch_count;
		pbtfmswr->ports[port_num].usecase = usecase;
		pbtfmswr->ports[port_num].sample_rate = sample_rate;
		btfm_num_ports_open++;
	}
	ret = swr_slvdev_datapath_control(pbtfmswr->swr_slave,
					  pbtfmswr->swr_slave->dev_num, true);
	if (ret) {
		if (!swr_disconnect_port(pbtfmswr->swr_slave, &port_id, 1,
					 &ch_mask, &usecase)) {
			pbtfmswr->ports[port_num].connected = false;
			btfm_num_ports_open--;
		}
		return ret;
	}
	pbtfmswr->ports[port_num].active = true;
	return 0;
}

int btfm_swr_enable_port(u8 port_num, u8 ch_count, u32 sample_rate, u8 usecase)
{
	int ret;

	mutex_lock(&btfm_swr_lock);
	if (!pbtfmswr || pbtfmswr->stopping)
		ret = -ENODEV;
	else
		ret = btfm_swr_enable_port_locked(port_num, ch_count,
						  sample_rate, usecase);
	mutex_unlock(&btfm_swr_lock);
	return ret;
}

static int btfm_swr_disable_port_locked(u8 port_num, u8 ch_count, u8 usecase)
{
	u8 port_id = port_num - 1;
	u8 ch_mask = ch_count == 2 ? TWO_CHANNEL_MASK : ONE_CHANNEL_MASK;
	int ret;

	if (!port_num || port_num >= ARRAY_SIZE(pbtfmswr->ports) ||
	    !ch_count || ch_count > 2 ||
	    pbtfmswr->ports[port_num].channels != ch_count ||
	    pbtfmswr->ports[port_num].usecase != usecase)
		return -EINVAL;
	if (!pbtfmswr->ports[port_num].disconnect_pending) {
		if (!pbtfmswr->ports[port_num].connected)
			return -ENOENT;
		ret = swr_disconnect_port(pbtfmswr->swr_slave, &port_id, 1,
					  &ch_mask, &usecase);
		if (ret)
			return ret;
		pbtfmswr->ports[port_num].connected = false;
		pbtfmswr->ports[port_num].disconnect_pending = true;
	}
	ret = swr_slvdev_datapath_control(pbtfmswr->swr_slave,
					  pbtfmswr->swr_slave->dev_num, false);
	if (ret)
		return ret;
	pbtfmswr->ports[port_num].disconnect_pending = false;
	pbtfmswr->ports[port_num].active = false;
	btfm_num_ports_open--;
	return 0;
}

int btfm_swr_disable_port(u8 port_num, u8 ch_count, u8 usecase)
{
	int ret;

	mutex_lock(&btfm_swr_lock);
	if (!pbtfmswr || pbtfmswr->stopping)
		ret = -ENODEV;
	else
		ret = btfm_swr_disable_port_locked(port_num, ch_count, usecase);
	mutex_unlock(&btfm_swr_lock);
	return ret;
}

static void btfm_swr_free(struct kref *ref)
{
	struct btfmswr *priv = container_of(ref, struct btfmswr, ref);

	kfree(priv);
}

static int btfm_swr_open(struct inode *inode, struct file *file)
{
	int ret = -ENODEV;

	mutex_lock(&btfm_swr_lock);
	if (pbtfmswr && !pbtfmswr->stopping) {
		kref_get(&pbtfmswr->ref);
		file->private_data = pbtfmswr;
		ret = 0;
	}
	mutex_unlock(&btfm_swr_lock);
	return ret;
}

static int btfm_swr_release(struct inode *inode, struct file *file)
{
	struct btfmswr *priv = file->private_data;

	kref_put(&priv->ref, btfm_swr_free);
	return 0;
}

static long btfm_swr_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct btfmswr *priv = file->private_data;
	int ret = -ENOTTY;

	mutex_lock(&btfm_swr_lock);
	if (priv != pbtfmswr || priv->stopping)
		ret = -ENODEV;
	else if (cmd == BT_CMD_SWR_TEST)
		ret = btfm_swr_hw_init_locked();
	mutex_unlock(&btfm_swr_lock);
	return ret;
}

static const struct file_operations bt_dev_fops = {
	.owner = THIS_MODULE,
	.open = btfm_swr_open,
	.release = btfm_swr_release,
	.unlocked_ioctl = btfm_swr_ioctl,
	.compat_ioctl = btfm_swr_ioctl,
};

static int btfm_swr_probe(struct swr_device *pdev)
{
	struct btfmswr *priv;
	struct device *chardev;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	kref_init(&priv->ref);
	priv->swr_slave = pdev;
	priv->dev = &pdev->dev;
	priv->stopping = true;
	mutex_lock(&btfm_swr_lock);
	if (pbtfmswr) {
		mutex_unlock(&btfm_swr_lock);
		kref_put(&priv->ref, btfm_swr_free);
		return -EBUSY;
	}
	pbtfmswr = priv;
	btfm_num_ports_open = 0;
	swr_set_dev_data(pdev, priv);
	mutex_unlock(&btfm_swr_lock);

	btfm_swr_major = register_chrdev(0, "btfm_swr", &bt_dev_fops);
	if (btfm_swr_major < 0) {
		ret = btfm_swr_major;
		goto clear_data;
	}
	btfm_swr_class = class_create("btfmswr-dev");
	if (IS_ERR(btfm_swr_class)) {
		ret = PTR_ERR(btfm_swr_class);
		goto unregister_chrdev;
	}
	chardev = device_create(btfm_swr_class, NULL,
				MKDEV(btfm_swr_major, 0), NULL, "btfmswr");
	if (IS_ERR(chardev)) {
		ret = PTR_ERR(chardev);
		goto destroy_class;
	}
	/* Endpoint registration is final; callbacks cannot race later failure. */
	ret = btfm_swr_register_hw_ep(priv);
	if (ret)
		goto destroy_device;
	mutex_lock(&btfm_swr_lock);
	priv->stopping = false;
	mutex_unlock(&btfm_swr_lock);
	return 0;

destroy_device:
	device_destroy(btfm_swr_class, MKDEV(btfm_swr_major, 0));
destroy_class:
	class_destroy(btfm_swr_class);
unregister_chrdev:
	unregister_chrdev(btfm_swr_major, "btfm_swr");
clear_data:
	mutex_lock(&btfm_swr_lock);
	pbtfmswr = NULL;
	swr_set_dev_data(pdev, NULL);
	mutex_unlock(&btfm_swr_lock);
	kref_put(&priv->ref, btfm_swr_free);
	return ret;
}

static int btfm_swr_remove(struct swr_device *pdev)
{
	struct btfmswr *priv = swr_get_dev_data(pdev);
	int port, ret;

	mutex_lock(&btfm_swr_lock);
	priv->stopping = true;
	mutex_unlock(&btfm_swr_lock);
	device_destroy(btfm_swr_class, MKDEV(btfm_swr_major, 0));
	unregister_chrdev(btfm_swr_major, "btfm_swr");
	/* Never hold the hardware lock while the codec drains callbacks. */
	btfm_swr_unregister_hwep();
	mutex_lock(&btfm_swr_lock);
	for (port = 1; port < ARRAY_SIZE(priv->ports); port++) {
		if (!priv->ports[port].connected &&
		    !priv->ports[port].disconnect_pending)
			continue;
		ret = btfm_swr_disable_port_locked(port,
						   priv->ports[port].channels,
						   priv->ports[port].usecase);
		if (ret)
			BTFMSWR_ERR("port %d teardown failed: %d", port, ret);
	}
	class_destroy(btfm_swr_class);
	pbtfmswr = NULL;
	btfm_num_ports_open = 0;
	swr_set_dev_data(pdev, NULL);
	mutex_unlock(&btfm_swr_lock);
	kref_put(&priv->ref, btfm_swr_free);
	return 0;
}

static const struct swr_device_id btfm_swr_id[] = {
	{SWR_SLAVE_COMPATIBLE_STR, 0},
	{}
};

static const struct of_device_id btfm_swr_dt_match[] = {
	{
		.compatible = "qcom,btfmswr_slave",
	},
	{}
};

static struct swr_driver btfm_swr_driver = {
	.driver = {
		.name = "btfmswr-driver",
		.suppress_bind_attrs = true,
		.owner = THIS_MODULE,
		.of_match_table = btfm_swr_dt_match,
	},
	.probe = btfm_swr_probe,
	.remove = btfm_swr_remove,
	.id_table = btfm_swr_id,
};

static int __init btfm_swr_init(void)
{
	BTFMSWR_INFO("");
	return swr_driver_register(&btfm_swr_driver);
}

static void __exit btfm_swr_exit(void)
{
	BTFMSWR_INFO("");
	swr_driver_unregister(&btfm_swr_driver);
}

module_init(btfm_swr_init);
module_exit(btfm_swr_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("BTFM SoundWire Slave driver");

