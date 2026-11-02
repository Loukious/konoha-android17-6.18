// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/idr.h>
#include <linux/thermal.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/of.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <linux/suspend.h>
#include <linux/cpu_cooling.h>
#include <linux/soc/qcom/panel_event_notifier.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>
#include <linux/kobject.h>
#include <linux/kernfs.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#if IS_ENABLED(CONFIG_XM_POWER_SUPPLY)
#include <mca/common/mca_event.h>
#include <mca/smartchg/smart_chg_class.h>
#endif
#include <linux/platform_device.h>
#include <linux/sort.h>

#if defined(CONFIG_DRM_PANEL)
static struct drm_panel *prim_panel;
#endif

#define BOOST_BUFFER_SIZE 128
#define BOARD__BUFFER_SIZE 128
struct mi_thermal_device {
	struct device *dev;
#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
	struct delayed_work work;
#endif
};

struct mi_powersave_device {
	struct device *dev;
};

struct freq_table {
	u32 frequency;
};

struct cpufreq_device {
	int id;
	unsigned int cpufreq_state;
	unsigned int max_level;
	struct freq_table *freq_table; /* In descending order */
	struct cpufreq_policy *policy;
	struct list_head node;
	struct freq_qos_request qos_req;
};

#if IS_ENABLED(CONFIG_MI_THERMAL_ATC_ENABLE)
static atomic_t atc_mode = ATOMIC_INIT(-1);
#endif

#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
struct usb_monitor {
	struct notifier_block psy_nb;
	int usb_online;
};
static struct usb_monitor usb_state;
static atomic_t charger_mode = ATOMIC_INIT(-1);
#endif
static struct mi_thermal_device mi_thermal_dev;
static struct mi_powersave_device mi_powersave_dev;

static int screen_state = 0;
static int screen_light = 0;
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
static void *cookie_sec = NULL;
static struct drm_panel *sec_panel;
#endif
static void *cookie = NULL;

static atomic_t temp_state = ATOMIC_INIT(0);
static atomic_t switch_mode = ATOMIC_INIT(-1);
static atomic_t balance_mode = ATOMIC_INIT(0);
static atomic_t modem_limit = ATOMIC_INIT(0);
static atomic_t poor_modem_limit = ATOMIC_INIT(0);
static atomic_t modem_level = ATOMIC_INIT(0);
static atomic_t market_download_limit = ATOMIC_INIT(0);
static atomic_t flash_state = ATOMIC_INIT(0);
static atomic_t wifi_limit = ATOMIC_INIT(0);
static atomic_t thermal_max_brightness = ATOMIC_INIT(0);
static atomic_t board_sensor_temp_comp_default = ATOMIC_INIT(0);
static atomic_t cpu_nolimit_temp_default = ATOMIC_INIT(0);
static atomic_t torch_level = ATOMIC_INIT(0);
static atomic_t torch_real_level = ATOMIC_INIT(-1);
static atomic_t boot_complete = ATOMIC_INIT(0);
static atomic_t cloud_game = ATOMIC_INIT(0);
static atomic_t display_therm_temp = ATOMIC_INIT(0);
static atomic_t dynamic_tj = ATOMIC_INIT(0);
static atomic_t ntn_limit = ATOMIC_INIT(0);
static atomic_t super_hdr = ATOMIC_INIT(0);
static atomic_t voice_limit = ATOMIC_INIT(0);
static atomic_t powersave_mode = ATOMIC_INIT(-1);
static atomic_t power_level = ATOMIC_INIT(-1);
static char boost_buf[128];
static char *board_sensor;
static char board_sensor_temp[128];
static char ambient_sensor_temp[128];
static char board_sensor_second_temp[128];
static char board_sensor_other_temp[128];

static LIST_HEAD(cpufreq_dev_list);
static DEFINE_MUTEX(cpufreq_list_lock);
static DEFINE_MUTEX(message_lock);
static DEFINE_SPINLOCK(screen_lock);

static struct delayed_work screen_state_dw;

/* cpufreq_list_lock covers requests, state and policy ownership. */
static int cpufreq_set_level(struct cpufreq_device *cdev, unsigned long state)
{
	int ret;

	if (state > cdev->max_level)
		return -EINVAL;
	if (cdev->cpufreq_state == state)
		return 0;
	ret = freq_qos_update_request(&cdev->qos_req,
				      cdev->freq_table[state].frequency);
	if (ret < 0)
		return ret;
	cdev->cpufreq_state = state;
	return 0;
}

static int cpu_limits_set_level(unsigned int cpu, unsigned int max_freq)
{
	struct cpufreq_device *cdev;
	unsigned int level;
	int ret = -ENODEV;

	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) || !max_freq)
		return -EINVAL;
	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry(cdev, &cpufreq_dev_list, node) {
		if (!cpumask_test_cpu(cpu, cdev->policy->related_cpus))
			continue;
		/* Round down, clamping to the minimum supported frequency. */
		for (level = 0; level < cdev->max_level; level++)
			if (max_freq >= cdev->freq_table[level].frequency)
				break;
		ret = cpufreq_set_level(cdev, level);
		break;
	}
	mutex_unlock(&cpufreq_list_lock);
	return ret;
}

static int compare_frequency(const void *left, const void *right)
{
	const struct freq_table *a = left, *b = right;

	return (a->frequency < b->frequency) - (a->frequency > b->frequency);
}

static void destroy_thermal_cpu(void)
{
	struct cpufreq_device *cdev, *tmp;

	mutex_lock(&cpufreq_list_lock);
	list_for_each_entry_safe(cdev, tmp, &cpufreq_dev_list, node) {
		freq_qos_remove_request(&cdev->qos_req);
		cpufreq_cpu_put(cdev->policy);
		list_del(&cdev->node);
		kfree(cdev->freq_table);
		kfree(cdev);
	}
	mutex_unlock(&cpufreq_list_lock);
}

static int cpu_thermal_init(void)
{
	struct cpufreq_device *cdev, *entry;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *pos;
	unsigned int count, i, unique;
	int cpu, ret = 0;
	bool found;

	mutex_lock(&cpufreq_list_lock);
	for_each_possible_cpu(cpu) {
		/* An offline CPU may share an already registered policy. */
		found = false;
		list_for_each_entry(entry, &cpufreq_dev_list, node)
			if (cpumask_test_cpu(cpu, entry->policy->related_cpus)) {
				found = true;
				break;
			}
		if (found)
			continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue; /* A later online sibling can supply this policy. */
		count = policy->freq_table ?
			cpufreq_table_count_valid_entries(policy) : 0;
		if (!count) {
			ret = -ENODEV;
			goto put_policy;
		}
		cdev = kzalloc(sizeof(*cdev), GFP_KERNEL);
		if (!cdev) {
			ret = -ENOMEM;
			goto put_policy;
		}
		cdev->freq_table = kmalloc_array(count, sizeof(*cdev->freq_table),
					       GFP_KERNEL);
		if (!cdev->freq_table) {
			ret = -ENOMEM;
			goto free_cdev;
		}
		i = 0;
		cpufreq_for_each_valid_entry(pos, policy->freq_table) {
			if (!pos->frequency || pos->frequency > S32_MAX) {
				ret = -EINVAL;
				goto free_table;
			}
			cdev->freq_table[i++].frequency = pos->frequency;
		}
		sort(cdev->freq_table, count, sizeof(*cdev->freq_table),
		     compare_frequency, NULL);
		/* Duplicate OPP entries must not create a zero-frequency level. */
		for (i = 1, unique = 1; i < count; i++)
			if (cdev->freq_table[i].frequency !=
			    cdev->freq_table[unique - 1].frequency)
				cdev->freq_table[unique++] = cdev->freq_table[i];
		cdev->max_level = unique - 1;
		cdev->policy = policy;
		cdev->id = policy->cpu;
		ret = freq_qos_add_request(&policy->constraints, &cdev->qos_req,
					  FREQ_QOS_MAX,
					  cdev->freq_table[0].frequency);
		if (ret < 0)
			goto free_table;
		list_add_tail(&cdev->node, &cpufreq_dev_list);
		continue;
free_table:
		kfree(cdev->freq_table);
free_cdev:
		kfree(cdev);
put_policy:
		cpufreq_cpu_put(policy);
		break;
	}
	if (ret >= 0) {
		for_each_possible_cpu(cpu) {
			found = false;
			list_for_each_entry(entry, &cpufreq_dev_list, node)
				if (cpumask_test_cpu(cpu, entry->policy->related_cpus)) {
					found = true;
					break;
				}
			if (!found) {
				ret = -EPROBE_DEFER;
				break;
			}
		}
	}
	mutex_unlock(&cpufreq_list_lock);
	if (ret < 0) {
		destroy_thermal_cpu();
		return ret;
	}
	return 0;
}

/* Sysfs strings are data, never printf formats. */
static ssize_t message_string_show(char *buf, const char *value)
{
	ssize_t ret;

	mutex_lock(&message_lock);
	ret = sysfs_emit(buf, "%s", value);
	mutex_unlock(&message_lock);
	return ret;
}

static ssize_t message_string_store(char *value, const char *buf, size_t len)
{
	if (len >= BOOST_BUFFER_SIZE)
		return -E2BIG;
	mutex_lock(&message_lock);
	memcpy(value, buf, len);
	value[len] = '\0';
	mutex_unlock(&message_lock);
	return len;
}

static ssize_t thermal_temp_state_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&temp_state));
}

static ssize_t thermal_temp_state_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&temp_state, val);

	return len;
}

static DEVICE_ATTR(temp_state, 0664, thermal_temp_state_show,
		   thermal_temp_state_store);

#ifdef CONFIG_MI_THERMAL_TZMODE
static int tzdriver_current_mode = 1;
#define GAME_MODE 0x0
#define NON_GAME_MODE 0x01
static ssize_t tzdriver_current_mode_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t len)
{
	int retVal = 0;
	int value;

	value = simple_strtol(buf, NULL, 10);

	if ((value != GAME_MODE) && (value != NON_GAME_MODE))
		return len;

	retVal = teei_switch_current_mode(value);

	if (retVal == 0)
		tzdriver_current_mode = value;

	return len;
}

static ssize_t tzdriver_current_mode_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%d\n", tzdriver_current_mode);
}

static DEVICE_ATTR(tzdriver_current_mode, 0664, tzdriver_current_mode_show,
		   tzdriver_current_mode_store);
#endif

static ssize_t cpu_limits_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	/* Stock userspace writes commands; its read ABI is an empty file. */
	return 0;
}

static ssize_t cpu_limits_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	unsigned int cpu, max;
	char extra;
	int ret;

	if (strchr(buf, '-') ||
	    sscanf(buf, "cpu%u %u %c", &cpu, &max, &extra) != 2)
		return -EINVAL;
	ret = cpu_limits_set_level(cpu, max);
	return ret < 0 ? ret : len;
}

static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);
static ssize_t thermal_max_brightness_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%d\n",
			atomic_read(&thermal_max_brightness));
}
static ssize_t thermal_max_brightness_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	int val, ret;
	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;
	atomic_set(&thermal_max_brightness, val);
	return len;
}
static DEVICE_ATTR(thermal_max_brightness, 0664, thermal_max_brightness_show,
		   thermal_max_brightness_store);

static ssize_t thermal_screen_state_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(screen_state));
}

static DEVICE_ATTR(screen_state, 0664, thermal_screen_state_show, NULL);

static ssize_t thermal_sconfig_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&switch_mode));
}

static ssize_t thermal_sconfig_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&switch_mode, val);

#if IS_ENABLED(CONFIG_XM_POWER_SUPPLY)
	mca_smartchg_set_scene(val);
#endif

	return len;
}

static ssize_t thermal_boost_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return message_string_show(buf, boost_buf);
}

static ssize_t thermal_boost_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	return message_string_store(boost_buf, buf, len);
}

static ssize_t thermal_balance_mode_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&balance_mode));
}

static ssize_t thermal_balance_mode_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&balance_mode, val);

	return len;
}

static ssize_t thermal_market_download_limit_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	return sysfs_emit(buf, "%d\n",
			atomic_read(&market_download_limit));
}

static ssize_t
thermal_market_download_limit_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&market_download_limit, val);
	return len;
}

static ssize_t thermal_wifi_limit_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&wifi_limit));
}
static ssize_t thermal_wifi_limit_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&wifi_limit, val);
	return len;
}

static DEVICE_ATTR(wifi_limit, 0664, thermal_wifi_limit_show,
		   thermal_wifi_limit_store);

static ssize_t thermal_flash_state_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&flash_state));
}
static ssize_t thermal_flash_state_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&flash_state, val);
	return len;
}

static DEVICE_ATTR(flash_state, 0664, thermal_flash_state_show,
		   thermal_flash_state_store);

static DEVICE_ATTR(boost, 0644, thermal_boost_show, thermal_boost_store);

static DEVICE_ATTR(sconfig, 0664, thermal_sconfig_show, thermal_sconfig_store);

static DEVICE_ATTR(balance_mode, 0664, thermal_balance_mode_show,
		   thermal_balance_mode_store);

static DEVICE_ATTR(market_download_limit, 0664,
		   thermal_market_download_limit_show,
		   thermal_market_download_limit_store);

static ssize_t thermal_board_sensor_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor);
}

static DEVICE_ATTR(board_sensor, 0664, thermal_board_sensor_show, NULL);

static ssize_t thermal_board_sensor_temp_show(struct device *dev,
					      struct device_attribute *attr,
					      char *buf)
{
	return message_string_show(buf, board_sensor_temp);
}

static ssize_t thermal_board_sensor_temp_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t len)
{
	ssize_t ret;
#if IS_ENABLED(CONFIG_XM_POWER_SUPPLY)
	int temp;

	ret = kstrtoint(buf, 10, &temp);
	if (ret)
		return ret;
#endif
	ret = message_string_store(board_sensor_temp, buf, len);
	if (ret < 0)
		return ret;
#if IS_ENABLED(CONFIG_XM_POWER_SUPPLY)
	mca_event_block_notify(MCA_EVENT_TYPE_THERMAL_TEMP,
			       MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE, &temp);
#endif
	return len;
}

static DEVICE_ATTR(board_sensor_temp, 0664, thermal_board_sensor_temp_show,
		   thermal_board_sensor_temp_store);

static ssize_t thermal_ambient_sensor_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%s", "ABT-SENSOR");
}

static DEVICE_ATTR(ambient_sensor, 0664, thermal_ambient_sensor_show, NULL);

static ssize_t thermal_ambient_sensor_temp_show(struct device *dev,
						struct device_attribute *attr,
						char *buf)
{
	return message_string_show(buf, ambient_sensor_temp);
}

static ssize_t thermal_ambient_sensor_temp_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t len)
{
	return message_string_store(ambient_sensor_temp, buf, len);
}

static DEVICE_ATTR(ambient_sensor_temp, 0664, thermal_ambient_sensor_temp_show,
		   thermal_ambient_sensor_temp_store);

static ssize_t thermal_board_sensor_second_temp_show(struct device *dev,
						     struct device_attribute *attr,
						     char *buf)
{
	return message_string_show(buf, board_sensor_second_temp);
}

static ssize_t thermal_board_sensor_second_temp_store(struct device *dev,
						      struct device_attribute *attr,
						      const char *buf, size_t len)
{
	return message_string_store(board_sensor_second_temp, buf, len);
}

static DEVICE_ATTR(board_sensor_second_temp, 0664,
		   thermal_board_sensor_second_temp_show,
		   thermal_board_sensor_second_temp_store);

static ssize_t thermal_board_sensor_other_temp_show(struct device *dev,
						    struct device_attribute *attr,
						    char *buf)
{
	return message_string_show(buf, board_sensor_other_temp);
}

static ssize_t thermal_board_sensor_other_temp_store(struct device *dev,
						     struct device_attribute *attr,
						     const char *buf, size_t len)
{
	return message_string_store(board_sensor_other_temp, buf, len);
}

static DEVICE_ATTR(board_sensor_other_temp, 0664,
		   thermal_board_sensor_other_temp_show,
		   thermal_board_sensor_other_temp_store);

static ssize_t thermal_board_sensor_temp_comp_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	return sysfs_emit(buf, "%d\n",
			atomic_read(&board_sensor_temp_comp_default));
}

static ssize_t thermal_board_sensor_temp_comp_store(struct device *dev,
						    struct device_attribute *attr,
						    const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&board_sensor_temp_comp_default, val);

	return len;
}

static DEVICE_ATTR(board_sensor_temp_comp, 0664,
		   thermal_board_sensor_temp_comp_show,
		   thermal_board_sensor_temp_comp_store);

static ssize_t thermal_cpu_nolimit_temp_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n",
			atomic_read(&cpu_nolimit_temp_default));
}

static ssize_t thermal_cpu_nolimit_temp_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&cpu_nolimit_temp_default, val);

	return len;
}

static DEVICE_ATTR(cpu_nolimit_temp, 0664, thermal_cpu_nolimit_temp_show,
		   thermal_cpu_nolimit_temp_store);

static ssize_t thermal_torch_level_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&torch_level));
}

static ssize_t thermal_torch_level_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&torch_level, val);

	return len;
}

static DEVICE_ATTR(torch_level, 0664, thermal_torch_level_show,
		   thermal_torch_level_store);

static ssize_t thermal_torch_real_level_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&torch_real_level));
}

static ssize_t thermal_torch_real_level_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&torch_real_level, val);

	return len;
}

static DEVICE_ATTR(torch_real_level, 0664, thermal_torch_real_level_show,
		   thermal_torch_real_level_store);

static ssize_t thermal_boot_complete_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&boot_complete));
}

static ssize_t thermal_boot_complete_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&boot_complete, val);

	return len;
}

static DEVICE_ATTR(boot_complete, 0664, thermal_boot_complete_show,
		   thermal_boot_complete_store);

static ssize_t cloud_game_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&cloud_game));
}

static ssize_t cloud_game_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&cloud_game, val);

	return len;
}

static DEVICE_ATTR(cloud_game, 0664, cloud_game_show, cloud_game_store);

static ssize_t display_therm_temp_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&display_therm_temp));
}

static ssize_t display_therm_temp_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&display_therm_temp, val);

	return len;
}

static DEVICE_ATTR(display_therm_temp, 0664, display_therm_temp_show,
		   display_therm_temp_store);

static ssize_t thermal_dynamic_tj_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&dynamic_tj));
}

static ssize_t thermal_dynamic_tj_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&dynamic_tj, val);

	return len;
}

static DEVICE_ATTR(dynamic_tj, 0664, thermal_dynamic_tj_show,
		   thermal_dynamic_tj_store);

static ssize_t thermal_ntn_limit_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&ntn_limit));
}

static ssize_t thermal_ntn_limit_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&ntn_limit, val);

	return len;
}

static DEVICE_ATTR(ntn_limit, 0664, thermal_ntn_limit_show,
		   thermal_ntn_limit_store);

static ssize_t super_hdr_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&super_hdr));
}

static ssize_t super_hdr_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&super_hdr, val);

	return len;
}

static DEVICE_ATTR(super_hdr, 0664, super_hdr_show, super_hdr_store);

static ssize_t voice_limit_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&voice_limit));
}

static ssize_t voice_limit_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&voice_limit, val);

	return len;
}

static DEVICE_ATTR(voice_limit, 0664, voice_limit_show, voice_limit_store);

static ssize_t thermal_modem_limit_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&modem_limit));
}
static ssize_t thermal_modem_limit_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&modem_limit, val);
	return len;
}

static DEVICE_ATTR(modem_limit, 0664, thermal_modem_limit_show,
		   thermal_modem_limit_store);

static ssize_t thermal_poor_modem_limit_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&poor_modem_limit));
}
static ssize_t thermal_poor_modem_limit_store(struct device *dev,
					      struct device_attribute *attr,
					      const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&poor_modem_limit, val);
	return len;
}

static DEVICE_ATTR(poor_modem_limit, 0664, thermal_poor_modem_limit_show,
		   thermal_poor_modem_limit_store);

static ssize_t thermal_modem_level_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&modem_level));
}
static ssize_t thermal_modem_level_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&modem_level, val);
	return len;
}

static DEVICE_ATTR(modem_level, 0664, thermal_modem_level_show,
		   thermal_modem_level_store);

#if IS_ENABLED(CONFIG_MI_THERMAL_ATC_ENABLE)
static ssize_t thermal_atc_enable_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&atc_mode));
}

static ssize_t thermal_atc_enable_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&atc_mode, val);

	return len;
}

static DEVICE_ATTR(atc_enable, 0664, thermal_atc_enable_show,
		   thermal_atc_enable_store);
#endif

#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
static ssize_t thermal_charger_temp_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&charger_mode));
}

static ssize_t thermal_charger_temp_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&charger_mode, val);

	return len;
}

static DEVICE_ATTR(charger_temp, 0664, thermal_charger_temp_show,
		   thermal_charger_temp_store);

static ssize_t thermal_usb_online_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(usb_state.usb_online));
}

static DEVICE_ATTR(usb_online, 0664, thermal_usb_online_show, NULL);

static int usb_online_callback(struct notifier_block *nb, unsigned long val,
			       void *data)
{
	struct power_supply *psy = data;

	if (val == PSY_EVENT_PROP_CHANGED && psy &&
	    !strcmp(psy->desc->name, "usb"))
		mod_delayed_work(system_wq, &mi_thermal_dev.work, 0);
	return NOTIFY_OK;
}
#endif
static int of_parse_thermal_message(void)
{
	struct device_node *np;
	const char *name = "invalid";

	/* Onyx has no override. Keep the native ABI sentinel, not a fake sensor. */
	np = of_find_node_by_name(NULL, "thermal-message");
	if (np) {
		if (of_property_read_string(np, "board-sensor", &name))
			name = "invalid";
		board_sensor = kstrdup(name, GFP_KERNEL);
		of_node_put(np);
	} else {
		board_sensor = kstrdup(name, GFP_KERNEL);
	}
	return board_sensor ? 0 : -ENOMEM;
}

static ssize_t thermal_powersave_mode_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&powersave_mode));
}

static ssize_t thermal_powersave_mode_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&powersave_mode, val);

	return len;
}

static DEVICE_ATTR(powersave_mode, 0664, thermal_powersave_mode_show,
		   thermal_powersave_mode_store);

static ssize_t thermal_power_level_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&power_level));
}

static ssize_t thermal_power_level_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	int val, ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return ret;

	atomic_set(&power_level, val);

	return len;
}

static DEVICE_ATTR(power_level, 0664, thermal_power_level_show,
		   thermal_power_level_store);

static struct attribute *mi_powersave_dev_attr_group[] = {
	&dev_attr_powersave_mode.attr,
	&dev_attr_power_level.attr,
	NULL,
};

static struct attribute *mi_thermal_dev_attr_group[] = {
	&dev_attr_temp_state.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_boost.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_ambient_sensor.attr,
	&dev_attr_ambient_sensor_temp.attr,
	&dev_attr_board_sensor_second_temp.attr,
	&dev_attr_board_sensor_other_temp.attr,
	&dev_attr_board_sensor_temp_comp.attr,
	&dev_attr_cpu_nolimit_temp.attr,
	&dev_attr_torch_level.attr,
	&dev_attr_torch_real_level.attr,
	&dev_attr_boot_complete.attr,
	&dev_attr_cloud_game.attr,
	&dev_attr_display_therm_temp.attr,
	&dev_attr_dynamic_tj.attr,
	&dev_attr_ntn_limit.attr,
	&dev_attr_super_hdr.attr,
	&dev_attr_voice_limit.attr,
	&dev_attr_balance_mode.attr,
	&dev_attr_modem_limit.attr,
	&dev_attr_poor_modem_limit.attr,
	&dev_attr_modem_level.attr,
	&dev_attr_market_download_limit.attr,
	&dev_attr_wifi_limit.attr,
	&dev_attr_thermal_max_brightness.attr,
	&dev_attr_flash_state.attr,
#ifdef CONFIG_MI_THERMAL_TZMODE
	&dev_attr_tzdriver_current_mode.attr,
#endif
#if IS_ENABLED(CONFIG_MI_THERMAL_ATC_ENABLE)
	&dev_attr_atc_enable.attr,
#endif
#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
	&dev_attr_charger_temp.attr,
	&dev_attr_usb_online.attr,
#endif
	NULL,
};

static const struct attribute_group thermal_message_group = {
	.attrs = mi_thermal_dev_attr_group,
};
static const struct attribute_group *thermal_message_groups[] = {
	&thermal_message_group, NULL,
};
static const struct attribute_group powersave_group = {
	.attrs = mi_powersave_dev_attr_group,
};
static const struct attribute_group *powersave_groups[] = {
	&powersave_group, NULL,
};

static void
screen_state_for_thermal_callback(enum panel_event_notifier_tag tag,
				  struct panel_event_notification *notification,
				  void *client_data)
{
	unsigned long flags;
	unsigned int bit;
	bool on, changed;

	if (!notification || notification->notif_data.early_trigger)
		return;
	if (tag == PANEL_EVENT_NOTIFICATION_PRIMARY)
		bit = 1;
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
	else if (tag == PANEL_EVENT_NOTIFICATION_SECONDARY)
		bit = 2;
#endif
	else
		return;
	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_UNBLANK:
		on = true;
		break;
	case DRM_PANEL_EVENT_BLANK:
	case DRM_PANEL_EVENT_BLANK_LP:
		on = false;
		break;
	default:
		return;
	}
	spin_lock_irqsave(&screen_lock, flags);
	if (on)
		screen_light |= bit;
	else
		screen_light &= ~bit;
	changed = screen_state != !!screen_light;
	WRITE_ONCE(screen_state, !!screen_light);
	spin_unlock_irqrestore(&screen_lock, flags);
	if (changed)
		sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "screen_state");
}

static int register_thermal_panel(struct device_node *np, const char *property,
				enum panel_event_notifier_tag tag,
				enum panel_event_notifier_client client,
				struct drm_panel **registered, void **handle)
{
	struct device_node *node;
	struct drm_panel *panel;
	void *candidate;
	int count, i, ret = -ENODEV;

	if (*handle)
		return 0;
	count = of_count_phandle_with_args(np, property, NULL);
	for (i = 0; i < count; i++) {
		node = of_parse_phandle(np, property, i);
		if (!node)
			continue;
		panel = of_drm_find_panel(node);
		of_node_put(node);
		if (IS_ERR(panel)) {
			ret = PTR_ERR(panel);
			continue;
		}
		candidate = panel_event_notifier_register(tag, client, panel,
					 screen_state_for_thermal_callback, NULL);
		if (IS_ERR_OR_NULL(candidate)) {
			ret = candidate ? PTR_ERR(candidate) : -ENODEV;
			continue;
		}
		*registered = panel;
		*handle = candidate;
		return 0;
	}
	return ret;
}

static unsigned int panel_retries;

static void screen_state_check(struct work_struct *work)
{
	struct device_node *node;
	int ret;
	bool retry;

	node = of_find_node_by_name(NULL, "thermal-screen");
	if (!node) {
		pr_err("mi_thermal: missing thermal-screen node\n");
		return;
	}
	ret = register_thermal_panel(node, "panel",
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_THERMAL, &prim_panel, &cookie);
	retry = ret != 0;
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
	ret = register_thermal_panel(node, "panel1",
			PANEL_EVENT_NOTIFICATION_SECONDARY,
			PANEL_EVENT_NOTIFIER_CLIENT_THERMAL_SECOND,
			&sec_panel, &cookie_sec);
	retry |= ret != 0;
#endif
	of_node_put(node);
	if (retry && ++panel_retries < 10)
		schedule_delayed_work(&screen_state_dw, 5 * HZ);
	else if (retry)
		pr_err("mi_thermal: panel notification registration failed: %d\n", ret);
}

#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
static void usb_online_work(struct work_struct *work)
{
	struct power_supply *psy;
	union power_supply_propval value;
	int ret;

	psy = power_supply_get_by_name("usb");
	if (!psy)
		return;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
	power_supply_put(psy);
	if (ret)
		return;
	if (READ_ONCE(usb_state.usb_online) != value.intval) {
		WRITE_ONCE(usb_state.usb_online, value.intval);
		sysfs_notify(&mi_thermal_dev.dev->kobj, NULL, "usb_online");
	}
}
#endif

static int mi_thermal_probe(struct platform_device *pdev)
{
	struct device *dev;
	int ret;

	ret = of_parse_thermal_message();
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "board sensor configuration\n");
	ret = cpu_thermal_init();
	if (ret)
		goto free_sensor;
	INIT_DELAYED_WORK(&screen_state_dw, screen_state_check);
#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
	INIT_DELAYED_WORK(&mi_thermal_dev.work, usb_online_work);
#endif
	dev = thermal_device_register("thermal_message", thermal_message_groups);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto destroy_cpu;
	}
	mi_thermal_dev.dev = dev;
	dev = thermal_device_register("power_save", powersave_groups);
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		goto unregister_message;
	}
	mi_powersave_dev.dev = dev;
#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
	usb_state.psy_nb.notifier_call = usb_online_callback;
	ret = power_supply_reg_notifier(&usb_state.psy_nb);
	if (ret)
		goto unregister_powersave;
	/* Capture a cable already connected before notifier registration. */
	schedule_delayed_work(&mi_thermal_dev.work, 0);
#endif
	panel_retries = 0;
	schedule_delayed_work(&screen_state_dw, 0);
	return 0;

#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
unregister_powersave:
	device_unregister(mi_powersave_dev.dev);
	mi_powersave_dev.dev = NULL;
#endif
unregister_message:
	device_unregister(mi_thermal_dev.dev);
	mi_thermal_dev.dev = NULL;
destroy_cpu:
	destroy_thermal_cpu();
free_sensor:
	kfree(board_sensor);
	board_sensor = NULL;
	return dev_err_probe(&pdev->dev, ret, "thermal interface initialization\n");
}

static void mi_thermal_remove(struct platform_device *pdev)
{
	/* Withdraw all producers and drain callbacks before removing sysfs. */
#if IS_ENABLED(CONFIG_MI_THERMAL_MULTI_CHARGE)
	power_supply_unreg_notifier(&usb_state.psy_nb);
	cancel_delayed_work_sync(&mi_thermal_dev.work);
#endif
	cancel_delayed_work_sync(&screen_state_dw);
	if (cookie) {
		panel_event_notifier_unregister(cookie);
		cookie = NULL;
	}
#if IS_ENABLED(CONFIG_HAVE_MULTI_SCREEN)
	if (cookie_sec) {
		panel_event_notifier_unregister(cookie_sec);
		cookie_sec = NULL;
	}
#endif
	device_unregister(mi_powersave_dev.dev);
	mi_powersave_dev.dev = NULL;
	device_unregister(mi_thermal_dev.dev);
	mi_thermal_dev.dev = NULL;
	destroy_thermal_cpu();
	kfree(board_sensor);
	board_sensor = NULL;
}

static struct platform_driver mi_thermal_driver = {
	.probe = mi_thermal_probe,
	.remove = mi_thermal_remove,
	.driver = { .name = "mi-thermal-interface" },
};
static struct platform_device *mi_thermal_pdev;

static int __init mi_thermal_interface_init(void)
{
	int ret;

	ret = platform_driver_register(&mi_thermal_driver);
	if (ret)
		return ret;
	mi_thermal_pdev = platform_device_register_simple("mi-thermal-interface",
						       -1, NULL, 0);
	if (IS_ERR(mi_thermal_pdev)) {
		ret = PTR_ERR(mi_thermal_pdev);
		platform_driver_unregister(&mi_thermal_driver);
		return ret;
	}
	return 0;
}

static void __exit mi_thermal_interface_exit(void)
{
	platform_device_unregister(mi_thermal_pdev);
	platform_driver_unregister(&mi_thermal_driver);
}

module_init(mi_thermal_interface_init);
module_exit(mi_thermal_interface_exit);

MODULE_AUTHOR("Xiaomi thermal team");
MODULE_DESCRIPTION("Xiaomi thermal control interface");
MODULE_LICENSE("GPL v2");
