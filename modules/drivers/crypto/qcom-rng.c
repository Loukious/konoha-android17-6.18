// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited
//
// Based on msm-rng.c and downstream driver

#include <crypto/internal/rng.h>
#include <linux/acpi.h>
#include <linux/clk.h>
#include <linux/crypto.h>
#include <linux/hw_random.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* Device specific register offsets */
#define PRNG_DATA_OUT		0x0000
#define PRNG_STATUS		0x0004
#define PRNG_LFSR_CFG		0x0100
#define PRNG_CONFIG		0x0104

/* Device specific register masks and config values */
#define PRNG_LFSR_CFG_MASK	0x0000ffff
#define PRNG_LFSR_CFG_CLOCKS	0x0000dddd
#define PRNG_CONFIG_HW_ENABLE	BIT(1)
#define PRNG_STATUS_DATA_AVAIL	BIT(0)

#define WORD_SZ			4

#define QCOM_TRNG_QUALITY	1024

struct qcom_rng {
	struct mutex lock;
	void __iomem *base;
	struct clk *clk;
	struct hwrng hwrng;
	const struct qcom_rng_of_data *of_data;
	struct rng_alg alg;
	struct kref ref;
	atomic_t alg_ref;
	bool stopping;
};

struct qcom_rng_ctx {
	struct qcom_rng *rng;
};

struct qcom_rng_of_data {
	bool skip_init;
	bool hwrng_support;
};

static void qcom_rng_release(struct kref *ref)
{
	struct qcom_rng *rng = container_of(ref, struct qcom_rng, ref);

	kfree(rng);
}

static void qcom_rng_alg_destroy(struct crypto_alg *alg)
{
	struct qcom_rng *rng = container_of(alg, struct qcom_rng, alg.base);

	/* Registration may drop this reference itself on a late failure. */
	if (atomic_xchg(&rng->alg_ref, 0))
		kref_put(&rng->ref, qcom_rng_release);
}

static int qcom_rng_read(struct qcom_rng *rng, u8 *data, unsigned int max,
			 bool wait)
{
	unsigned int currsize = 0;
	u32 val;
	int ret;

	/* read random data from hardware */
	while (currsize < max) {
		if (!wait) {
			val = readl(rng->base + PRNG_STATUS);
			if (!(val & PRNG_STATUS_DATA_AVAIL))
				break;
			ret = 0;
		} else {
			ret = readl_poll_timeout(rng->base + PRNG_STATUS, val,
						 val & PRNG_STATUS_DATA_AVAIL,
						 200, 10000);
		}
		if (ret)
			return ret;

		val = readl_relaxed(rng->base + PRNG_DATA_OUT);
		if (!val)
			return -EINVAL;

		if ((max - currsize) >= WORD_SZ) {
			memcpy(data, &val, WORD_SZ);
			data += WORD_SZ;
			currsize += WORD_SZ;
		} else {
			/* copy only remaining bytes */
			memcpy(data, &val, max - currsize);
			currsize = max;
		}
	}

	return currsize;
}

static int qcom_rng_get(struct qcom_rng *rng, void *data, size_t max,
			bool wait)
{
	int ret;

	mutex_lock(&rng->lock);
	if (rng->stopping) {
		ret = -ENODEV;
		goto out;
	}
	if (!max) {
		ret = 0;
		goto out;
	}
	ret = clk_prepare_enable(rng->clk);
	if (ret)
		goto out;

	ret = qcom_rng_read(rng, data, min_t(size_t, max, INT_MAX), wait);
	clk_disable_unprepare(rng->clk);
out:
	mutex_unlock(&rng->lock);
	return ret;
}

static int qcom_rng_generate(struct crypto_rng *tfm,
			     const u8 *src, unsigned int slen,
			     u8 *dstn, unsigned int dlen)
{
	struct qcom_rng_ctx *ctx = crypto_rng_ctx(tfm);
	int ret;

	if (dlen > INT_MAX)
		return -EOVERFLOW;
	ret = qcom_rng_get(ctx->rng, dstn, dlen, true);
	return ret < 0 ? ret : 0;
}

static int qcom_rng_seed(struct crypto_rng *tfm, const u8 *seed,
			 unsigned int slen)
{
	return 0;
}

static int qcom_hwrng_read(struct hwrng *hwrng, void *data, size_t max, bool wait)
{
	struct qcom_rng *qrng = container_of(hwrng, struct qcom_rng, hwrng);

	return qcom_rng_get(qrng, data, max, wait);
}

static int qcom_rng_enable(struct qcom_rng *rng)
{
	u32 val;
	int ret;

	ret = clk_prepare_enable(rng->clk);
	if (ret)
		return ret;

	/* Enable PRNG only if it is not already enabled */
	val = readl_relaxed(rng->base + PRNG_CONFIG);
	if (val & PRNG_CONFIG_HW_ENABLE)
		goto already_enabled;

	val = readl_relaxed(rng->base + PRNG_LFSR_CFG);
	val &= ~PRNG_LFSR_CFG_MASK;
	val |= PRNG_LFSR_CFG_CLOCKS;
	writel(val, rng->base + PRNG_LFSR_CFG);

	val = readl_relaxed(rng->base + PRNG_CONFIG);
	val |= PRNG_CONFIG_HW_ENABLE;
	writel(val, rng->base + PRNG_CONFIG);

already_enabled:
	clk_disable_unprepare(rng->clk);

	return 0;
}

static int qcom_rng_init(struct crypto_tfm *tfm)
{
	struct qcom_rng_ctx *ctx = crypto_tfm_ctx(tfm);
	struct qcom_rng *rng;
	int ret = 0;

	/* The transform holds the algorithm, which retains the controller. */
	rng = container_of(tfm->__crt_alg, struct qcom_rng, alg.base);
	ctx->rng = rng;
	mutex_lock(&rng->lock);
	if (rng->stopping)
		ret = -ENODEV;
	else if (!rng->of_data->skip_init)
		ret = qcom_rng_enable(rng);
	mutex_unlock(&rng->lock);
	return ret;
}

static const struct rng_alg qcom_rng_alg = {
	.generate	= qcom_rng_generate,
	.seed		= qcom_rng_seed,
	.seedsize	= 0,
	.base		= {
		.cra_name		= "stdrng",
		.cra_driver_name	= "qcom-rng",
		.cra_flags		= CRYPTO_ALG_TYPE_RNG,
		.cra_priority		= 300,
		.cra_ctxsize		= sizeof(struct qcom_rng_ctx),
		.cra_module		= THIS_MODULE,
		.cra_init		= qcom_rng_init,
		.cra_destroy		= qcom_rng_alg_destroy,
	}
};

static void qcom_rng_stop(struct qcom_rng *rng)
{
	/* Drain readers before devres can unmap the registers or put the clock. */
	mutex_lock(&rng->lock);
	rng->stopping = true;
	mutex_unlock(&rng->lock);
}

static int qcom_rng_probe(struct platform_device *pdev)
{
	struct qcom_rng *rng;
	int ret;

	rng = kzalloc(sizeof(*rng), GFP_KERNEL);
	if (!rng)
		return -ENOMEM;
	kref_init(&rng->ref);
	mutex_init(&rng->lock);
	rng->of_data = device_get_match_data(&pdev->dev);
	if (!rng->of_data) {
		ret = -ENODEV;
		goto put_rng;
	}
	rng->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rng->base)) {
		ret = PTR_ERR(rng->base);
		goto put_rng;
	}
	rng->clk = devm_clk_get_optional(&pdev->dev, "core");
	if (IS_ERR(rng->clk)) {
		ret = PTR_ERR(rng->clk);
		goto put_rng;
	}

	if (rng->of_data->hwrng_support) {
		rng->hwrng.name = "qcom_hwrng";
		rng->hwrng.read = qcom_hwrng_read;
		rng->hwrng.quality = QCOM_TRNG_QUALITY;
		ret = hwrng_register(&rng->hwrng);
		if (ret)
			goto put_rng;
	}

	rng->alg = qcom_rng_alg;
	kref_get(&rng->ref);
	atomic_set(&rng->alg_ref, 1);
	ret = crypto_register_rng(&rng->alg);
	if (ret) {
		qcom_rng_stop(rng);
		qcom_rng_alg_destroy(&rng->alg.base);
		if (rng->of_data->hwrng_support)
			hwrng_unregister(&rng->hwrng);
		goto put_rng;
	}
	platform_set_drvdata(pdev, rng);
	return 0;

put_rng:
	kref_put(&rng->ref, qcom_rng_release);
	return ret;
}

static void qcom_rng_remove(struct platform_device *pdev)
{
	struct qcom_rng *rng = platform_get_drvdata(pdev);

	qcom_rng_stop(rng);
	crypto_unregister_rng(&rng->alg);
	if (rng->of_data->hwrng_support)
		hwrng_unregister(&rng->hwrng);
	platform_set_drvdata(pdev, NULL);
	kref_put(&rng->ref, qcom_rng_release);
}

static const struct qcom_rng_of_data qcom_prng_of_data = {
	.skip_init = false,
	.hwrng_support = false,
};

static const struct qcom_rng_of_data qcom_prng_ee_of_data = {
	.skip_init = true,
	.hwrng_support = false,
};

static const struct qcom_rng_of_data qcom_trng_of_data = {
	.skip_init = true,
	.hwrng_support = true,
};

static const struct acpi_device_id __maybe_unused qcom_rng_acpi_match[] = {
	{ .id = "QCOM8160", .driver_data = (kernel_ulong_t)&qcom_prng_ee_of_data },
	{}
};
MODULE_DEVICE_TABLE(acpi, qcom_rng_acpi_match);

static const struct of_device_id __maybe_unused qcom_rng_of_match[] = {
	{ .compatible = "qcom,prng", .data = &qcom_prng_of_data },
	{ .compatible = "qcom,prng-ee", .data = &qcom_prng_ee_of_data },
	{ .compatible = "qcom,trng", .data = &qcom_trng_of_data },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_rng_of_match);

static struct platform_driver qcom_rng_driver = {
	.probe = qcom_rng_probe,
	.remove =  qcom_rng_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_match_ptr(qcom_rng_of_match),
		.acpi_match_table = ACPI_PTR(qcom_rng_acpi_match),
	}
};
module_platform_driver(qcom_rng_driver);

MODULE_ALIAS("platform:" KBUILD_MODNAME);
MODULE_DESCRIPTION("Qualcomm random number generator driver");
MODULE_LICENSE("GPL v2");
