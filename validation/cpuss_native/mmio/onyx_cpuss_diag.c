// SPDX-License-Identifier: GPL-2.0-only
/* Bounded read-only access check for native Onyx CPUSS configuration. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/firmware/qcom/qcom_scm.h>

static const phys_addr_t registers[] = {
	0x17800054, 0x17810054, 0x17820054, 0x17830054,
	0x17840054, 0x17850054, 0x17860054, 0x17870054,
	0x178a0098,
	0x178cc004, 0x178cc008, 0x178cc00c, 0x178cc010,
	0x178cc014, 0x178cc018, 0x178cc01c, 0x178cc020,
	0x178cc030,
};

static int __init onyx_cpuss_diag_init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		void __iomem *mapping;
		u32 value = 0, secure_value = 0;
		int secure_ret;
		long ret;

		secure_ret = qcom_scm_io_readl(registers[i], &secure_value);
		mapping = ioremap(registers[i], sizeof(value));
		if (!mapping)
			return -ENOMEM;
		ret = copy_from_kernel_nofault(&value,
					      (const void __force *)mapping,
					      sizeof(value));
		iounmap(mapping);
		pr_info("ONYX_CPUSS_DIAG addr=%pa scm_ret=%d scm_value=%08x mmio_ret=%ld mmio_value=%08x\n",
			&registers[i], secure_ret, secure_value, ret, value);
		if (ret)
			return ret;
	}
	pr_info("ONYX_CPUSS_DIAG complete: %zu configuration reads, no writes\n",
		ARRAY_SIZE(registers));
	return 0;
}

static void __exit onyx_cpuss_diag_exit(void)
{
}

module_init(onyx_cpuss_diag_init);
module_exit(onyx_cpuss_diag_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temporary read-only native Onyx CPUSS access diagnostic");
