/*
 * Copyright (C) 2020 SI-IN, Yun Shi (yun.shi@si-in.com).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define DEBUG
#define LOG_FLAG "sipa_param "

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/firmware.h>
#include "sipa_tuning_if.h"
#include "sipa_parameter.h"

#define LOAD_INTERVAL_TIMES (2000)
#define LOAD_FW_RETRIES (15)

static SIPA_PARAM *sipa_parameters;
static uint32_t sipa_fw_loaded;
static DEFINE_MUTEX(sipa_fw_load_lock);

static bool sipa_param_list_valid(const SIPA_PARAM_FW *param,
				  const SIPA_PARAM_LIST *list, size_t size)
{
	if (list->offset > param->data_size)
		return false;
	if (!list->num)
		return true;
	return list->node_size == size &&
		list->num <= (param->data_size - list->offset) / size;
}

static int sipa_param_validate(const struct firmware *cont, size_t *size)
{
	const SIPA_PARAM_FW *param;
	const SIPA_PARAM_LIST *list;
	const SIPA_CHIP_CFG *chip;
	u32 ch, i;

	if (!cont || cont->size < sizeof(*param) || cont->size > U32_MAX)
		return -EINVAL;
	param = (const SIPA_PARAM_FW *)cont->data;
	if (param->data_size > cont->size - sizeof(*param))
		return -EINVAL;
	*size = sizeof(*param) + param->data_size;
	if (param->version != SIPA_FW_VER ||
	    param->crc != crc32((uint8_t *)&param->version,
				 *size - sizeof(param->crc)))
		return -EINVAL;

	for (ch = 0; ch < SIPA_CHANNEL_NUM; ch++) {
		if (param->ch_en[ch] > 1)
			return -EINVAL;
		if (!param->ch_en[ch])
			continue;
		list = &param->chip_cfg[ch];
		if (!sipa_param_list_valid(param, list, sizeof(*chip)))
			return -EINVAL;
		chip = (const SIPA_CHIP_CFG *)(param->data + list->offset);
		for (i = 0; i < list->num; i++, chip++) {
			if (chip->reg_val_width > 32 || chip->reg_addr_width > 32 ||
			    chip->pvdd_limit.bit_offset >= 32 ||
			    chip->trim_regs.crc_width > 16 ||
			    !sipa_param_list_valid(param, &chip->chip_id_ranges,
						   sizeof(SIPA_VAL_RANGE)) ||
			    !sipa_param_list_valid(param, &chip->init,
						   sizeof(SIPA_REG_COMMON)) ||
			    !sipa_param_list_valid(param, &chip->startup,
						   sizeof(SIPA_REG_PROC)) ||
			    !sipa_param_list_valid(param, &chip->shutdown,
						   sizeof(SIPA_REG_PROC)) ||
			    !sipa_param_list_valid(param, &chip->trim_regs.efuse,
						   sizeof(SIPA_REG_PROC)) ||
			    !sipa_param_list_valid(param, &chip->trim_regs.crc,
						   sizeof(SIPA_REG_PROC)) ||
			    !sipa_param_list_valid(param, &chip->trim_regs.default_set,
						   sizeof(SIPA_REG_PROC)) ||
			    !sipa_param_list_valid(param, &chip->sram_ops.sram,
						   sizeof(SIPA_REG_COMMON)))
				return -EINVAL;
		}
	}
	return 0;
}

static void sipa_fw_load_work_delay(struct work_struct *work)
{
	sipa_dev_t *si_pa = container_of(work, sipa_dev_t, fw_load_work.work);
	const struct firmware *cont = NULL;
	SIPA_PARAM *parameters;
	size_t size;
	int ret;

	mutex_lock(&sipa_fw_load_lock);
	if (si_pa->fw_stopping)
		goto out;
	if (sipa_fw_loaded)
		goto pending;
	ret = request_firmware(&cont, si_pa->fw_name, &si_pa->pdev->dev);
	if (ret) {
		if (++si_pa->fw_load_count < LOAD_FW_RETRIES)
			queue_delayed_work(si_pa->sipa_wq, &si_pa->fw_load_work,
					   msecs_to_jiffies(LOAD_INTERVAL_TIMES));
		else
			dev_err(&si_pa->pdev->dev, "firmware retries exhausted: %d\n", ret);
		goto out;
	}
	ret = sipa_param_validate(cont, &size);
	if (ret) {
		dev_err(&si_pa->pdev->dev, "invalid amplifier firmware: %d\n", ret);
		goto out;
	}
	parameters = kzalloc(sizeof(SIPA_PARAM_WRITEABLE) + size, GFP_KERNEL);
	if (!parameters)
		goto out;
	memcpy(&parameters->fw, cont->data, size);
	sipa_parameters = parameters;
	/* All raw native configuration readers retain this allocation to module exit. */
	smp_store_release(&sipa_fw_loaded, 1);
pending:
	si_pa->fw_load_count = 0;
	mutex_unlock(&sipa_fw_load_lock);
	release_firmware(cont);
	sipa_pending_actions(si_pa);
	return;
out:
	mutex_unlock(&sipa_fw_load_lock);
	release_firmware(cont);
}

void sipa_param_init(sipa_dev_t *si_pa)
{
	INIT_DELAYED_WORK(&si_pa->fw_load_work, sipa_fw_load_work_delay);
}

void sipa_param_load_fw(struct device *dev, char *fwname)
{
	sipa_dev_t *si_pa = dev_get_drvdata(dev);

	if (!si_pa || !fwname)
		return;
	mutex_lock(&sipa_fw_load_lock);
	if (!si_pa->ready || strscpy(si_pa->fw_name, fwname,
					   sizeof(si_pa->fw_name)) < 0)
		goto out;
	si_pa->fw_stopping = false;
	si_pa->fw_load_count = 0;
	mod_delayed_work(si_pa->sipa_wq, &si_pa->fw_load_work,
			 msecs_to_jiffies(LOAD_INTERVAL_TIMES));
out:
	mutex_unlock(&sipa_fw_load_lock);
}

void sipa_param_stop(sipa_dev_t *si_pa)
{
	mutex_lock(&sipa_fw_load_lock);
	si_pa->fw_stopping = true;
	mutex_unlock(&sipa_fw_load_lock);
	cancel_delayed_work_sync(&si_pa->fw_load_work);
}

void sipa_param_release(void)
{
	/* Called only after both drivers and all firmware work have been removed. */
	mutex_lock(&sipa_fw_load_lock);
	sipa_fw_loaded = 0;
	kfree(sipa_parameters);
	sipa_parameters = NULL;
	mutex_unlock(&sipa_fw_load_lock);
}

void *sipa_param_read_chip_cfg(uint32_t ch, uint32_t chip_type,
			       SIPA_CHIP_CFG *cfg)
{
	int i;
	const SIPA_PARAM_LIST *chip_cfg_list = NULL;
	SIPA_CHIP_CFG *chips_cfg = NULL;

	if (!smp_load_acquire(&sipa_fw_loaded)) {
		pr_warn("[ warn][%s] %s: firmware unload \n", LOG_FLAG,
			__func__);
		return NULL;
	}

	if (ch >= SIPA_CHANNEL_NUM) {
		pr_err("[  err][%s] %s: ch(%u) >= SIPA_CHANNEL_NUM(%u) \n",
		       LOG_FLAG, __func__, ch, SIPA_CHANNEL_NUM);
		return NULL;
	}

	if (1 != sipa_parameters->fw.ch_en[ch]) {
		pr_err("[  err][%s] %s: 1 != sipa_parameters.fw.ch_en[%u](%u) \n",
		       LOG_FLAG, __func__, ch, sipa_parameters->fw.ch_en[ch]);
		return NULL;
	}

	if (NULL == cfg) {
		pr_err("[  err][%s] %s: NULL == cfg \n", LOG_FLAG, __func__);
		return NULL;
	}

	chip_cfg_list = &sipa_parameters->fw.chip_cfg[ch];
	if (chip_cfg_list->node_size != sizeof(SIPA_CHIP_CFG)) {
		pr_err("[  err][%s] %s: node_size(%d) != sizeof(SIPA_CHIP_CFG)(%lu)",
		       LOG_FLAG, __func__, chip_cfg_list->node_size,
		       sizeof(SIPA_CHIP_CFG));
		return NULL;
	}

	chips_cfg = (SIPA_CHIP_CFG *)(sipa_parameters->fw.data +
				      chip_cfg_list->offset);
	for (i = 0; i < chip_cfg_list->num; i++) {
		if (chip_type == chips_cfg[i].chip_type) {
			memcpy(cfg, &chips_cfg[i], sizeof(SIPA_CHIP_CFG));
			break;
		}
	}

	if (i >= chip_cfg_list->num) {
		pr_err("[  err][%s] %s: chip_type(%u) mismatch chip_cfg_list->num = %d version = %x\n",
		       LOG_FLAG, __func__, chip_type, chip_cfg_list->num,
		       sipa_parameters->fw.version);
		return NULL;
	}

	return sipa_parameters->fw.data;
}

int sipa_param_read_extra_cfg(uint32_t ch, SIPA_EXTRA_CFG *cfg)
{
	if (!smp_load_acquire(&sipa_fw_loaded)) {
		pr_err("[  err][%s] %s: firmware unload \n", LOG_FLAG,
		       __func__);
		return -EINVAL;
	}

	if (ch >= SIPA_CHANNEL_NUM) {
		pr_err("[  err][%s] %s: ch(%u) >= SIPA_CHANNEL_NUM(%u) \n",
		       LOG_FLAG, __func__, ch, SIPA_CHANNEL_NUM);
		return -EINVAL;
	}

	if (1 != sipa_parameters->fw.ch_en[ch]) {
		pr_err("[  err][%s] %s: 1 != sipa_parameters.fw.ch_en[%u](%u) \n",
		       LOG_FLAG, __func__, ch, sipa_parameters->fw.ch_en[ch]);
		return -EINVAL;
	}

	if (NULL == cfg) {
		pr_err("[  err][%s] %s: NULL == cfg \n", LOG_FLAG, __func__);
		return -EINVAL;
	}

	memcpy(cfg, &sipa_parameters->fw.extra_cfg[ch], sizeof(SIPA_EXTRA_CFG));

	return 0;
}

const SIPA_PARAM *sipa_param_instance(void)
{
	if (!smp_load_acquire(&sipa_fw_loaded))
		return NULL;

	return sipa_parameters;
}

bool sipa_param_is_loaded(void)
{
	if (smp_load_acquire(&sipa_fw_loaded))
		return true;

	return false;
}
