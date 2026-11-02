// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 *
 ***************************************************************************/
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 ***************************************************************************/

#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include "common.h"

/*
 * Power management of the eSE
 * eSE and NFCC both are powered using VEN gpio,
 * VEN HIGH - eSE and NFCC both are powered on
 * VEN LOW - eSE and NFCC both are power down
 */
int nfc_ese_pwr(struct nfc_dev *nfc_dev, unsigned long arg)
{
	int ret = 0;

	if (arg == ESE_POWER_ON) {
		/*
		 * Let's store the NFC VEN pin state
		 * will check stored value in case of eSE power off request,
		 * to find out if NFC MW also sent request to set VEN HIGH
		 * VEN state will remain HIGH if NFC is enabled otherwise
		 * it will be set as LOW
		 */
		nfc_dev->nfc_ven_enabled = gpio_get_value(nfc_dev->configs.gpio.ven);
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("eSE HAL service setting ven HIGH\n");
			gpio_set_ven(nfc_dev, 1);
		} else {
			pr_debug("ven already HIGH\n");
		}
		nfc_dev->is_ese_session_active = true;
	} else if (arg == ESE_POWER_OFF) {
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("NFC not enabled, disabling ven\n");
			gpio_set_ven(nfc_dev, 0);
		} else {
			pr_debug("keep ven high as NFC is enabled\n");
		}
		nfc_dev->is_ese_session_active = false;
#ifdef NFC_SECURE_PERIPHERAL_ENABLED
		if (nfc_dev->configs.CNSS_NFC_HW_SECURE_ENABLE == true) {
			if(chk_eSE_pwr_off)
			  up(&sem_eSE_pwr_off);
		}
#endif
	} else if (arg == ESE_POWER_STATE) {
		/* get VEN gpio state for eSE, as eSE also enabled through same GPIO */
		ret = gpio_get_value(nfc_dev->configs.gpio.ven);
	} else {
		pr_err("%s bad arg %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

/* Caller holds cold_reset.lock and serializes physical reads with read_mutex. */
static int read_cold_reset_rsp_locked(struct nfc_dev *nfc_dev,
				     const u8 *header, size_t len)
{
	struct cold_reset *reset = &nfc_dev->cold_reset;
	u8 response[COLD_RESET_RSP_LEN];
	int ret;

	if (!reset->cmd_buf || reset->rsp_len != sizeof(response))
		return -EINVAL;
	if (header) {
		if (len < NCI_HDR_LEN)
			return -EPROTO;
		memcpy(response, header, NCI_HDR_LEN);
	} else {
		ret = i2c_master_recv(nfc_dev->i2c_dev.client, response, NCI_HDR_LEN);
		if (ret != NCI_HDR_LEN)
			return ret < 0 ? ret : -EIO;
	}
	if (response[0] != PROP_NCI_RSP_GID ||
	    response[1] != reset->cmd_buf[1] || response[2] != 1)
		return -EPROTO;
	if (header && len >= sizeof(response)) {
		response[NCI_PAYLOAD_IDX] = header[NCI_PAYLOAD_IDX];
	} else {
		ret = i2c_master_recv(nfc_dev->i2c_dev.client,
				     &response[NCI_PAYLOAD_IDX], 1);
		if (ret != 1)
			return ret < 0 ? ret : -EIO;
	}
	return response[NCI_PAYLOAD_IDX];
}

/* Consume only the response of the active eSE transaction. */
bool nfc_cold_reset_response(struct nfc_dev *nfc_dev, const u8 *data, size_t len)
{
	struct cold_reset *reset = &nfc_dev->cold_reset;
	bool consumed = false;

	mutex_lock(&reset->lock);
	if (len >= NCI_HDR_LEN && reset->rsp_pending && reset->cmd_buf &&
	    data[0] == PROP_NCI_RSP_GID && data[1] == reset->cmd_buf[1]) {
		reset->status = read_cold_reset_rsp_locked(nfc_dev, data, len);
		WRITE_ONCE(reset->rsp_pending, false);
		consumed = true;
	}
	mutex_unlock(&reset->lock);
	if (consumed)
		wake_up_interruptible(&reset->read_wq);
	return consumed;
}

int ese_cold_reset_ioctl(struct nfc_dev *nfc_dev, unsigned long arg)
{
	struct cold_reset *reset = &nfc_dev->cold_reset;
	struct ese_cold_reset_arg request;
	struct ese_ioctl_arg envelope;
	u8 *command = NULL;
	bool nfc_enabled;
	int ret;

	if (copy_from_user(&envelope, (void __user *)arg, sizeof(envelope)))
		return -EFAULT;
	if (envelope.type != ESE_ARG_TYPE_COLD_RESET ||
	    envelope.buf_size < sizeof(request) || envelope.buf_size > PAGE_SIZE)
		return -EINVAL;
	ret = copy_struct_from_user(&request, sizeof(request),
				   u64_to_user_ptr(envelope.buf), envelope.buf_size);
	if (ret)
		return ret;
	if (request.rfu || request.sub_cmd > ESE_COLD_RESET_PROTECT_DIS ||
	    (request.src != ESE_COLD_RESET_ORIGIN_ESE &&
	     request.src != ESE_COLD_RESET_ORIGIN_NFC &&
	     request.src != ESE_COLD_RESET_ORIGIN_OTHER))
		return -EINVAL;
	mutex_lock(&nfc_dev->write_mutex);
	mutex_lock(&reset->lock);
	if (reset->is_crp_en && request.src != reset->last_src_ese_prot) {
		ret = -EACCES;
		goto unlock;
	}
	if (request.sub_cmd == ESE_COLD_RESET_PROTECT_EN && reset->is_crp_en) {
		ret = 0;
		goto unlock;
	}
	ret = validate_nfc_state_nci(nfc_dev);
	if (ret)
		goto unlock;
	command = kzalloc(COLD_RESET_PROT_CMD_LEN, GFP_DMA | GFP_KERNEL);
	if (!command) {
		ret = -ENOMEM;
		goto unlock;
	}
	command[0] = PROP_NCI_CMD_GID;
	if (request.sub_cmd == ESE_COLD_RESET_DO) {
		command[1] = COLD_RESET_OID;
		command[2] = COLD_RESET_CMD_PL_LEN;
		reset->cmd_len = COLD_RESET_CMD_LEN;
	} else {
		command[1] = COLD_RESET_PROT_OID;
		command[2] = COLD_RESET_PROT_CMD_PL_LEN;
		command[3] = request.sub_cmd == ESE_COLD_RESET_PROTECT_EN;
		reset->cmd_len = COLD_RESET_PROT_CMD_LEN;
	}
	reset->cmd_buf = command;
	reset->rsp_len = COLD_RESET_RSP_LEN;
	reset->status = -EINPROGRESS;
	nfc_enabled = READ_ONCE(reset->is_nfc_enabled);
	/* A response may arrive as soon as the transport submits the command. */
	WRITE_ONCE(reset->rsp_pending, true);
	mutex_unlock(&reset->lock);
	ret = nfc_dev->nfc_write(nfc_dev, command, reset->cmd_len, MAX_RETRY_COUNT);
	if (ret != reset->cmd_len) {
		ret = ret < 0 ? ret : -EIO;
		goto finish;
	}
	/*
	 * A blocked HAL reader can own read_mutex even while NFC is disabled.
	 * Let that reader consume the response without waiting for its lock.
	 */
	if (!nfc_enabled && mutex_trylock(&nfc_dev->read_mutex)) {
		if (!READ_ONCE(reset->rsp_pending)) {
			ret = 0;
		} else {
			ret = is_nfc_data_available_for_read(nfc_dev);
			if (!ret)
				ret = -ETIMEDOUT;
		}
		if (ret > 0 && !READ_ONCE(nfc_dev->removing)) {
			mutex_lock(&reset->lock);
			reset->status = read_cold_reset_rsp_locked(nfc_dev, NULL, 0);
			WRITE_ONCE(reset->rsp_pending, false);
			mutex_unlock(&reset->lock);
			ret = 0;
		} else if (READ_ONCE(nfc_dev->removing)) {
			ret = -ENODEV;
		}
		nfc_dev->nfc_disable_intr(nfc_dev);
		mutex_unlock(&nfc_dev->read_mutex);
	} else {
		ret = wait_event_interruptible_timeout(reset->read_wq,
			!READ_ONCE(reset->rsp_pending) || READ_ONCE(nfc_dev->removing),
			msecs_to_jiffies(NCI_CMD_RSP_TIMEOUT_MS));
		if (ret <= 0) {
			ret = ret ? ret : -ETIMEDOUT;
			goto finish;
		}
		ret = READ_ONCE(nfc_dev->removing) ? -ENODEV : 0;
	}
finish:
	mutex_lock(&reset->lock);
	if (!ret)
		ret = reset->status;
	WRITE_ONCE(reset->rsp_pending, false);
	reset->cmd_buf = NULL;
	if (!ret) {
		if (request.sub_cmd == ESE_COLD_RESET_PROTECT_EN) {
			reset->is_crp_en = true;
			reset->last_src_ese_prot = request.src;
		} else if (request.sub_cmd == ESE_COLD_RESET_PROTECT_DIS) {
			reset->is_crp_en = false;
			reset->last_src_ese_prot = ESE_COLD_RESET_ORIGIN_NONE;
		}
	}
unlock:
	mutex_unlock(&reset->lock);
	kfree(command);
	mutex_unlock(&nfc_dev->write_mutex);
	return ret;
}
