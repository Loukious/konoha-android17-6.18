// SPDX-License-Identifier: GPL-2.0
/*
 * mca_pd_auth.c
 *
 * pd adapter private auth driver
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
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/common/mca_log.h>
#include <mca/common/mca_sysfs.h>
#include <mca/common/mca_event.h>
#include <mca/common/mca_charge_mievent.h>
#include <mca/platform/platform_buckchg_class.h>
#include "pd_auth.h"
#include <linux/power_supply.h>
#include <mca/common/mca_charge_interface.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "pd_auth"
#endif

#ifdef CONFIG_SYSFS
static ssize_t strategy_pd_auth_sysfs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf);
static ssize_t strategy_pd_auth_sysfs_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count);

static struct mca_sysfs_attr_info strategy_pd_auth_sysfs_field_tbl[] = {
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_NAME, name),
	mca_sysfs_attr_rw(strategy_pd_auth_sysfs, 0664, PD_AUTH_REQUEST_VDM_CMD,
			  request_vdm_cmd),
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_CURRENT_STATE,
			  current_state),
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_ADAPTER_ID,
			  adapter_id),
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_ADAPTER_SVID,
			  adapter_svid),
	mca_sysfs_attr_rw(strategy_pd_auth_sysfs, 0664, PD_AUTH_VERIFY_PROCESS,
			  verify_process),
	mca_sysfs_attr_rw(strategy_pd_auth_sysfs, 0664, PD_AUTH_USBPD_VERIFIED,
			  verified),
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_CURRENT_PR,
			  current_pr),
	mca_sysfs_attr_ro(strategy_pd_auth_sysfs, 0440, PD_AUTH_IS_PD_ADAPTER,
			  is_pd_adapter),
	mca_sysfs_attr_rw(strategy_pd_auth_sysfs, 0664, PD_AUTH_USBPD_DATA_ROLE,
			  data_role),
};

#define PD_AUTH_ATTRS_SIZE ARRAY_SIZE(strategy_pd_auth_sysfs_field_tbl)

static struct attribute *strategy_pd_auth_sysfs_attrs[PD_AUTH_ATTRS_SIZE + 1];

static const struct attribute_group strategy_pd_auth_sysfs_attr_group = {
	.attrs = strategy_pd_auth_sysfs_attrs,
};
static int strategy_pd_auth_get_vdm_cmd(char *buf, int active_port)
{
	struct usbpd_vdm_data data = {};
	int cmd, ret, i, len;

	ret = protocol_class_pd_get_vdm_cmd(active_port, &cmd, &data);
	if (ret)
		return ret;
	switch (cmd) {
	case USBPD_UVDM_CHARGER_VERSION:
		return sysfs_emit(buf, "%d,%x\n", cmd, data.ta_version);
	case USBPD_UVDM_CHARGER_TEMP:
		return sysfs_emit(buf, "%d,%d\n", cmd, data.ta_temp);
	case USBPD_UVDM_CHARGER_VOLTAGE:
		return sysfs_emit(buf, "%d,%d\n", cmd, data.ta_voltage);
	case USBPD_UVDM_REVERSE_AUTHEN:
		return sysfs_emit(buf, "%d,%d\n", cmd, data.reauth);
	case USBPD_UVDM_AUTHENTICATION:
		len = sysfs_emit(buf, "%d,", cmd);
		for (i = 0; i < USBPD_UVDM_AUTH_WORDS; i++) {
			if (data.s_secert[USBPD_UVDM_AUTH_FIRST + i] > U32_MAX)
				return -ERANGE;
			len += sysfs_emit_at(
				buf, len, "%08x",
				(u32)data.s_secert[USBPD_UVDM_AUTH_FIRST + i]);
		}
		return len + sysfs_emit_at(buf, len, "\n");
	case USBPD_UVDM_SESSION_SEED:
	case USBPD_UVDM_CONNECT:
	case USBPD_UVDM_DISCONNECT:
	case USBPD_UVDM_VERIFIED:
	case USBPD_UVDM_REMOVE_COMPENSATION:
	case USBPD_UVDM_NAN_ACK:
		return sysfs_emit(buf, "%d,Null\n", cmd);
	default:
		if ((cmd >= USBPD_UVDM_CMD_INIT &&
		     cmd <= USBPD_UVDM_CMD_INIT + USBPD_UVDM_CONNECT) ||
		    (cmd >= USBPD_UVDM_CMD_NAK &&
		     cmd <= USBPD_UVDM_CMD_NAK + USBPD_UVDM_CONNECT))
			return sysfs_emit(buf, "%d,Null\n", cmd);
		return -EOPNOTSUPP;
	}
}
static int strategy_pd_auth_is_pd_adapter(char *buf, int active_port)
{
	struct pd_pdo pdos[PROTOCOL_PD_MAX_PDO_NUMS] = {};
	int ret =
		protocol_class_pd_get_pdos(active_port, pdos, ARRAY_SIZE(pdos));

	if (ret)
		return ret;
	return sysfs_emit(buf, "%s\n", pdos[1].max_volt ? "true" : "false");
}
static ssize_t strategy_pd_auth_sysfs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct pd_auth_strategy *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *field;
	unsigned int value = 0;
	unsigned char role = 0;
	int ret, port, flag = 0;
	char state[PROTOCOL_PD_MAX_STRING_LEN] = {};

	if (!info)
		return -ENODEV;
	field = mca_sysfs_lookup_attr(attr->attr.name,
				      strategy_pd_auth_sysfs_field_tbl,
				      PD_AUTH_ATTRS_SIZE);
	if (!field)
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	if (field->sysfs_attr_name == PD_AUTH_NAME)
		return sysfs_emit(buf, "pd_auth\n");
	port = protocol_class_pd_get_port_num();
	if (port < TYPEC_PORT_0 || port >= TYPEC_PORT_MAX)
		return -ENODEV;
	switch (field->sysfs_attr_name) {
	case PD_AUTH_REQUEST_VDM_CMD:
		return strategy_pd_auth_get_vdm_cmd(buf, port);
	case PD_AUTH_CURRENT_STATE:
		ret = protocol_class_pd_get_current_state(port, state,
							  sizeof(state));
		if (ret)
			return ret;
		if (strnlen(state, sizeof(state)) == sizeof(state))
			return -EOVERFLOW;
		return sysfs_emit(buf, "%s\n", state);
	case PD_AUTH_ADAPTER_ID:
		ret = protocol_class_pd_get_adapter_id(port, &value);
		return ret ? ret : sysfs_emit(buf, "%08x\n", value);
	case PD_AUTH_ADAPTER_SVID:
		ret = protocol_class_pd_get_adapter_svid(port, &value);
		return ret ? ret : sysfs_emit(buf, "%04x\n", value);
	case PD_AUTH_VERIFY_PROCESS:
		ret = protocol_class_pd_get_verify_process(port, &flag);
		break;
	case PD_AUTH_USBPD_VERIFIED:
		ret = protocol_class_pd_get_pd_verifed(port, &flag);
		break;
	case PD_AUTH_CURRENT_PR:
		ret = protocol_class_pd_get_power_role(port, &role);
		if (ret)
			return ret;
		return sysfs_emit(buf, "%s\n",
				  role == PD_ROLE_SINK_FOR_ADAPTER ? "sink" :
				  role == PD_ROLE_SOURCE_FOR_ADAPTER ?
								     "source" :
								     "none");
	case PD_AUTH_IS_PD_ADAPTER:
		return strategy_pd_auth_is_pd_adapter(buf, port);
	case PD_AUTH_USBPD_DATA_ROLE:
		ret = protocol_class_pd_get_data_role(port, &role);
		if (ret)
			return ret;
		return sysfs_emit(buf, "%s\n",
				  role == XM_REQUEST_PD_DR_UFP ? "ufp" :
				  role == XM_REQUEST_PD_DR_DFP ? "dfp" :
								 "unknown");
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;
	if (flag != 0 && flag != 1)
		return -ERANGE;
	return sysfs_emit(buf, "%d\n", flag);
}
static int strategy_pd_auth_string2hex(const char *str, unsigned char *out,
				       unsigned int *outlen)
{
	size_t len;
	int ret;

	if (!str || !out || !outlen)
		return -EINVAL;
	len = strnlen(str, PD_AUTH_VDM_CMD_HEX_DATA_LEN * 2 + 1);
	if (!len || len > PD_AUTH_VDM_CMD_HEX_DATA_LEN * 2 || len % 2)
		return -EINVAL;
	ret = hex2bin(out, str, len / 2);
	if (ret)
		return ret;
	*outlen = len / 2;
	return 0;
}
static int strategy_pd_auth_set_vdm_cmd(const char *buf, size_t size,
					int active_port)
{
	unsigned int payload[PD_AUTH_VDM_CMD_HEX_DATA_LEN /
			     sizeof(unsigned int)] = {};
	unsigned int count = 0;
	char *copy, *command, *hex;
	int cmd, ret;

	if (!size || size > PD_AUTH_VDM_CMD_HEX_DATA_LEN * 2 + 12 ||
	    memchr(buf, 0, size))
		return -EINVAL;
	copy = kmemdup_nul(buf, size, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	hex = copy;
	command = strsep(&hex, ",");
	if (!hex || kstrtoint(command, 10, &cmd) || cmd < 0 || cmd > 255) {
		ret = -EINVAL;
		goto out;
	}
	ret = strategy_pd_auth_string2hex(strim(hex), (unsigned char *)payload,
					  &count);
	if (ret)
		goto out;
	if ((cmd == USBPD_UVDM_SESSION_SEED ||
	     cmd == USBPD_UVDM_AUTHENTICATION ||
	     cmd == USBPD_UVDM_REVERSE_AUTHEN) &&
	    count != PD_AUTH_UVDM_AUTH_DATA_LEN) {
		ret = -EINVAL;
		goto out;
	}
	ret = protocol_class_pd_request_vdm_cmd(active_port, cmd, payload,
						count);
out:
	memzero_explicit(payload, sizeof(payload));
	kfree_sensitive(copy);
	return ret;
}

#define XM_ADAPTER_SVID 0x2717
static void strategy_pd_auth_fail_report_dfx(int port)
{
	unsigned int id, svid;
	int online;

	if (platform_class_buckchg_ops_get_online(MAIN_BUCK_CHARGER, &online) ||
	    !online || protocol_class_pd_get_adapter_svid(port, &svid) ||
	    svid != XM_ADAPTER_SVID ||
	    protocol_class_pd_get_adapter_id(port, &id))
		return;
	mca_charge_mievent_report(CHARGE_DFX_PD_AUTH_FAILED, &id, 1);
}
static ssize_t strategy_pd_auth_sysfs_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct pd_auth_strategy *info = dev_get_drvdata(dev);
	struct mca_sysfs_attr_info *field;
	unsigned int role;
	int value, ret, port;

	if (!info)
		return -ENODEV;
	field = mca_sysfs_lookup_attr(attr->attr.name,
				      strategy_pd_auth_sysfs_field_tbl,
				      PD_AUTH_ATTRS_SIZE);
	if (!field)
		return -EINVAL;
	guard(mutex)(&info->lock);
	if (info->stopping)
		return -ENODEV;
	port = protocol_class_pd_get_port_num();
	if (port < TYPEC_PORT_0 || port >= TYPEC_PORT_MAX)
		return -ENODEV;
	if (field->sysfs_attr_name == PD_AUTH_REQUEST_VDM_CMD) {
		ret = strategy_pd_auth_set_vdm_cmd(buf, count, port);
		return ret ? ret : count;
	}
	if (field->sysfs_attr_name == PD_AUTH_USBPD_DATA_ROLE) {
		if (sysfs_streq(buf, "ufp"))
			role = XM_REQUEST_PD_DR_UFP;
		else if (sysfs_streq(buf, "dfp"))
			role = XM_REQUEST_PD_DR_DFP;
		else
			return -EINVAL;
		ret = protocol_class_pd_request_vdm_cmd(
			port, USBPD_UVDM_REQUEST_PD_DR, &role, sizeof(role));
		return ret ? ret : count;
	}
	if (kstrtoint(buf, 10, &value) || (value != 0 && value != 1))
		return -EINVAL;
	switch (field->sysfs_attr_name) {
	case PD_AUTH_VERIFY_PROCESS:
		ret = protocol_class_pd_set_verify_process(port, value);
		if (ret)
			return ret;
		info->verify_porcess_end = value;
		if (!value)
			mca_event_block_notify(
				MCA_EVENT_TYPE_CHARGE_TYPE,
				MCA_EVENT_CHARGE_VERIFY_PROCESS_END,
				&info->verify_porcess_end);
		break;
	case PD_AUTH_USBPD_VERIFIED:
		ret = protocol_class_pd_set_pd_verifed(port, value);
		if (ret)
			return ret;
		info->pd_verified_type = value ? XM_CHARGER_TYPE_PD_VERIFY :
						 XM_CHARGER_TYPE_UNKNOW;
		mca_event_block_notify(MCA_EVENT_TYPE_CHARGE_TYPE,
				       MCA_EVENT_CHARGE_TYPE_CHANGE,
				       &info->pd_verified_type);
		if (!value)
			strategy_pd_auth_fail_report_dfx(port);
		break;
	default:
		return -EINVAL;
	}
	return count;
}

static int strategy_pd_auth_create_group(struct device *dev)
{
	mca_sysfs_init_attrs(strategy_pd_auth_sysfs_attrs,
			     strategy_pd_auth_sysfs_field_tbl,
			     PD_AUTH_ATTRS_SIZE);
	return mca_sysfs_create_link_group("typec", "strategy_pd_auth", dev,
					   &strategy_pd_auth_sysfs_attr_group);
}

static void strategy_pd_auth_remove_group(struct device *dev)
{
	mca_sysfs_remove_link_group("typec", "strategy_pd_auth", dev,
				    &strategy_pd_auth_sysfs_attr_group);
}

#else
static inline int strategy_pd_auth_create_group(struct device *dev)
{
	return 0;
}

static void strategy_pd_auth_remove_group(struct device *dev)
{
}
#endif /* CONFIG_SYSFS */
static int strategy_pd_auth_probe(struct platform_device *pdev)
{
	struct pd_auth_strategy *info;
	int ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	mutex_init(&info->lock);
	info->verify_porcess_end = 1;
	platform_set_drvdata(pdev, info);
	ret = strategy_pd_auth_create_group(info->dev);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		return ret;
	}
	info->group_registered = true;
	return 0;
}
static void strategy_pd_auth_stop(struct pd_auth_strategy *info)
{
	if (!info)
		return;
	mutex_lock(&info->lock);
	if (info->stopping) {
		mutex_unlock(&info->lock);
		return;
	}
	info->stopping = true;
	mutex_unlock(&info->lock);
	if (info->group_registered)
		strategy_pd_auth_remove_group(info->dev);
}

static void strategy_pd_auth_remove(struct platform_device *pdev)
{
	strategy_pd_auth_stop(platform_get_drvdata(pdev));
	platform_set_drvdata(pdev, NULL);
}
static void strategy_pd_auth_shutdown(struct platform_device *pdev)
{
	strategy_pd_auth_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,strategy_pd_auth" },
	{},
};

MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver strategy_pd_auth_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "strategy_pd_auth",
		.of_match_table = match_table,
	},
	.probe = strategy_pd_auth_probe,
	.remove = strategy_pd_auth_remove,
	.shutdown = strategy_pd_auth_shutdown,
};

static int __init strategy_pd_auth_init(void)
{
	return platform_driver_register(&strategy_pd_auth_driver);
}
module_init(strategy_pd_auth_init);

static void __exit strategy_pd_auth_exit(void)
{
	platform_driver_unregister(&strategy_pd_auth_driver);
}
module_exit(strategy_pd_auth_exit);

MODULE_DESCRIPTION("Xiaomi Sub Pmic Protocol");
MODULE_AUTHOR("yinshunan@xiaomi.com");
MODULE_LICENSE("GPL v2");
