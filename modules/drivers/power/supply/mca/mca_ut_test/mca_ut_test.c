#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <mca/common/mca_log.h>
#include <mca/common/mca_hwid.h>
#include <mca/common/mca_voter.h>
#include <mca/strategy/strategy_class.h>
#include <mca/strategy/strategy_fg_class.h>
#include <mca/platform/platform_fg_ic_ops.h>
#include <mca/platform/platform_wireless_class.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "ut_test"
#endif

#define UT_QC_STATUS_IS_QUICK_CHARGE 2
#define UT_QC_STATUS_FCC 5

#define UT_VENDOR_DEFAULT "_nvt"

struct ut_test {
	struct device *dev;
	struct class class;
	char *config_str;
	struct mca_votable *buck_charge_votable;
	struct delayed_work voter_monitor_work;
	struct delayed_work parse_dt_work;
};

static ssize_t config_show(const struct class *class,
			   const struct class_attribute *attr, char *buf)
{
	struct ut_test *ut = container_of(class, struct ut_test, class);
	const char *str = smp_load_acquire(&ut->config_str);

	if (!str)
		str = "";

	return scnprintf(buf, PAGE_SIZE, "%s\n", str);
}
static CLASS_ATTR_RO(config);

static ssize_t current_fcc_show(const struct class *class,
				const struct class_attribute *attr, char *buf)
{
	struct ut_test *ut = container_of(class, struct ut_test, class);
	int wls_present = 0;
	int is_quick_charge = 0;
	int fcc = 0;
	unsigned int type;

	platform_class_wireless_is_present(0, &wls_present);
	if (!wls_present) {
		type = STRATEGY_FUNC_TYPE_QUICK_CHARGE;
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_CHARGE,
					     UT_QC_STATUS_IS_QUICK_CHARGE,
					     &is_quick_charge);
	} else {
		type = STRATEGY_FUNC_TYPE_QUICK_WIRELESS;
		mca_strategy_func_get_status(STRATEGY_FUNC_TYPE_QUICK_WIRELESS,
					     UT_QC_STATUS_IS_QUICK_CHARGE,
					     &is_quick_charge);
	}

	if (is_quick_charge == 1) {
		mca_strategy_func_get_status(type, UT_QC_STATUS_FCC, &fcc);
	} else {
		if (ut->buck_charge_votable)
			fcc = mca_get_effective_result(ut->buck_charge_votable);
	}

	return scnprintf(buf, PAGE_SIZE, "%lld\n", (long long)fcc * 1000);
}
static CLASS_ATTR_RO(current_fcc);

static struct attribute *ut_test_attrs[] = {
	&class_attr_config.attr,
	&class_attr_current_fcc.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ut_test);

static void ut_voter_monitor_work(struct work_struct *work)
{
	struct ut_test *ut = container_of(to_delayed_work(work), struct ut_test,
					  voter_monitor_work);

	if (!ut->buck_charge_votable) {
		ut->buck_charge_votable = mca_find_votable("buck_charge_curr");
		if (!ut->buck_charge_votable) {
			mca_log_err("voter not ready, wait %dms to retry\n",
				    1000);
			queue_delayed_work(system_wq, &ut->voter_monitor_work,
					   msecs_to_jiffies(1000));
			return;
		}
	}

	mca_log_err("voter is ok\n");
}

static const char *const ut_region_suffix[] = {
	"_global",
	"_in",
	"_jp",
};
static const char *const ut_vendor_suffix[] = {
	[0] = "_byd", [1] = "_coslight", [2] = "_swd",	 [4] = "_scud",
	[5] = "_tws", [6] = "_lishen",	 [7] = "_desay",
};

static const char *ut_vendor_suffix_of(int vendor_id)
{
	if (vendor_id >= 0 &&
	    (size_t)vendor_id < ARRAY_SIZE(ut_vendor_suffix) &&
	    ut_vendor_suffix[vendor_id])
		return ut_vendor_suffix[vendor_id];

	return UT_VENDOR_DEFAULT;
}

#define ut_test_read_param(node, key, keysz, base, vendor, region, type)  \
	({                                                                \
		const char *__val = NULL;                                 \
		scnprintf((key), (keysz), "%s%s%s%s", (base), (vendor),   \
			  (region), (type));                              \
		mca_log_info("dts_key: %s\n", (key));                     \
		of_property_read_string((node), (key), &__val);           \
		mca_log_info("%s : %s\n", (key), __val);                  \
		if (!__val)                                               \
			mca_log_err("read property %s failed!\n", (key)); \
		__val;                                                    \
	})

static void ut_test_append_kv(char *out, size_t size, size_t *len,
			       const char *key, const char *val)
{
	*len += scnprintf(out + *len, size - *len, "%s%s,%s",
			 *len ? "," : "", key, val);
}

static void ut_test_parse_dt_work(struct work_struct *work)
{
	struct ut_test *ut = container_of(to_delayed_work(work), struct ut_test,
					  parse_dt_work);
	struct device_node *node = ut->dev->of_node;
	const struct mca_hwid *hwid = mca_get_hwid_info();
	const char *vendor = "";
	const char *region = "";
	const char *type = "";
	const char *cycle_volt = NULL;
	const char *cycle_step_curr = NULL;
	const char *temp_term_curr = NULL;
	const char *thermal = NULL;
	const char *devname = NULL;
	bool has_lossless;
	char key[64];
	int vendor_id = 0;
	size_t len = 1, used = 0;
	char *out;

	if (of_find_property(node, "has-global-batt-para", NULL)) {
		strategy_class_fg_get_pack_vendor_id(&vendor_id);
		mca_log_info("country_version = %d, pack_vendor_id = %d\n",
			     hwid ? hwid->country_version : 0, vendor_id);

		if (hwid) {
			unsigned int idx = hwid->country_version - 1;

			if (idx < ARRAY_SIZE(ut_region_suffix))
				region = ut_region_suffix[idx];
		}

		vendor = ut_vendor_suffix_of(vendor_id);
	}

	if (of_find_property(node, "has-tmp-batt-para", NULL)) {
		platform_fg_ops_get_device_name(0, &devname);
		if (!devname) {
			mca_log_err("get device name fail, wait for it\n");
			return;
		}
		mca_log_err("device name = %s\n", devname);
		if (!strcmp("2@BP", devname))
			type = "_xm22";
		if (!strcmp("5@BP", devname))
			type = "_xm95";
	}

	cycle_volt = ut_test_read_param(node, key, sizeof(key), "mi,cycle_volt",
					vendor, region, type);
	if (cycle_volt)
		len += strlen("cycle_volt") + strlen(cycle_volt) + 2;

	cycle_step_curr = ut_test_read_param(node, key, sizeof(key),
					     "mi,cycle_step_curr", vendor,
					     region, type);
	if (cycle_step_curr)
		len += strlen("cycle_step_curr") + strlen(cycle_step_curr) + 2;

	temp_term_curr = ut_test_read_param(node, key, sizeof(key),
					    "mi,temp_term_curr", vendor, region,
					    type);
	if (temp_term_curr)
		len += strlen("temp_term_curr") + strlen(temp_term_curr) + 2;

	thermal = ut_test_read_param(node, key, sizeof(key), "mi,thermal",
				     vendor, region, type);
	if (thermal)
		len += strlen("thermal") + strlen(thermal) + 2;

	scnprintf(key, sizeof(key), "%s%s%s%s", "mi,lossless_rechg", vendor,
		  region, type);
	mca_log_info("dts_key: %s\n", key);
	has_lossless = of_find_property(node, key, NULL) != NULL;
	if (has_lossless)
		len += sizeof(",lossless_rechg");
	mca_log_info("%s : %s\n", key, has_lossless ? "true" : "false");

	/* Class output is limited to one page, including its terminating NUL. */
	if (len > PAGE_SIZE - 1) {
		mca_log_err("battery configuration exceeds class output size\n");
		return;
	}
	out = kzalloc(len, GFP_KERNEL);
	if (!out)
		return;
	if (cycle_volt)
		ut_test_append_kv(out, len, &used, "cycle_volt", cycle_volt);
	if (cycle_step_curr)
		ut_test_append_kv(out, len, &used, "cycle_step_curr", cycle_step_curr);
	if (temp_term_curr)
		ut_test_append_kv(out, len, &used, "temp_term_curr", temp_term_curr);
	if (thermal)
		ut_test_append_kv(out, len, &used, "thermal", thermal);
	if (has_lossless)
		scnprintf(out + used, len - used, "%slossless_rechg", used ? "," : "");
	/* This work runs once; publish only a complete immutable string. */
	smp_store_release(&ut->config_str, out);
}

static void ut_test_class_release(const struct class *class)
{
	struct ut_test *ut = container_of(class, struct ut_test, class);

	kfree(ut->config_str);
	kfree(ut);
}

static int ut_test_probe(struct platform_device *pdev)
{
	struct ut_test *ut;
	int rc;

	mca_log_err("start\n");

	ut = kzalloc(sizeof(*ut), GFP_KERNEL);
	if (!ut) {
		mca_log_err("memory alloc failed!\n");
		return -ENOMEM;
	}

	ut->dev = &pdev->dev;
	ut->class.name = "ut_test";
	ut->class.class_groups = ut_test_groups;
	ut->class.class_release = ut_test_class_release;

	rc = class_register(&ut->class);
	if (rc < 0) {
		mca_log_err("Failed to create ut_class, rc = %d\n", rc);
		kfree(ut);
		return rc;
	}

	INIT_DELAYED_WORK(&ut->voter_monitor_work, ut_voter_monitor_work);
	INIT_DELAYED_WORK(&ut->parse_dt_work, ut_test_parse_dt_work);
	platform_set_drvdata(pdev, ut);

	queue_delayed_work(system_wq, &ut->parse_dt_work, msecs_to_jiffies(5000));
	mca_log_err("success\n");
	queue_delayed_work(system_wq, &ut->voter_monitor_work, msecs_to_jiffies(6250));

	return 0;
}

static void ut_test_remove(struct platform_device *pdev)
{
	struct ut_test *ut = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ut->voter_monitor_work);
	cancel_delayed_work_sync(&ut->parse_dt_work);
	class_unregister(&ut->class);
}

static const struct of_device_id ut_test_of_match[] = {
	{ .compatible = "charge,ut-test" },
	{},
};
MODULE_DEVICE_TABLE(of, ut_test_of_match);

static struct platform_driver ut_test_driver = {
	.driver = {
		.name = "ut_test",
		.of_match_table = ut_test_of_match,
	},
	.probe = ut_test_probe,
	.remove = ut_test_remove,
};

static int __init ut_test_init(void)
{
	int rc;

	rc = platform_driver_register(&ut_test_driver);
	if (rc < 0)
		pr_err("ut_test driver register failed!\n");

	return rc;
}
module_init(ut_test_init);

static void __exit ut_test_exit(void)
{
	platform_driver_unregister(&ut_test_driver);
}
module_exit(ut_test_exit);

MODULE_DESCRIPTION("ut test driver");
MODULE_AUTHOR("v-wangyang27@xiaomi.com");
MODULE_LICENSE("GPL v2");
