#include <linux/device.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/pm_wakeirq.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/regulator/consumer.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/notifier.h>
#include <linux/remoteproc/qcom_rproc.h>

#define MI_T1_GPIO_COUN_MAX 20
static int MI_T1_GPIO_COUNT = 4; //default for N1
static int FIRST_GPIO_ID = 0;
static bool b_legacy_drv = true;
#define MI_T1_MODE_MSK (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH) /*664*/
#define MI_T1_GPIO_PROC_NAME "modem_t1_gpio_ctrl"

static bool clk_reg = false;
static bool mi_t1_bound;
//T2 modem crah recover add
static bool T2_reg = false;
static bool T2_lock_reg = false;
static ssize_t gpio_mi_t1_set_general(struct device *device, int gpio_type,
				      const char *buf, size_t count);
//T2 modem crah recover add

int gpio_defalut_value[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
enum {
	MI_T1_GPIO_SATELLITE_CTL = 0,
	MI_T1_GPIO_WWAN_WLAN_1,
	MI_T1_GPIO_WWAN_WLAN_2,
	MI_ANT_GPIO_QM13111,
};

static const char *const mi_t1_gpio_names_legacy[] = {
	"gpio-satellite-ctl",
	"gpio-wwan-wlan-1",
	"gpio-wwan-wlan-2",
	"gpio-qm13111",
};

static const char *const mi_t1_gpio_names[] = {
	"mi-t1-gpio-1", "mi-t1-gpio-2",	 "mi-t1-gpio-3",  "mi-t1-gpio-4",
	"mi-t1-gpio-5", "mi-t1-gpio-6",	 "mi-t1-gpio-7",  "mi-t1-gpio-8",
	"mi-t1-gpio-9", "mi-t1-gpio-10", "mi-t1-gpio-11", "mi-t1-gpio-12",
};

struct gpio_data {
	int irq;
	int gpio_num;
	int gpio_status;
	int gpio_offset_num;
};

struct mi_t1_gpio_data {
	struct device *dev;

	struct mutex lock; /* To set/get exported values in sysfs */
	struct gpio_data *data;

	/*char dev related data */
	char *driver_name;
	dev_t mi_t1_major;
	struct class *mi_t1_class;
	struct device chardev;
	bool removed;
	bool clk_enabled;
	bool char_registered;
	bool sysfs_registered;
	struct proc_dir_entry *proc;
	struct cdev cdev;

	struct clk *clk;
	struct notifier_block nb;
	void *notifier;
};

#define MINOR_NUMBER_COUNT 1

#define MI_T1_IOCTL_MAGIC 'y'
#define MI_T1_IOCTL_GPIO_SATELLITE_CTL_SET _IOWR(MI_T1_IOCTL_MAGIC, 0, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_1_SET _IOWR(MI_T1_IOCTL_MAGIC, 1, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_2_SET _IOWR(MI_T1_IOCTL_MAGIC, 2, int)

#define MI_T1_IOCTL_GPIO_SATELLITE_CTL_GET _IOWR(MI_T1_IOCTL_MAGIC, 3, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_1_GET _IOWR(MI_T1_IOCTL_MAGIC, 4, int)
#define MI_T1_IOCTL_GPIO_WWAN_WLAN_2_GET _IOWR(MI_T1_IOCTL_MAGIC, 5, int)

#define MI_T1_IOCTL_GPIO_QM13111_SET _IOWR(MI_T1_IOCTL_MAGIC, 6, int)
#define MI_T1_IOCTL_GPIO_QM13111_GET _IOWR(MI_T1_IOCTL_MAGIC, 7, int)

static int mi_t1_modem_notify(struct notifier_block *nb, unsigned long action,
			      void *data)
{
	struct mi_t1_gpio_data *mi_t1_data =
		container_of(nb, struct mi_t1_gpio_data, nb);
	struct qcom_ssr_notify_data *notify_data = data;
	struct device *dev = mi_t1_data->dev;

	switch (action) {
	case QCOM_SSR_BEFORE_POWERUP:
		dev_info(dev, "received modem starting event\n");
		break;
	case QCOM_SSR_AFTER_POWERUP:
		dev_info(dev, "received modem running event\n");
		if (T2_reg && T2_lock_reg) {
			dev_info(dev, "Exit Modem recover mode\n");
			T2_lock_reg = false;

		} else {
			dev_info(dev, "do not Modem recover mode\n");
		}
		break;
	case QCOM_SSR_BEFORE_SHUTDOWN:
		dev_info(dev, "received modem %s event\n",
			 notify_data && notify_data->crashed ? "crashed" : "stopping");
		if (T2_reg) {
			dev_info(dev, "Enter T2 Modem recover mode\n");
			char t2_default_value[13] = "000000000000";
			for (int i = 0; i < MI_T1_GPIO_COUNT; i++) {
				gpio_mi_t1_set_general(mi_t1_data->dev, i,
						       t2_default_value + i, 1);
			}
			T2_lock_reg = true;
		} else {
			dev_info(dev, "do not Modem recover mode\n");
		}
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		dev_info(dev, "received modem offline event\n");
		break;
	default:
		dev_err(dev, "received unrecognized event %lu\n", action);
		break;
	}
	return NOTIFY_OK;
}

static int mi_t1_modem_config(struct mi_t1_gpio_data *mi_t1_data)
{
	void *notifier;

	dev_info(mi_t1_data->dev, "%s, enter\n", __func__);

	mi_t1_data->nb.notifier_call = mi_t1_modem_notify;

	notifier = qcom_register_ssr_notifier("mpss", &mi_t1_data->nb);
	if (IS_ERR(notifier))
		return PTR_ERR(notifier);

	mi_t1_data->notifier = notifier;

	return 0;
}

static void mi_t1_modem_deconfig(struct mi_t1_gpio_data *mi_t1_data)
{
	struct device *dev = mi_t1_data->dev;
	int ret;

	dev_info(dev, "%s, enter\n", __func__);

	ret = qcom_unregister_ssr_notifier(mi_t1_data->notifier,
					   &mi_t1_data->nb);
	if (ret)
		dev_err(dev, "error %d unregistering notifier", ret);

	mi_t1_data->notifier = NULL;
	memset(&mi_t1_data->nb, 0, sizeof(mi_t1_data->nb));
}

static ssize_t gpio_mi_t1_set_general(struct device *device, int gpio_type,
				      const char *buf, size_t count)
{
	struct mi_t1_gpio_data *data = dev_get_drvdata(device);
	int ret;

	if (!data || gpio_type < 0 || gpio_type >= MI_T1_GPIO_COUNT || !count)
		return -EINVAL;
	if (buf[0] != '0' && buf[0] != '1')
		return -EINVAL;
	mutex_lock(&data->lock);
	if (data->removed) {
		ret = -ENODEV;
		goto out;
	}
	if (data->data[gpio_type].gpio_offset_num == 76)
		udelay(10);
	ret = gpio_direction_output(data->data[gpio_type].gpio_num, buf[0] - '0');
out:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}

static ssize_t gpio_mi_t1_get_general(struct device *device, int gpio_type, char *buf)
{
	struct mi_t1_gpio_data *data = dev_get_drvdata(device);
	int value;
	if (!data || gpio_type < 0 || gpio_type >= MI_T1_GPIO_COUNT)
		return -EINVAL;
	mutex_lock(&data->lock);
	value = data->removed ? -ENODEV : gpio_get_value_cansleep(data->data[gpio_type].gpio_num);
	mutex_unlock(&data->lock);
	return value < 0 ? value : sysfs_emit(buf, "%d\n", value);
}

static void gpio_mi_t1_free_gpio(void *gpio)
{
	gpio_free((unsigned long)gpio);
}

static int gpio_mi_t1_reuqest_gpio(struct platform_device *pdev,
				   struct mi_t1_gpio_data *mi_t1_data,
				   const char *gpio_name, int idx)
{
	int ret = 0;
	int gpio_num = -1;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	static int gpio_offset = 0;

	if (idx == -1) {
		dev_err(dev, "Invalid gpio config name: %s", gpio_name);
		return -EINVAL;
	}

	gpio_num = of_get_named_gpio(np, gpio_name, 0);
	if (gpio_num < 0) {
		dev_err(dev, "Failed to get gpio %s, error: %d.\n", gpio_name,
			gpio_num);
	}

	if (gpio_num < 0)
		return gpio_num;
	ret = gpio_request_one(gpio_num,
				    gpio_defalut_value[idx] ? GPIOF_OUT_INIT_HIGH : GPIOF_OUT_INIT_LOW,
				    gpio_name);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, gpio_mi_t1_free_gpio, (void *)(unsigned long)gpio_num);
	if (ret)
		return ret;
	if (!idx)
		gpio_offset = gpio_num - FIRST_GPIO_ID;
	mi_t1_data->data[idx].gpio_offset_num = gpio_num - gpio_offset;

	dev_info(dev, "mi_t1_gpio %s, #%u. offset=%u\n", gpio_name, gpio_num,
		 mi_t1_data->data[idx].gpio_offset_num);
	mi_t1_data->data[idx].gpio_num = gpio_num;

	return ret;
}

static int mi_t1_gpio_open(struct inode *inode, struct file *file)
{
	struct mi_t1_gpio_data *data = container_of(inode->i_cdev, struct mi_t1_gpio_data, cdev);
	int ret = 0;
	mutex_lock(&data->lock);
	if (data->removed)
		ret = -ENODEV;
	else {
		get_device(&data->chardev);
		file->private_data = data;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static int mi_t1_gpio_release(struct inode *inode, struct file *file)
{
	struct mi_t1_gpio_data *data = file->private_data;
	put_device(&data->chardev);
	return 0;
}

static ssize_t gpio_mi_t1_get_satellite_ctl(struct device *device,
					    struct device_attribute *attr,
					    char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_SATELLITE_CTL, buf);
}

static ssize_t gpio_mi_t1_set_satellite_ctl(struct device *device,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_SATELLITE_CTL, buf,
				      count);
}

static DEVICE_ATTR(satellite_ctl, MI_T1_MODE_MSK, gpio_mi_t1_get_satellite_ctl,
		   gpio_mi_t1_set_satellite_ctl);

static ssize_t gpio_mi_t1_get_wwan_wlan_1(struct device *device,
					  struct device_attribute *attr,
					  char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_WWAN_WLAN_1, buf);
}

static ssize_t gpio_mi_t1_set_wwan_wlan_1(struct device *device,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_WWAN_WLAN_1, buf,
				      count);
}

static DEVICE_ATTR(wwan_wlan_1, MI_T1_MODE_MSK, gpio_mi_t1_get_wwan_wlan_1,
		   gpio_mi_t1_set_wwan_wlan_1);

static ssize_t gpio_mi_t1_get_wwan_wlan_2(struct device *device,
					  struct device_attribute *attr,
					  char *buf)
{
	return gpio_mi_t1_get_general(device, MI_T1_GPIO_WWAN_WLAN_2, buf);
}

static ssize_t gpio_mi_t1_set_wwan_wlan_2(struct device *device,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_T1_GPIO_WWAN_WLAN_2, buf,
				      count);
}

static DEVICE_ATTR(wwan_wlan_2, MI_T1_MODE_MSK, gpio_mi_t1_get_wwan_wlan_2,
		   gpio_mi_t1_set_wwan_wlan_2);

static ssize_t gpio_mi_t1_get_qm13111(struct device *device,
				      struct device_attribute *attr, char *buf)
{
	return gpio_mi_t1_get_general(device, MI_ANT_GPIO_QM13111, buf);
}

static ssize_t gpio_mi_t1_set_qm13111(struct device *device,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	return gpio_mi_t1_set_general(device, MI_ANT_GPIO_QM13111, buf, count);
}

static DEVICE_ATTR(qm13111_ctl, MI_T1_MODE_MSK, gpio_mi_t1_get_qm13111,
		   gpio_mi_t1_set_qm13111);

static struct attribute *attributes[] = { &dev_attr_satellite_ctl.attr,
					  &dev_attr_wwan_wlan_1.attr,
					  &dev_attr_wwan_wlan_2.attr,
					  &dev_attr_qm13111_ctl.attr, NULL };

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static long mi_t1_gpio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mi_t1_gpio_data *data = file->private_data;
	int value, index, ret = 0;
	bool write;
	void __user *user = (void __user *)arg;

	switch (cmd) {
	case MI_T1_IOCTL_GPIO_SATELLITE_CTL_SET: index = 0; write = true; break;
	case MI_T1_IOCTL_GPIO_WWAN_WLAN_1_SET: index = 1; write = true; break;
	case MI_T1_IOCTL_GPIO_WWAN_WLAN_2_SET: index = 2; write = true; break;
	case MI_T1_IOCTL_GPIO_QM13111_SET: index = 3; write = true; break;
	case MI_T1_IOCTL_GPIO_SATELLITE_CTL_GET: index = 0; write = false; break;
	case MI_T1_IOCTL_GPIO_WWAN_WLAN_1_GET: index = 1; write = false; break;
	case MI_T1_IOCTL_GPIO_WWAN_WLAN_2_GET: index = 2; write = false; break;
	case MI_T1_IOCTL_GPIO_QM13111_GET: index = 3; write = false; break;
	default: return -ENOTTY;
	}
	if (index >= MI_T1_GPIO_COUNT)
		return -EINVAL;
	if (write) {
		if (get_user(value, (int __user *)user))
			return -EFAULT;
		if (value != 0 && value != 1)
			return -EINVAL;
	}
	mutex_lock(&data->lock);
	if (data->removed)
		ret = -ENODEV;
	else if (write)
		ret = gpio_direction_output(data->data[index].gpio_num, value);
	else {
		value = gpio_get_value_cansleep(data->data[index].gpio_num);
		ret = value < 0 ? value : put_user(value, (int __user *)user);
	}
	mutex_unlock(&data->lock);
	return ret;
}

static struct file_operations mi_t1_gpio_fops = {
	.owner = THIS_MODULE,
	.open = mi_t1_gpio_open,
	.release = mi_t1_gpio_release,
};

static int set_clk(struct device *dev, int enable)
{
	struct mi_t1_gpio_data *data = dev_get_drvdata(dev);
	int ret = 0;
	mutex_lock(&data->lock);
	if (data->removed)
		ret = -ENODEV;
	else if (enable && !data->clk_enabled) {
		ret = clk_prepare_enable(data->clk);
		if (!ret)
			data->clk_enabled = true;
	} else if (!enable && data->clk_enabled) {
		clk_disable_unprepare(data->clk);
		data->clk_enabled = false;
	}
	mutex_unlock(&data->lock);
	return ret;
}

static ssize_t t1_gpio_write(struct file *file, const char __user *buffer,
		      size_t count, loff_t *ops)
{
	char config[MI_T1_GPIO_COUN_MAX + 1];
	char clk_config[256] = { 0 };
	int i = 0;
	struct mi_t1_gpio_data *mi_t1_data = NULL;
	mi_t1_data = (struct mi_t1_gpio_data *)pde_data(file_inode(file));
	if (T2_lock_reg) {
		pr_info("%s  T2_lock_reg is true do not receive config \n",
			__func__);
		return count;
	}
	if (mi_t1_data == NULL) {
		pr_err("%s  failed.  data = null", __func__);
		return 0;
	}
	if (count <= 0) {
		pr_err("%s  failed. support count: %d : write count:%zu",
		       __func__, MI_T1_GPIO_COUNT, count);
		return -EINVAL;
	}
	if (clk_reg) {
		if (count >= sizeof(clk_config)) {
			return -EINVAL;
		}
		if (copy_from_user(clk_config, buffer, count)) {
			return -EFAULT;
		}
		pr_info("%s  clk_config= %s\n", __func__, clk_config);
		if (strncmp(clk_config, "ON", 2) == 0) {
			pr_info("%s  enter clk enable\n", __func__);
			int ret = set_clk(mi_t1_data->dev, 1);
			return ret ? ret : count;
		} else if (strncmp(clk_config, "OFF", 3) == 0) {
			pr_info("%s enter clk disable\n", __func__);
			int ret = set_clk(mi_t1_data->dev, 0);
			return ret ? ret : count;
		} else {
			printk(KERN_INFO "Invalid input clk set\n");
		}
	}
	memset(config, 0xff, MI_T1_GPIO_COUNT);
	config[MI_T1_GPIO_COUNT] = 0;
	pr_info("%s  count = %zu\n", __func__, count);
	if (copy_from_user((void *)config, (const void __user *)buffer,
			   count > MI_T1_GPIO_COUNT ? MI_T1_GPIO_COUNT : count))
		return -EFAULT;
	pr_info("%s  config[0]= 0x%x\n", __func__, config[0]);
	for (i = 0; i < MI_T1_GPIO_COUNT; i++) {
		if (config[i] != 0xff) {
			gpio_mi_t1_set_general(mi_t1_data->dev, i, config + i,
					       1);
		}
	}
	return count;
}

static int gpio_t1_get_state(struct device *device, int gpio_type, char *buf)
{
	int value = -1;
	int gpio = 0;
	struct mi_t1_gpio_data *mi_t1_data;
	mi_t1_data = dev_get_drvdata(device);
	mutex_lock(&mi_t1_data->lock);
	gpio = mi_t1_data->data[gpio_type].gpio_num;
	if (gpio_is_valid(gpio)) {
		value = gpio_get_value(gpio);
	} else {
		dev_err(device, "mi_t1 %s: unable to get gpio %d!\n", __func__,
			gpio);
	}
	mutex_unlock(&mi_t1_data->lock);
	dev_info(device, "%s, gpio_type [%d] get, value [%d]\n", __func__,
		 gpio_type, value);
	*buf = (char)value;
	return mi_t1_data->data[gpio_type].gpio_offset_num;
}

static int t1_gpio_show(struct seq_file *m, void *v)
{
	int i = 0, gpio = 0;
	char config[4];
	struct mi_t1_gpio_data *mi_t1_data =
		(struct mi_t1_gpio_data *)m->private;
	;
	seq_printf(m, "GPIO cout=%d : \n", MI_T1_GPIO_COUNT);
#if 1
	for (i = 0; i < MI_T1_GPIO_COUNT; i++) {
		gpio = gpio_t1_get_state(mi_t1_data->dev, i, config);
		seq_printf(m, "GPIO-%d state=%d \n", gpio, config[0]);
	}
#endif
	return 0;
}
static int t1_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, t1_gpio_show, pde_data(inode));
}

static struct proc_ops t1_gpio_fops = {
	.proc_open = t1_gpio_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = t1_gpio_write,
};

static void gpio_mi_t1_release_device(struct device *dev)
{
	kfree(container_of(dev, struct mi_t1_gpio_data, chardev));
}

static void gpio_mi_t1_cleanup(struct mi_t1_gpio_data *data)
{
	if (data->notifier)
		mi_t1_modem_deconfig(data);
	proc_remove(data->proc);
	if (data->sysfs_registered)
		sysfs_remove_group(&data->dev->kobj, &attribute_group);
	mutex_lock(&data->lock);
	data->removed = true;
	if (data->clk_enabled) {
		clk_disable_unprepare(data->clk);
		data->clk_enabled = false;
	}
	mutex_unlock(&data->lock);
	if (data->char_registered)
		cdev_device_del(&data->cdev, &data->chardev);
	if (data->mi_t1_class)
		class_destroy(data->mi_t1_class);
	if (data->mi_t1_major)
		unregister_chrdev_region(data->mi_t1_major, MINOR_NUMBER_COUNT);
	device_init_wakeup(data->dev, false);
	put_device(&data->chardev);
}

static int gpio_mi_t1_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mi_t1_gpio_data *data;
	u32 count, first = 0;
	const char *clk_name;
	int ret, i;

	/* The native ABI publishes one global class and proc entry. */
	if (mi_t1_bound)
		return -EBUSY;
	b_legacy_drv = !of_find_property(dev->of_node, "gpio-count", NULL);
	count = ARRAY_SIZE(mi_t1_gpio_names_legacy);
	if (!b_legacy_drv) {
		ret = of_property_read_u32(dev->of_node, "gpio-count", &count);
		if (ret || !count || count > ARRAY_SIZE(mi_t1_gpio_names))
			return -EINVAL;
	}
	MI_T1_GPIO_COUNT = count;
	of_property_read_u32(dev->of_node, "first-gpio", &first);
	if (first > INT_MAX)
		return -EINVAL;
	FIRST_GPIO_ID = first;
	clk_reg = false;
	T2_reg = false;
	T2_lock_reg = false;
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->dev = dev;
	mutex_init(&data->lock);
	device_initialize(&data->chardev);
	data->chardev.release = gpio_mi_t1_release_device;
	platform_set_drvdata(pdev, data);
	data->data = devm_kcalloc(dev, count, sizeof(*data->data), GFP_KERNEL);
	if (!data->data) {
		ret = -ENOMEM;
		goto cleanup;
	}
	for (i = 0; i < count; i++) {
		ret = gpio_mi_t1_reuqest_gpio(pdev, data,
			b_legacy_drv ? mi_t1_gpio_names_legacy[i] : mi_t1_gpio_names[i], i);
		if (ret)
			goto cleanup;
	}
	if (!of_property_read_string(dev->of_node, "clock-names", &clk_name)) {
		data->clk = devm_clk_get(dev, clk_name);
		if (IS_ERR(data->clk)) {
			ret = PTR_ERR(data->clk);
			goto cleanup;
		}
		clk_reg = true;
		T2_reg = true;
	}
	if (b_legacy_drv) {
		ret = sysfs_create_group(&dev->kobj, &attribute_group);
		if (ret)
			goto cleanup;
		data->sysfs_registered = true;
	}
	ret = alloc_chrdev_region(&data->mi_t1_major, 0, 1, "mi_t1_chip");
	if (ret)
		goto cleanup;
	data->mi_t1_class = class_create("mi_t1_chip");
	if (IS_ERR(data->mi_t1_class)) {
		ret = PTR_ERR(data->mi_t1_class);
		data->mi_t1_class = NULL;
		goto cleanup;
	}
	data->chardev.class = data->mi_t1_class;
	data->chardev.parent = dev;
	data->chardev.devt = data->mi_t1_major;
	ret = dev_set_name(&data->chardev, "mi_t1_chip");
	if (ret)
		goto cleanup;
	mi_t1_gpio_fops.unlocked_ioctl = b_legacy_drv ? mi_t1_gpio_ioctl : NULL;
	cdev_init(&data->cdev, &mi_t1_gpio_fops);
	data->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&data->cdev, &data->chardev);
	if (ret)
		goto cleanup;
	data->char_registered = true;
	ret = mi_t1_modem_config(data);
	if (ret)
		goto cleanup;
	data->proc = proc_create_data(MI_T1_GPIO_PROC_NAME, 0664, NULL, &t1_gpio_fops, data);
	if (!data->proc) {
		ret = -ENOMEM;
		goto cleanup;
	}
	mi_t1_bound = true;
	return 0;
cleanup:
	gpio_mi_t1_cleanup(data);
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static void gpio_mi_t1_remove(struct platform_device *pdev)
{
	gpio_mi_t1_cleanup(platform_get_drvdata(pdev));
	mi_t1_bound = false;
}

#if defined(CONFIG_OF)
static const struct of_device_id gpio_mi_t1_of_match[] = {
	{
		.compatible = "mdm,mi_t1_chip",
	},
	{},
};
MODULE_DEVICE_TABLE(of, gpio_mi_t1_of_match);
#endif

#ifdef CONFIG_PM
static int gpio_mi_t1_suspend(struct device *dev)
{
	struct mi_t1_gpio_data *mi_t1_data;

	mi_t1_data = dev_get_drvdata(dev);
	dev_info(dev, "mi_t1_suspend.\n");

	return 0;
}

static int gpio_mi_t1_resume(struct device *dev)
{
	struct mi_t1_gpio_data *mi_t1_data;

	mi_t1_data = dev_get_drvdata(dev);
	dev_info(dev, "mi_t1_resume.\n");

	return 0;
}

static const struct dev_pm_ops gpio_mi_t1_pm_ops = {
	.suspend = gpio_mi_t1_suspend,
	.resume = gpio_mi_t1_resume,
	.freeze = gpio_mi_t1_suspend,
	.restore = gpio_mi_t1_resume,
};
#endif

static struct platform_driver gpio_mi_t1_driver = {
	.driver = {
		.name = "gpio-mi-t1",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(gpio_mi_t1_of_match),
#ifdef CONFIG_PM
		.pm = &gpio_mi_t1_pm_ops,
#endif
	},
	.probe = gpio_mi_t1_probe,
	.remove = gpio_mi_t1_remove,
};

static int __init gpio_mi_t1_driver_init(void)
{
	int err;

	pr_info("%s.\n", __func__);
	err = platform_driver_register(&gpio_mi_t1_driver);
	if (err) {
		pr_err("%s error: %d\n", __func__, err);
	}

	return err;
}

static void __exit gpio_mi_t1_driver_exit(void)
{
	platform_driver_unregister(&gpio_mi_t1_driver);
}

module_init(gpio_mi_t1_driver_init);
module_exit(gpio_mi_t1_driver_exit);

//module_platform_driver(gpio_mi_t1_driver);
MODULE_AUTHOR("huangqianhong@xiaomi.com");
MODULE_DESCRIPTION("Driver for mi_t1 GPIO control");
MODULE_LICENSE("GPL v2");
