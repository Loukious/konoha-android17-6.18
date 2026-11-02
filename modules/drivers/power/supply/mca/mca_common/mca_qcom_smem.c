#include <linux/module.h>
#include <linux/types.h>
#include <linux/err.h>
#include <linux/soc/qcom/smem.h>

#include <mca/common/mca_log.h>
#include <mca/common/mca_smem.h>

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "mca_smem"
#endif

#define SMEM_ID_VENDOR_BATTERY_INFO 0x51

struct mca_smem_battery_info {
	u32 reserved[8]; /* 0x00 .. 0x1f */
	u32 zero_speed_start_mode; /* 0x20 */
	u8 verify_result; /* 0x24 : bit5 verified, bit4 chip_ok */
	u8 cell_sn; /* 0x25 */
};

static struct mca_smem_battery_info *mca_smem_get_battery_info(void)
{
	struct mca_smem_battery_info *info;
	size_t size;
	int ret;

	ret = qcom_smem_alloc(QCOM_SMEM_HOST_ANY, SMEM_ID_VENDOR_BATTERY_INFO,
			      sizeof(*info));
	if (ret < 0 && ret != -EEXIST)
		return ERR_PTR(ret);
	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_ID_VENDOR_BATTERY_INFO, &size);
	if (IS_ERR(info))
		return info;
	if (size < sizeof(*info))
		return ERR_PTR(-EMSGSIZE);
	return info;
}

int get_smem_battery_info(int *is_zero_speed)
{
	struct mca_smem_battery_info *info;

	if (!is_zero_speed)
		return -EINVAL;
	info = mca_smem_get_battery_info();
	if (IS_ERR(info))
		return PTR_ERR(info);
	*is_zero_speed = READ_ONCE(info->zero_speed_start_mode);
	return 0;
}
EXPORT_SYMBOL(get_smem_battery_info);

int get_smem_battery_verify_result(u8 *verified, u8 *chip_ok)
{
	struct mca_smem_battery_info *info;
	u8 result;

	if (!verified || !chip_ok)
		return -EINVAL;
	info = mca_smem_get_battery_info();
	if (IS_ERR(info))
		return PTR_ERR(info);
	result = READ_ONCE(info->verify_result);

	*verified = (result >> 5) & 1;
	*chip_ok = (result >> 4) & 1;
	return 0;
}
EXPORT_SYMBOL(get_smem_battery_verify_result);

int get_smem_battery_cell_sn(u32 *cell_sn)
{
	struct mca_smem_battery_info *info;

	if (!cell_sn)
		return -EINVAL;
	info = mca_smem_get_battery_info();
	if (IS_ERR(info))
		return PTR_ERR(info);
	*cell_sn = READ_ONCE(info->cell_sn);
	return 0;
}
EXPORT_SYMBOL(get_smem_battery_cell_sn);

MODULE_DESCRIPTION("mca get smem info");
MODULE_AUTHOR("liuliang10@xiaomi.com");
MODULE_LICENSE("GPL v2");
