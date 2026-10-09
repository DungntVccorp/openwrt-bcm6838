// SPDX-License-Identifier: GPL-2.0
/*
 * Flashing helper for dropping the stock image B: expose the old image B
 * (bootfs + rootfs) and the enlarged UBI range as writable MTD partitions.
 * Nothing outside 0x620000..0x7ae0000 (CFE, bootfs A, stock data, CFE BBT)
 * can be reached through them.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#define BOOTFS_B_OFS	0x3d80000
#define BOOTFS_B_LEN	0x0520000
#define ROOTFS_B_OFS	0x42a0000
#define ROOTFS_B_LEN	0x3840000
#define UBI_BIG_OFS	0x0620000
#define UBI_BIG_LEN	0x74c0000

static int __init wipeb_init(void)
{
	struct mtd_info *cfe, *master;
	int ret;

	BUILD_BUG_ON(BOOTFS_B_OFS + BOOTFS_B_LEN != ROOTFS_B_OFS);
	BUILD_BUG_ON(ROOTFS_B_OFS + ROOTFS_B_LEN != 0x7ae0000);
	BUILD_BUG_ON(UBI_BIG_OFS + UBI_BIG_LEN != 0x7ae0000);

	cfe = get_mtd_device_nm("cfe");
	if (IS_ERR(cfe))
		return PTR_ERR(cfe);
	master = cfe->parent;
	put_mtd_device(cfe);
	if (!master || master->size != 0x8000000 || master->erasesize != 0x20000) {
		pr_err("wipeb: unexpected NAND master\n");
		return -ENODEV;
	}

	ret = mtd_add_partition(master, "bootfs_b", BOOTFS_B_OFS, BOOTFS_B_LEN);
	if (ret)
		return ret;
	ret = mtd_add_partition(master, "rootfs_b", ROOTFS_B_OFS, ROOTFS_B_LEN);
	if (ret)
		return ret;
	ret = mtd_add_partition(master, "ubi_big", UBI_BIG_OFS, UBI_BIG_LEN);
	pr_info("wipeb: partitions added (%d)\n", ret);
	return ret;
}
module_init(wipeb_init);
MODULE_LICENSE("GPL");
