#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/err.h>
#include <mca/common/mca_log.h>
#include <mca/protocol/protocol_class.h>
#include <mca/protocol/protocol_pd_class.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/platform/platform_buckchg_class.h>
#include <mca/platform/platform_cp_class.h>
#include <mca/platform/platform_fg_ic_ops.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_qcom_sysfs"
#endif

struct mca_qcom_sysfs_dev {
	struct device *dev;
	struct class *class;
	bool support_multi_typec;
};

/* Indexed by the adapter type returned from protocol_class_get_adapter_type(). */
static const char *const power_supply_usb_type_text[] = {
	"Unknown", "SDP",	"CDP",	     "DCP", "USB_FLOAT", "HVDCP",
	"HVDCP_3", "HVDCP_3_B", "HVDCP_3P5", "C",   "PD",	 "PD_PPS",
	"PD_PPS",  "Unknown",	"ACA",	     "DCP",
};

static ssize_t real_type_show(const struct class *class,
			      const struct class_attribute *attr, char *buf)
{
	unsigned int type = 0;
	const char *s;
	int ret;

	ret = protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12, &type);
	if (ret < 0)
		return ret;
	s = (type < ARRAY_SIZE(power_supply_usb_type_text)) ?
		    power_supply_usb_type_text[type] :
		    "Unknown";
	mca_log_info("real type = %s\n", s);
	return sysfs_emit(buf, "%s\n", s);
}
static CLASS_ATTR_RO(real_type);

static ssize_t usb_real_type_show(const struct class *class,
				  const struct class_attribute *attr, char *buf)
{
	unsigned int type = 0;
	const char *s;
	int ret;

	ret = protocol_class_get_adapter_type(ADAPTER_PROTOCOL_BC12, &type);
	if (ret < 0)
		return ret;
	s = (type < ARRAY_SIZE(power_supply_usb_type_text)) ?
		    power_supply_usb_type_text[type] :
		    "Unknown";
	mca_log_info("usb real type = %s\n", s);
	return sysfs_emit(buf, "%s\n", s);
}
static CLASS_ATTR_RO(usb_real_type);

static ssize_t wireless_type_show(const struct class *class,
				  const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(wireless_type);

static ssize_t authentic_show(const struct class *class,
			      const struct class_attribute *attr, char *buf)
{
	bool authentic = false;
	int ret = strategy_class_fg_get_authentic(&authentic);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", authentic);
}
static ssize_t authentic_store(const struct class *class,
			       const struct class_attribute *attr,
			       const char *buf, size_t count)
{
	/* Authentication is established by the fuel gauge, not by a sysfs write. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(authentic);

static ssize_t slave_authentic_show(const struct class *class,
				    const struct class_attribute *attr,
				    char *buf)
{
	int authentic = 0;
	int ret = platform_fg_ops_get_authentic(FG_IC_SLAVE, &authentic);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", authentic);
}
static ssize_t slave_authentic_store(const struct class *class,
				     const struct class_attribute *attr,
				     const char *buf, size_t count)
{
	/* Authentication is established by the fuel gauge, not by a sysfs write. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(slave_authentic);

static ssize_t pd_verifed_show(const struct class *class,
			       const struct class_attribute *attr, char *buf)
{
	int verified = 0;
	int ret;

	ret = protocol_class_get_adapter_verified(ADAPTER_PROTOCOL_PD,
						  &verified);
	if (ret < 0)
		return ret;
	mca_log_info("show pd_verifed = %d\n", verified);
	return sysfs_emit(buf, "%d\n", verified);
}
static ssize_t pd_verifed_store(const struct class *class,
				const struct class_attribute *attr,
				const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val) || (val != 0 && val != 1))
		return -EINVAL;
	mca_log_info("store pd_verifed = %d\n", val);
	ret = protocol_class_set_adapter_verified(ADAPTER_PROTOCOL_PD, val);
	if (ret < 0)
		return ret;
	return count;
}
static CLASS_ATTR_RW(pd_verifed);

static ssize_t quick_charge_type_show(const struct class *class,
				      const struct class_attribute *attr,
				      char *buf)
{
	int qc_type = 0;
	int ret;

	ret = mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					 STRATEGY_STATUS_TYPE_QC_TYPE, &qc_type);
	if (ret < 0)
		return ret;
	if (qc_type == 0) {
		ret = mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_BUCK_CHARGE,
						 STRATEGY_STATUS_TYPE_QC_TYPE, &qc_type);
		if (ret < 0)
			return ret;
	}
	return sysfs_emit(buf, "%d\n", qc_type);
}
static CLASS_ATTR_RO(quick_charge_type);

static ssize_t power_max_show(const struct class *class,
			      const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(power_max);

static ssize_t soc_decimal_show(const struct class *class,
				const struct class_attribute *attr, char *buf)
{
	int decimal = 0, rate = 0;
	int ret = strategy_class_fg_ops_get_soc_decimal(&decimal, &rate);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", decimal);
}
static CLASS_ATTR_RO(soc_decimal);

static ssize_t soc_decimal_rate_show(const struct class *class,
				     const struct class_attribute *attr,
				     char *buf)
{
	int decimal = 0, rate = 0;
	int ret = strategy_class_fg_ops_get_soc_decimal(&decimal, &rate);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", rate);
}
static CLASS_ATTR_RO(soc_decimal_rate);

static ssize_t otg_ui_support_show(const struct class *class,
				   const struct class_attribute *attr,
				   char *buf)
{
	bool support = false;
	int ret = platform_class_buckchg_ops_is_support_cid(MAIN_BUCK_CHARGER, &support);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", support);
}
static CLASS_ATTR_RO(otg_ui_support);

static ssize_t cid_status_show(const struct class *class,
			       const struct class_attribute *attr, char *buf)
{
	bool status = false;
	int ret;

	ret = protocol_class_pd_get_cid_status(TYPEC_PORT_0, &status);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", status);
}
static CLASS_ATTR_RO(cid_status);

static ssize_t cc_toggle_show(const struct class *class,
			      const struct class_attribute *attr, char *buf)
{
	bool en = false;
	int ret;

	ret = protocol_class_pd_get_cc_toggle(TYPEC_PORT_0, &en);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", en);
}
static ssize_t cc_toggle_store(const struct class *class,
			       const struct class_attribute *attr,
			       const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val) || (val != 0 && val != 1))
		return -EINVAL;
	ret = protocol_class_pd_set_cc_toggle(TYPEC_PORT_0, val != 0);
	if (ret < 0)
		return ret;
	return count;
}
static CLASS_ATTR_RW(cc_toggle);

static ssize_t has_dp_show(const struct class *class,
			   const struct class_attribute *attr, char *buf)
{
	bool has_dp = false;
	int ret;

	ret = protocol_class_pd_get_has_dp(TYPEC_PORT_0, &has_dp);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", has_dp);
}
static CLASS_ATTR_RO(has_dp);

static ssize_t dam_ovpgate_show(const struct class *class,
				const struct class_attribute *attr, char *buf)
{
	bool status = false;
	int ret;

	ret = platform_class_cp_get_ovpgate_status(CP_ROLE_MASTER, &status);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", status);
}
static ssize_t dam_ovpgate_store(const struct class *class,
				 const struct class_attribute *attr,
				 const char *buf, size_t count)
{
	bool en = false;
	int ret;

	if (kstrtobool(buf, &en))
		return -EINVAL;
	ret = platform_class_cp_enable_ovpgate(CP_ROLE_MASTER, en);
	if (ret < 0)
		return ret;
	return count;
}
static CLASS_ATTR_RW(dam_ovpgate);

static ssize_t pmic_ibat_show(const struct class *class,
			      const struct class_attribute *attr, char *buf)
{
	int ibat = 0;
	int ret;

	ret = platform_class_buckchg_ops_get_pack_ibat(MAIN_BUCK_CHARGER,
						       &ibat);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", ibat);
}
static CLASS_ATTR_RO(pmic_ibat);

static ssize_t wireless_chip_fw_show(const struct class *class,
				     const struct class_attribute *attr,
				     char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static ssize_t wireless_chip_fw_store(const struct class *class,
				      const struct class_attribute *attr,
				      const char *buf, size_t count)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(wireless_chip_fw);

static ssize_t reverse_chg_mode_show(const struct class *class,
				     const struct class_attribute *attr,
				     char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static ssize_t reverse_chg_mode_store(const struct class *class,
				      const struct class_attribute *attr,
				      const char *buf, size_t count)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(reverse_chg_mode);

static ssize_t reverse_chg_state_show(const struct class *class,
				      const struct class_attribute *attr,
				      char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(reverse_chg_state);

static ssize_t magnetic_case_flag_show(const struct class *class,
				       const struct class_attribute *attr,
				       char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(magnetic_case_flag);

static ssize_t tx_adapter_show(const struct class *class,
			       const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(tx_adapter);

static ssize_t rx_vout_show(const struct class *class,
			    const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(rx_vout);

static ssize_t rx_vrect_show(const struct class *class,
			     const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(rx_vrect);

static ssize_t rx_iout_show(const struct class *class,
			    const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(rx_iout);

static ssize_t rx_ss_show(const struct class *class,
			  const struct class_attribute *attr, char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RO(rx_ss);

static ssize_t shipmode_count_reset_show(const struct class *class,
					 const struct class_attribute *attr,
					 char *buf)
{
	bool ship = false;
	int ret;

	ret = platform_class_buckchg_ops_get_ship_mode(MAIN_BUCK_CHARGER,
						       &ship);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", ship);
}
static ssize_t shipmode_count_reset_store(const struct class *class,
					  const struct class_attribute *attr,
					  const char *buf, size_t count)
{
	int val = 0;
	int ret;

	if (kstrtoint(buf, 10, &val) || (val != 0 && val != 1))
		return -EINVAL;
	ret = platform_class_buckchg_ops_set_ship_mode(MAIN_BUCK_CHARGER,
						       val != 0);
	if (ret < 0)
		return ret;
	return count;
}
static CLASS_ATTR_RW(shipmode_count_reset);

static ssize_t wls_thermal_remove_show(const struct class *class,
				       const struct class_attribute *attr,
				       char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static ssize_t wls_thermal_remove_store(const struct class *class,
					const struct class_attribute *attr,
					const char *buf, size_t count)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(wls_thermal_remove);

static ssize_t hall_phone_case_show(const struct class *class,
				    const struct class_attribute *attr,
				    char *buf)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static ssize_t hall_phone_case_store(const struct class *class,
				     const struct class_attribute *attr,
				     const char *buf, size_t count)
{
	/* No wireless strategy/hardware provider is fitted to Onyx. */
	return -EOPNOTSUPP;
}
static CLASS_ATTR_RW(hall_phone_case);

static ssize_t soh_show(const struct class *class,
			const struct class_attribute *attr, char *buf)
{
	int soh = 0;
	int ret = strategy_class_fg_get_soh(&soh);

	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", soh);
}
static CLASS_ATTR_RO(soh);

static const struct class_attribute * const mca_qcom_sysfs_attrs[] = {
	&class_attr_real_type,
	&class_attr_usb_real_type,
	&class_attr_wireless_type,
	&class_attr_authentic,
	&class_attr_slave_authentic,
	&class_attr_pd_verifed,
	&class_attr_quick_charge_type,
	&class_attr_power_max,
	&class_attr_soc_decimal,
	&class_attr_soc_decimal_rate,
	&class_attr_otg_ui_support,
	&class_attr_cid_status,
	&class_attr_cc_toggle,
	&class_attr_has_dp,
	&class_attr_dam_ovpgate,
	&class_attr_pmic_ibat,
	&class_attr_wireless_chip_fw,
	&class_attr_reverse_chg_mode,
	&class_attr_reverse_chg_state,
	&class_attr_magnetic_case_flag,
	&class_attr_tx_adapter,
	&class_attr_rx_vout,
	&class_attr_rx_vrect,
	&class_attr_rx_iout,
	&class_attr_rx_ss,
	&class_attr_shipmode_count_reset,
	&class_attr_wls_thermal_remove,
	&class_attr_hall_phone_case,
	&class_attr_soh,
	NULL,
};


static int mca_qcom_sysfs_probe(struct platform_device *pdev)
{
	struct mca_qcom_sysfs_dev *info;
	int i, ret;

	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->dev = &pdev->dev;
	info->support_multi_typec = of_property_read_bool(pdev->dev.of_node,
						      "mi,support_multi_typec");
	/* class_create owns the class until its final reference is released. */
	info->class = class_create("qcom-battery");
	if (IS_ERR(info->class))
		return PTR_ERR(info->class);
	for (i = 0; mca_qcom_sysfs_attrs[i]; i++) {
		ret = class_create_file(info->class, mca_qcom_sysfs_attrs[i]);
		if (ret)
			goto remove_files;
	}
	platform_set_drvdata(pdev, info);
	mca_log_info("probe ok\n");
	return 0;

remove_files:
	while (i-- > 0)
		class_remove_file(info->class, mca_qcom_sysfs_attrs[i]);
	class_destroy(info->class);
	return ret;
}

static void mca_qcom_sysfs_remove(struct platform_device *pdev)
{
	struct mca_qcom_sysfs_dev *info = platform_get_drvdata(pdev);
	int i;

	/* Removing each file drains its active callbacks before provider teardown. */
	for (i = 0; mca_qcom_sysfs_attrs[i]; i++)
		class_remove_file(info->class, mca_qcom_sysfs_attrs[i]);
	class_destroy(info->class);
}

static const struct of_device_id match_table[] = {
	{ .compatible = "mca,qcom_sysfs" },
	{},
};
MODULE_DEVICE_TABLE(of, match_table);

static struct platform_driver mca_qcom_sysfs_driver = {
	.driver = {
		.name = "mca_qcom_sysfs",
		.of_match_table = match_table,
	},
	.probe = mca_qcom_sysfs_probe,
	.remove = mca_qcom_sysfs_remove,
};
module_platform_driver(mca_qcom_sysfs_driver);

MODULE_DESCRIPTION("mca qcom sysfs");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
