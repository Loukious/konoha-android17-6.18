// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/spmi.h>
#include <linux/string.h>

#include <linux/soc/qcom/qti_pmic_glink.h>

#include <linux/unaligned.h>

#define MSG_OWNER_REG_DUMP		32783
#define MSG_TYPE_REQ_RESP		1
#define REG_DUMP_REG_READ_REQ		0x37
#define REG_DUMP_REG_WRITE_REQ		0x38

#define REG_DUMP_WAIT_TIME_MS		1000

#define REG_DUMP_GLINK_MAX_READ_BYTES	256
#define REG_DUMP_GLINK_MAX_WRITE_BYTES	1

#define PERIPH_MASK			GENMASK(7, 0)

struct reg_dump_read_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				bus_id;
	u32				pmic_sid;
	u32				address;
	u32				byte_count;
};

struct reg_dump_read_resp_msg {
	struct pmic_glink_hdr		hdr;
	u32				bus_id;
	u32				pmic_sid;
	u32				address;
	u32				byte_count;
	u8				data[REG_DUMP_GLINK_MAX_READ_BYTES];
};

struct reg_dump_write_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				bus_id;
	u32				pmic_sid;
	u32				address;
	u32				data;
};

struct reg_dump_write_resp_msg {
	struct pmic_glink_hdr		hdr;
	u32				return_status;
};

struct pmic_glink_debug_dev;

struct spmi_glink_ctrl {
	struct pmic_glink_debug_dev	*gd;
	struct spmi_controller		*spmi;
	u32				bus_id;
};

struct i2c_glink_ctrl {
	struct pmic_glink_debug_dev	*gd;
	struct i2c_adapter		i2c;
	u32				bus_id;
};

struct pmic_glink_debug_dev {
	struct pmic_glink_client	*client;
	struct device			*dev;
	struct mutex			lock;
	struct completion		ack;
	spinlock_t response_lock;
	bool pending;
	bool poisoned;
	u32 opcode;
	u32 bus_id, sid, address, bytes;
	int response_error;
	struct reg_dump_read_resp_msg	read_msg;
	struct spmi_glink_ctrl		**spmi_gctrl;
	struct i2c_glink_ctrl		**i2c_gctrl;
	u32				spmi_bus_count;
	u32				i2c_bus_count;
};

static int pmic_glink_debug_write(struct pmic_glink_debug_dev *gd, void *data, size_t len)
{
	struct reg_dump_read_req_msg *request = data;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&gd->response_lock, flags);
	/* The wire protocol has no transaction sequence: after timeout, a late
	 * untagged write ACK cannot safely be associated with another request. */
	if (gd->poisoned) {
		spin_unlock_irqrestore(&gd->response_lock, flags);
		return -EIO;
	}
	reinit_completion(&gd->ack);
	gd->opcode = request->hdr.opcode;
	gd->bus_id = request->bus_id;
	gd->sid = request->pmic_sid;
	gd->address = request->address;
	gd->bytes = request->hdr.opcode == REG_DUMP_REG_READ_REQ ? request->byte_count : 0;
	gd->response_error = 0;
	gd->pending = true;
	spin_unlock_irqrestore(&gd->response_lock, flags);
	ret = pmic_glink_write(gd->client, data, len);
	if (!ret && !wait_for_completion_timeout(&gd->ack, msecs_to_jiffies(REG_DUMP_WAIT_TIME_MS)))
		ret = -ETIMEDOUT;
	spin_lock_irqsave(&gd->response_lock, flags);
	gd->pending = false;
	if (ret == -ETIMEDOUT)
		gd->poisoned = true;
	if (!ret)
		ret = gd->response_error;
	spin_unlock_irqrestore(&gd->response_lock, flags);
	return ret;
}

static int pmic_glink_debug_callback(void *priv, void *data, size_t len)
{
	struct pmic_glink_debug_dev *gd = priv;
	struct pmic_glink_hdr *hdr = data;
	unsigned long flags;
	int ret = 0;
	if (!data || len < sizeof(*hdr) || hdr->owner != MSG_OWNER_REG_DUMP ||
	    hdr->type != MSG_TYPE_REQ_RESP)
		return -EINVAL;
	spin_lock_irqsave(&gd->response_lock, flags);
	if (!gd->pending || hdr->opcode != gd->opcode)
		goto out;
	if (hdr->opcode == REG_DUMP_REG_READ_REQ) {
		struct reg_dump_read_resp_msg *response = data;
		if (len != sizeof(*response)) {
			ret = -EPROTO;
			goto complete;
		}
		if (response->bus_id != gd->bus_id || response->pmic_sid != gd->sid ||
		    response->address != gd->address)
			goto out;
		if ((s32)response->byte_count < 0)
			ret = (s32)response->byte_count;
		else if (response->byte_count > sizeof(response->data) || response->byte_count != gd->bytes)
			ret = -EPROTO;
		else
			memcpy(&gd->read_msg, response, sizeof(*response));
	} else if (hdr->opcode == REG_DUMP_REG_WRITE_REQ) {
		struct reg_dump_write_resp_msg *response = data;
		if (len != sizeof(*response))
			ret = -EPROTO;
		else if (response->return_status)
			ret = (s32)response->return_status < 0 ? (s32)response->return_status : -EIO;
	} else {
		goto out;
	}
complete:
	gd->response_error = ret;
	gd->pending = false;
	if (ret == -EPROTO)
		gd->poisoned = true;
	complete(&gd->ack);
out:
	spin_unlock_irqrestore(&gd->response_lock, flags);
	return ret;
}

static int pmic_glink_read_regs(struct pmic_glink_debug_dev *gd, u32 bus_id,
				u8 sid, u16 addr, u8 *buf, size_t len)
{
	struct reg_dump_read_req_msg msg = {{0}};
	int ret;

	if (!len || !buf || len > REG_DUMP_GLINK_MAX_READ_BYTES)
		return -EINVAL;

	msg.hdr.owner = MSG_OWNER_REG_DUMP;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = REG_DUMP_REG_READ_REQ;

	msg.bus_id = bus_id;
	msg.pmic_sid = sid;
	msg.address = addr;
	msg.byte_count = len;

	ret = pmic_glink_debug_write(gd, &msg, sizeof(msg));
	if (ret)
		return ret;

	if (gd->read_msg.byte_count != len)
		return -EINVAL;

	memcpy(buf, gd->read_msg.data, len);

	return 0;
}

static int pmic_glink_write_reg(struct pmic_glink_debug_dev *gd, u32 bus_id,
				u8 sid, u16 addr, u8 val)
{
	struct reg_dump_write_req_msg msg = {{0}};

	msg.hdr.owner = MSG_OWNER_REG_DUMP;
	msg.hdr.type = MSG_TYPE_REQ_RESP;
	msg.hdr.opcode = REG_DUMP_REG_WRITE_REQ;

	msg.bus_id = bus_id;
	msg.pmic_sid = sid;
	msg.address = addr;
	msg.data = val;

	return pmic_glink_debug_write(gd, &msg, sizeof(msg));
}

static int pmic_glink_debug_read_regs(struct pmic_glink_debug_dev *gd, u32 bus_id,
			u8 sid, u16 addr, u8 *buf, size_t len)
{
	int ret, count;

	if (!len || !buf || sid > 0xf || len > ((0x10 - sid) * 0x10000UL - addr))
		return -EINVAL;
	mutex_lock(&gd->lock);
	do {
		count = min_t(size_t, len, REG_DUMP_GLINK_MAX_READ_BYTES);
		/* Ensure transactions are divided across peripherals */
		if ((addr & PERIPH_MASK) + count > PERIPH_MASK + 1)
			count = PERIPH_MASK + 1 - (addr & PERIPH_MASK);

		ret = pmic_glink_read_regs(gd, bus_id, sid, addr, buf, count);
		if (ret)
			goto done;

		/* Handle a transaction split across SIDs */
		if ((u16)(addr + count) < addr)
			sid++;
		addr += count;
		buf += count;
		len -= count;
	} while (len > 0);

done:
	mutex_unlock(&gd->lock);
	return ret;
}

static int pmic_glink_debug_write_regs(struct pmic_glink_debug_dev *gd, u32 bus_id,
			u8 sid, u16 addr, const u8 *buf, size_t len)
{
	int ret, count;

	if (!len || !buf || sid > 0xf || len > ((0x10 - sid) * 0x10000UL - addr))
		return -EINVAL;
	mutex_lock(&gd->lock);
	do {
		count = min_t(size_t, len, REG_DUMP_GLINK_MAX_WRITE_BYTES);
		/* Ensure transactions are divided across peripherals */
		if ((addr & PERIPH_MASK) + count > PERIPH_MASK + 1)
			count = PERIPH_MASK + 1 - (addr & PERIPH_MASK);

		ret = pmic_glink_write_reg(gd, bus_id, sid, addr, *buf);
		if (ret)
			goto done;

		/* Handle a transaction split across SIDs */
		if ((u16)(addr + count) < addr)
			sid++;
		addr += count;
		buf += count;
		len -= count;
	} while (len > 0);

done:
	mutex_unlock(&gd->lock);
	return ret;
}

/* Non-data SPMI command */
static int spmi_glink_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	return -EOPNOTSUPP;
}

static int spmi_glink_read_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			     u16 addr, u8 *buf, size_t len)
{
	struct spmi_glink_ctrl *spmi_gctrl = spmi_controller_get_drvdata(ctrl);
	int ret;

	ret = pmic_glink_debug_read_regs(spmi_gctrl->gd,
			spmi_gctrl->bus_id, sid, addr, buf, len);
	if (ret < 0)
		return ret;

	dev_dbg(spmi_gctrl->gd->dev, "%s: bus id %#x, sid %#x, reg %#x, data %*ph\n",
		__func__, spmi_gctrl->bus_id, sid, addr, (int)len, buf);
	return 0;
}

static int spmi_glink_write_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			     u16 addr, const u8 *buf, size_t len)
{
	struct spmi_glink_ctrl *spmi_gctrl = spmi_controller_get_drvdata(ctrl);
	int ret;

	ret = pmic_glink_debug_write_regs(spmi_gctrl->gd,
			spmi_gctrl->bus_id, sid, addr, buf, len);
	if (ret < 0)
		return ret;

	dev_dbg(spmi_gctrl->gd->dev, "%s: bus id %#x, sid %#x, reg %#x, data %*ph\n",
		__func__, spmi_gctrl->bus_id, sid, addr, (int)len, buf);
	return 0;
}

static int i2c_glink_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct i2c_glink_ctrl *i2c_gctrl = i2c_get_adapdata(adap);
	struct pmic_glink_debug_dev *gd;
	u8 sid, *buf;
	u16 reg;
	u32 bus_id;
	size_t len;
	bool read;
	int i, ret;

	if (!i2c_gctrl)
		return -ENODEV;

	gd = i2c_gctrl->gd;
	bus_id = i2c_gctrl->bus_id;

	for (i = 0; i < num; i++) {
		if (msgs[i].len < 2 || !msgs[i].buf || msgs[i].addr > 0xf || msgs[i].flags) {
			dev_dbg(gd->dev, "%s: unexpected msg: addr=%#x, flags=%#x, len=%u\n",
				__func__, msgs[i].addr, msgs[i].flags,
				msgs[i].len);
			return -EINVAL;
		}
		/*
		 * For I2C write operation, only one i2c_msg data block is
		 * present. i2c_msg[0].buf contains the 2 byte register address
		 * followed by the data buffer. i2c_msg[0].len is the sum of the
		 * register length (2 bytes) and the data length.
		 *
		 * For I2C read, there are two i2c_msg data blocks.
		 * i2c_msg[0].buf has the register address and i2c_msg[0].len is
		 * the register length (2). i2c_msg[1].flags will have the
		 * I2C_M_RD flag set. i2c_msg[1].buf is the data buffer and
		 * msg[1].len is the data length.
		 */
		sid = (u8) msgs[i].addr;
		reg = get_unaligned_be16(msgs[i].buf);

		if (i + 1 < num && (msgs[i + 1].flags & I2C_M_RD) &&
		    msgs[i].addr == msgs[i + 1].addr && msgs[i + 1].len > 0 &&
		    msgs[i].len == 2 && msgs[i + 1].flags == I2C_M_RD && msgs[i + 1].buf) {
			/* Read operation */
			read = true;
			len = (size_t) msgs[i + 1].len;
			buf = msgs[i + 1].buf;
			i++;
		} else if (msgs[i].len > 2 && !(msgs[i].flags & I2C_M_RD)) {
			/* Write operation */
			read = false;
			len = (size_t) msgs[i].len - 2;
			buf = msgs[i].buf + 2;
		} else {
			/* Unknown operation */
			dev_dbg(gd->dev, "%s: unexpected msg: addr=%#x, flags=%#x, len=%u\n",
				__func__, msgs[i].addr, msgs[i].flags,
				msgs[i].len);
			return -EINVAL;
		}

		if (read)
			ret = pmic_glink_debug_read_regs(gd, bus_id, sid, reg, buf, len);
		else
			ret = pmic_glink_debug_write_regs(gd, bus_id, sid, reg, buf, len);

		if (ret) {
			dev_err(gd->dev, "%s failed\n", __func__);
			return ret;
		}

		dev_dbg(gd->dev, "%s: %s: bus id %#x, sid %#x, reg %#x, data %*ph\n",
			__func__, read ? "read" : "write", bus_id, sid, reg, (int)len, buf);
	}

	return num;
}

static u32 i2c_glink_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm glink_i2c_algo = {
	.master_xfer	= i2c_glink_xfer,
	.functionality	= i2c_glink_func,
};

static int pmic_glink_debug_add_i2c_bus(struct pmic_glink_debug_dev *gd,
				struct fwnode_handle *fwnode)
{
	int ret;
	struct i2c_glink_ctrl *i2c_gctrl;
	struct i2c_adapter *adap;

	if (!gd->i2c_gctrl)
		return -ENODEV;

	i2c_gctrl = devm_kzalloc(gd->dev, sizeof(*i2c_gctrl), GFP_KERNEL);
	if (!i2c_gctrl)
		return -ENOMEM;

	ret = fwnode_property_read_u32(fwnode, "reg", &i2c_gctrl->bus_id);
	if (ret) {
		dev_err(gd->dev, "Could not find reg property, ret=%d\n",
			ret);
		return ret;
	}

	i2c_gctrl->gd = gd;
	adap = &i2c_gctrl->i2c;
	adap->owner = THIS_MODULE;
	adap->algo = &glink_i2c_algo;
	adap->dev.parent = gd->dev;
	adap->dev.of_node = to_of_node(fwnode);
	strscpy(adap->name, "glink-i2c", sizeof(adap->name));
	i2c_set_adapdata(adap, i2c_gctrl);

	ret = i2c_add_adapter(adap);
	if (ret) {
		dev_err(gd->dev, "Add i2c adapter failed, ret=%d\n", ret);
		return ret;
	}

	gd->i2c_gctrl[gd->i2c_bus_count++] = i2c_gctrl;
	return 0;
}

static int pmic_glink_debug_add_spmi_bus(struct pmic_glink_debug_dev *gd,
				struct fwnode_handle *fwnode)
{
	struct spmi_controller *ctrl;
	struct spmi_glink_ctrl *spmi_gctrl;
	int ret;

	if (!gd->spmi_gctrl)
		return -ENODEV;

	ctrl = spmi_controller_alloc(gd->dev, sizeof(*spmi_gctrl));
	if (!ctrl)
		return -ENOMEM;

	spmi_gctrl = spmi_controller_get_drvdata(ctrl);
	spmi_gctrl->spmi = ctrl;
	spmi_gctrl->gd = gd;
	ret = fwnode_property_read_u32(fwnode, "reg", &spmi_gctrl->bus_id);
	if (ret) {
		dev_err(gd->dev, "Could not find reg property, ret=%d\n",
			ret);
		spmi_controller_put(ctrl);
		return ret;
	}

	ctrl->cmd = spmi_glink_cmd;
	ctrl->read_cmd = spmi_glink_read_cmd;
	ctrl->write_cmd = spmi_glink_write_cmd;
	ctrl->dev.of_node = to_of_node(fwnode);

	ret = spmi_controller_add(ctrl);
	if (ret) {
		spmi_controller_put(ctrl);
		return ret;
	}

	gd->spmi_gctrl[gd->spmi_bus_count++] = spmi_gctrl;
	return 0;
}

static void pmic_glink_debug_remove(struct platform_device *pdev)
{
	struct pmic_glink_debug_dev *gd = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < gd->spmi_bus_count; i++) {
		if (gd->spmi_gctrl[i]) {
			spmi_controller_remove(gd->spmi_gctrl[i]->spmi);
			spmi_controller_put(gd->spmi_gctrl[i]->spmi);
		}
	}

	for (i = 0; i < gd->i2c_bus_count; i++) {
		if (gd->i2c_gctrl[i])
			i2c_del_adapter(&gd->i2c_gctrl[i]->i2c);
	}

	pmic_glink_unregister_client(gd->client);
}

static int pmic_glink_debug_probe(struct platform_device *pdev)
{
	struct pmic_glink_debug_dev *gd;
	struct pmic_glink_client_data client_data = { };
	struct fwnode_handle *child;
	const char *bus = NULL;
	u32 spmi_bus_count = 0, i2c_bus_count = 0;
	int ret;

	gd = devm_kzalloc(&pdev->dev, sizeof(*gd), GFP_KERNEL);
	if (!gd)
		return -ENOMEM;

	gd->dev = &pdev->dev;
	mutex_init(&gd->lock);
	spin_lock_init(&gd->response_lock);
	init_completion(&gd->ack);
	platform_set_drvdata(pdev, gd);

	device_for_each_child_node(&pdev->dev, child) {
		ret = fwnode_property_read_string(child, "qcom,bus-type", &bus);
		if (ret) {
			if (ret == -EINVAL) {
				spmi_bus_count++;
				continue;
			}
			fwnode_handle_put(child);
			dev_err(gd->dev, "Get qcom,bus-type failed, ret=%d\n", ret);
			return ret;
		}

		if (!strcmp(bus, "i2c")) {
			i2c_bus_count++;
		} else if (!strcmp(bus, "spmi")) {
			spmi_bus_count++;
		} else  {
			dev_err(gd->dev, "unsupported bus type: %s\n", bus);
			fwnode_handle_put(child);
			return -EINVAL;
		}
	}

	if (!spmi_bus_count && !i2c_bus_count) {
		dev_err(&pdev->dev, "pmic bus child nodes missing\n");
		return -ENODEV;
	}

	client_data.id = MSG_OWNER_REG_DUMP;
	client_data.name = "pmic_register_dump";
	client_data.msg_cb = pmic_glink_debug_callback;
	client_data.priv = gd;

	gd->client = pmic_glink_register_client(&pdev->dev, &client_data);
	if (IS_ERR(gd->client)) {
		ret = PTR_ERR(gd->client);
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Error registering with pmic_glink, ret=%d\n",
				ret);
		return ret;
	}

	if (spmi_bus_count) {
		gd->spmi_gctrl = devm_kcalloc(&pdev->dev, spmi_bus_count,
				sizeof(*gd->spmi_gctrl), GFP_KERNEL);
		if (!gd->spmi_gctrl) {
			ret = -ENOMEM;
			goto err_remove_ctrl;
		}
	}

	if (i2c_bus_count) {
		gd->i2c_gctrl = devm_kcalloc(&pdev->dev, i2c_bus_count,
				sizeof(*gd->i2c_gctrl), GFP_KERNEL);
		if (!gd->i2c_gctrl) {
			ret = -ENOMEM;
			goto err_remove_ctrl;
		}
	}

	device_for_each_child_node(&pdev->dev, child) {
		bus = NULL;
		ret = fwnode_property_read_string(child, "qcom,bus-type", &bus);
		if (!ret && !strcmp(bus, "i2c"))
			ret = pmic_glink_debug_add_i2c_bus(gd, child);
		else
			ret = pmic_glink_debug_add_spmi_bus(gd, child);
		if (ret) {
			fwnode_handle_put(child);
			goto err_remove_ctrl;
		}
	}

	return 0;

err_remove_ctrl:
	pmic_glink_debug_remove(pdev);

	return ret;
}

static const struct of_device_id pmic_glink_debug_match_table[] = {
	{ .compatible = "qcom,pmic-glink-debug", },
	{ .compatible = "qcom,spmi-glink-debug", },
	{ .compatible = "qcom,i2c-glink-debug", },
	{}
};
MODULE_DEVICE_TABLE(of, pmic_glink_debug_match_table);

static struct platform_driver pmic_glink_debug_driver = {
	.driver = {
		.name = "pmic_glink_debug",
		.of_match_table = pmic_glink_debug_match_table,
	},
	.probe = pmic_glink_debug_probe,
	.remove = pmic_glink_debug_remove,
};
module_platform_driver(pmic_glink_debug_driver);

MODULE_DESCRIPTION("PMIC Glink Debug Driver");
MODULE_LICENSE("GPL");
