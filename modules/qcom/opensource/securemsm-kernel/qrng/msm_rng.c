// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2011-2013, 2015, 2017-2021 The Linux Foundation. All rights
 * reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/hw_random.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/qrng.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/crypto.h>
#include <crypto/internal/rng.h>
#include <linux/interconnect.h>
#include <linux/sched/signal.h>

#define DRIVER_NAME "msm_rng"

/* Device specific register offsets */
#define PRNG_DATA_OUT_OFFSET    0x0000
#define PRNG_STATUS_OFFSET	0x0004
#define PRNG_LFSR_CFG_OFFSET	0x0100
#define PRNG_CONFIG_OFFSET	0x0104

/* Device specific register masks and config values */
#define PRNG_LFSR_CFG_MASK	0xFFFF0000
#define PRNG_LFSR_CFG_CLOCKS	0x0000DDDD
#define PRNG_CONFIG_MASK	0xFFFFFFFD
#define PRNG_HW_ENABLE		0x00000002

#define MAX_HW_FIFO_DEPTH 16                     /* FIFO is 16 words deep */
#define MAX_HW_FIFO_SIZE (MAX_HW_FIFO_DEPTH * 4) /* FIFO is 32 bits wide  */

#define RETRY_MAX_CNT		5	/* max retry times to read register */
#define RETRY_DELAY_INTERVAL	440	/* retry delay interval in us */

struct msm_rng_device {
	struct platform_device *pdev;
	void __iomem *base;
	struct clk *prng_clk;
	struct mutex rng_lock;
	struct icc_path *icc_path;
	bool suspended;
	bool stopping;
	struct hwrng hwrng;
	struct device chardev;
	struct cdev cdev;
	struct class *class;
};

static struct msm_rng_device *msm_rng_dev_cached;
static DEFINE_MUTEX(cached_rng_lock);
static bool msm_rng_ready;

static void msm_rng_release_device(struct device *dev)
{
	struct msm_rng_device *rng = container_of(dev, struct msm_rng_device,
						chardev);

	kfree_sensitive(rng);
}

static int msm_rng_open(struct inode *inode, struct file *filp)
{
	struct msm_rng_device *rng = container_of(inode->i_cdev,
						struct msm_rng_device, cdev);
	int ret = -ENODEV;

	mutex_lock(&cached_rng_lock);
	/* An open racing unbind must never attach to a replacement device. */
	if (msm_rng_ready && msm_rng_dev_cached == rng) {
		get_device(&rng->chardev);
		filp->private_data = rng;
		ret = 0;
	}
	mutex_unlock(&cached_rng_lock);
	return ret;
}

static int msm_rng_release_file(struct inode *inode, struct file *filp)
{
	struct msm_rng_device *rng = filp->private_data;

	put_device(&rng->chardev);
	return 0;
}

static long msm_rng_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	struct msm_rng_device *rng = filp->private_data;
	long ret = 0;

	mutex_lock(&rng->rng_lock);
	if (rng->stopping)
		ret = -ENODEV;
	else if (rng->suspended)
		ret = -EAGAIN;
	else if (cmd == QRNG_IOCTL_RESET_BUS_BANDWIDTH)
		ret = icc_set_bw(rng->icc_path, 0, 0);
	/* Preserve the native success result for an unsupported ioctl. */
	mutex_unlock(&rng->rng_lock);
	return ret;
}

static int msm_rng_direct_read(struct msm_rng_device *rng, void *data,
			       size_t max, bool wait)
{
	size_t currsize = 0;
	u8 *out = data;
	u32 val;
	int ret = 0, vote_ret;
	int failed = 0;

	max = min_t(size_t, max, INT_MAX);
	mutex_lock(&rng->rng_lock);
	if (rng->stopping) {
		ret = -ENODEV;
		goto unlock;
	}
	if (rng->suspended) {
		ret = -EAGAIN;
		goto unlock;
	}
	/* Native legacy reads return complete words only. */
	if (max < sizeof(val))
		goto unlock;
	ret = icc_set_bw(rng->icc_path, 0, 300000);
	if (ret)
		goto unlock;
	ret = clk_prepare_enable(rng->prng_clk);
	if (ret)
		goto drop_vote;

	while (max - currsize >= sizeof(val)) {
		if (!(readl_relaxed(rng->base + PRNG_STATUS_OFFSET) & BIT(0))) {
			/* Keep the native cumulative retry budget for this read. */
			if (!wait || failed++ == RETRY_MAX_CNT)
				break;
			udelay(RETRY_DELAY_INTERVAL);
			continue;
		}
		val = readl_relaxed(rng->base + PRNG_DATA_OUT_OFFSET);
		memcpy(out + currsize, &val, sizeof(val));
		currsize += sizeof(val);
	}
	clk_disable_unprepare(rng->prng_clk);
drop_vote:
	vote_ret = icc_set_bw(rng->icc_path, 0, 0);
	if (!ret)
		ret = vote_ret;
unlock:
	mutex_unlock(&rng->rng_lock);
	return ret ? ret : currsize;
}

static int msm_rng_read(struct hwrng *hwrng, void *data, size_t max, bool wait)
{
	struct msm_rng_device *rng = container_of(hwrng, struct msm_rng_device,
						hwrng);

	return msm_rng_direct_read(rng, data, max, wait);
}

static int msm_rng_enable_hw(struct msm_rng_device *msm_rng_dev)
{
	unsigned long val = 0;
	unsigned long reg_val = 0;
	int ret = 0;

	if (msm_rng_dev->icc_path) {
		ret = icc_set_bw(msm_rng_dev->icc_path, 0, 30000);
		if (ret)
			return ret;
	}
	/* Enable the PRNG CLK */
	if (msm_rng_dev->prng_clk) {
		ret = clk_prepare_enable(msm_rng_dev->prng_clk);
		if (ret) {
			dev_err(&(msm_rng_dev->pdev)->dev,
				"failed to enable clock in probe\n");
			goto drop_vote;
		}
	}

	/* Enable PRNG h/w only if it is NOT ON */
	val = readl_relaxed(msm_rng_dev->base + PRNG_CONFIG_OFFSET) &
					PRNG_HW_ENABLE;
	/* PRNG H/W is not ON */
	if (val != PRNG_HW_ENABLE) {
		val = readl_relaxed(msm_rng_dev->base + PRNG_LFSR_CFG_OFFSET);
		val &= PRNG_LFSR_CFG_MASK;
		val |= PRNG_LFSR_CFG_CLOCKS;
		writel_relaxed(val, msm_rng_dev->base + PRNG_LFSR_CFG_OFFSET);

		/* The PRNG CONFIG register should be first written */
		mb();

		reg_val = readl_relaxed(msm_rng_dev->base + PRNG_CONFIG_OFFSET)
						& PRNG_CONFIG_MASK;
		reg_val |= PRNG_HW_ENABLE;
		writel_relaxed(reg_val, msm_rng_dev->base + PRNG_CONFIG_OFFSET);

		/* The PRNG clk should be disabled only after we enable the
		 * PRNG h/w by writing to the PRNG CONFIG register.
		 */
		mb();
	}
	if (msm_rng_dev->prng_clk)
		clk_disable_unprepare(msm_rng_dev->prng_clk);

drop_vote:
	if (msm_rng_dev->icc_path) {
		int vote_ret = icc_set_bw(msm_rng_dev->icc_path, 0, 0);

		if (!ret)
			ret = vote_ret;
	}

	return ret;
}

static const struct file_operations msm_rng_fops = {
	.owner = THIS_MODULE,
	.open = msm_rng_open,
	.release = msm_rng_release_file,
	.unlocked_ioctl = msm_rng_ioctl,
};

static void msm_rng_stop(struct msm_rng_device *rng)
{
	mutex_lock(&rng->rng_lock);
	rng->stopping = true;
	mutex_unlock(&rng->rng_lock);
}

static int msm_rng_probe(struct platform_device *pdev)
{
	struct msm_rng_device *rng;
	struct resource *res;
	int ret;

	mutex_lock(&cached_rng_lock);
	if (msm_rng_dev_cached) {
		ret = -EBUSY;
		goto unlock;
	}
	rng = kzalloc(sizeof(*rng), GFP_KERNEL);
	if (!rng) {
		ret = -ENOMEM;
		goto unlock;
	}
	device_initialize(&rng->chardev);
	rng->chardev.release = msm_rng_release_device;
	rng->chardev.parent = &pdev->dev;
	rng->chardev.devt = MKDEV(QRNG_IOC_MAGIC, 0);
	rng->pdev = pdev;
	mutex_init(&rng->rng_lock);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -EFAULT;
		goto put_rng;
	}
	rng->base = ioremap(res->start, resource_size(res));
	if (!rng->base) {
		ret = -ENOMEM;
		goto put_rng;
	}
	if (pdev->dev.of_node &&
	    !of_property_read_bool(pdev->dev.of_node, "qcom,no-clock-support"))
		rng->prng_clk = clk_get(&pdev->dev, "km_clk_src");
	if (IS_ERR(rng->prng_clk)) {
		ret = PTR_ERR(rng->prng_clk);
		goto unmap;
	}
	if (pdev->dev.of_node)
		rng->icc_path = of_icc_get(&pdev->dev, "data_path");
	if (IS_ERR(rng->icc_path)) {
		ret = PTR_ERR(rng->icc_path);
		goto put_clk;
	}
	if (!of_property_read_bool(pdev->dev.of_node, "qcom,no-qrng-config")) {
		ret = msm_rng_enable_hw(rng);
		if (ret)
			goto put_icc;
	}

	rng->hwrng.name = DRIVER_NAME;
	rng->hwrng.read = msm_rng_read;
	rng->hwrng.quality = 1024;
	ret = hwrng_register(&rng->hwrng);
	if (ret)
		goto put_icc;
	ret = register_chrdev_region(rng->chardev.devt, 1, DRIVER_NAME);
	if (ret)
		goto unregister_rng;
	rng->class = class_create("msm-rng");
	if (IS_ERR(rng->class)) {
		ret = PTR_ERR(rng->class);
		goto unregister_chrdev;
	}
	rng->chardev.class = rng->class;
	ret = dev_set_name(&rng->chardev, "msm-rng");
	if (ret)
		goto destroy_class;
	cdev_init(&rng->cdev, &msm_rng_fops);
	rng->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&rng->cdev, &rng->chardev);
	if (ret)
		goto destroy_class;

	platform_set_drvdata(pdev, rng);
	msm_rng_dev_cached = rng;
	mutex_unlock(&cached_rng_lock);
	return 0;

destroy_class:
	class_destroy(rng->class);
unregister_chrdev:
	unregister_chrdev_region(rng->chardev.devt, 1);
unregister_rng:
	msm_rng_stop(rng);
	hwrng_unregister(&rng->hwrng);
put_icc:
	icc_put(rng->icc_path);
put_clk:
	clk_put(rng->prng_clk);
unmap:
	iounmap(rng->base);
put_rng:
	put_device(&rng->chardev);
unlock:
	mutex_unlock(&cached_rng_lock);
	return ret;
}

static void msm_rng_remove(struct platform_device *pdev)
{
	struct msm_rng_device *rng = platform_get_drvdata(pdev);

	mutex_lock(&cached_rng_lock);
	msm_rng_dev_cached = NULL;
	msm_rng_stop(rng);
	cdev_device_del(&rng->cdev, &rng->chardev);
	class_destroy(rng->class);
	unregister_chrdev_region(rng->chardev.devt, 1);
	hwrng_unregister(&rng->hwrng);
	icc_put(rng->icc_path);
	clk_put(rng->prng_clk);
	iounmap(rng->base);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&cached_rng_lock);
	put_device(&rng->chardev);
}

static int qrng_get_random(struct crypto_rng *tfm, const u8 *src,
			   unsigned int slen, u8 *rdata, unsigned int dlen)
{
	int ret;

	if (!rdata)
		return -EINVAL;
	if (dlen > INT_MAX)
		return -EOVERFLOW;
	if (signal_pending(current) || mutex_lock_interruptible(&cached_rng_lock))
		return -ERESTARTSYS;
	if (!msm_rng_dev_cached) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = msm_rng_direct_read(msm_rng_dev_cached, rdata, dlen, true);
	/* Native crypto requests require an exact, whole-word result. */
	if (ret == -EAGAIN || (ret >= 0 && ret != dlen))
		ret = -EFAULT;
	else if (ret >= 0)
		ret = 0;
unlock:
	mutex_unlock(&cached_rng_lock);
	return ret;
}

static int qrng_reset(struct crypto_rng *tfm, const u8 *seed, unsigned int slen)
{
	return 0;
}

static int msm_rng_suspend(struct device *dev)
{
	struct msm_rng_device *msm_rng_dev = dev_get_drvdata(dev);

	mutex_lock(&msm_rng_dev->rng_lock);
	msm_rng_dev->suspended = true;
	mutex_unlock(&msm_rng_dev->rng_lock);
	pr_info("%s: Suspended MSM RNG driver\n", __func__);

	return 0;
}

static int msm_rng_resume(struct device *dev)
{
	struct msm_rng_device *msm_rng_dev = dev_get_drvdata(dev);

	mutex_lock(&msm_rng_dev->rng_lock);
	msm_rng_dev->suspended = false;
	mutex_unlock(&msm_rng_dev->rng_lock);
	pr_info("%s: Resumed MSM RNG driver\n", __func__);

	return 0;
}

static struct rng_alg rng_algs[] = { {
	.generate	= qrng_get_random,
	.seed		= qrng_reset,
	.seedsize	= 0,
	.base		= {
		.cra_name		= "qrng",
		.cra_driver_name	= "fips_hw_qrng",
		.cra_priority		= 300,
		.cra_ctxsize		= 0,
		.cra_module		= THIS_MODULE,
	}
} };

static const struct of_device_id qrng_match[] = {
	{.compatible = "qcom,msm-rng"},
	{},
};

static const struct dev_pm_ops msm_rng_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_rng_suspend, msm_rng_resume)
};

static struct platform_driver rng_driver = {
	.probe      = msm_rng_probe,
	.remove     = msm_rng_remove,
	.driver     = {
		.name   = DRIVER_NAME,
		.of_match_table = qrng_match,
		.pm = &msm_rng_pm_ops,
	},
};

static int __init msm_rng_init(void)
{
	int ret;

	ret = platform_driver_register(&rng_driver);
	if (ret) {
		pr_err("%s: platform_driver_register error:%d\n",
			__func__, ret);
		goto err_exit;
	}
	ret = crypto_register_rngs(rng_algs, ARRAY_SIZE(rng_algs));
	if (ret) {
		pr_err("%s: crypto_register_algs error:%d\n",
			__func__, ret);
		platform_driver_unregister(&rng_driver);
		goto err_exit;
	}

	/* No later fallible initialization may leave a live file behind. */
	mutex_lock(&cached_rng_lock);
	msm_rng_ready = true;
	mutex_unlock(&cached_rng_lock);
err_exit:
	return ret;
}

module_init(msm_rng_init);

static void __exit msm_rng_exit(void)
{
	mutex_lock(&cached_rng_lock);
	msm_rng_ready = false;
	mutex_unlock(&cached_rng_lock);
	crypto_unregister_rngs(rng_algs, ARRAY_SIZE(rng_algs));
	platform_driver_unregister(&rng_driver);
}

module_exit(msm_rng_exit);

MODULE_DESCRIPTION("QTI MSM Random Number Driver");
MODULE_LICENSE("GPL v2");
