// SPDX-License-Identifier: GPL-2.0
/*
 * Flashing helper: expose exactly the two slot A ranges of the NAND as
 * writable MTD partitions on top of the read-only "storage" partition's
 * master. Nothing else of the flash becomes writable.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#define SLOTA_BOOTFS_OFS	0x0020000
#define SLOTA_BOOTFS_LEN	0x0600000
#define SLOTA_UBI_OFS		0x0620000
#define SLOTA_UBI_LEN		0x3760000
#define SLOTB_START		0x3d80000

static int __init slota_init(void)
{
	struct mtd_info *storage, *master;
	int ret;

	BUILD_BUG_ON(SLOTA_UBI_OFS + SLOTA_UBI_LEN != SLOTB_START);
	BUILD_BUG_ON(SLOTA_BOOTFS_OFS + SLOTA_BOOTFS_LEN != SLOTA_UBI_OFS);

	storage = get_mtd_device_nm("storage");
	if (IS_ERR(storage))
		return PTR_ERR(storage);
	master = storage->parent;
	put_mtd_device(storage);
	if (!master || master->size != 0x8000000 || master->erasesize != 0x20000) {
		pr_err("slota: unexpected NAND master\n");
		return -ENODEV;
	}

	ret = mtd_add_partition(master, "slota_bootfs", SLOTA_BOOTFS_OFS, SLOTA_BOOTFS_LEN);
	if (ret)
		return ret;
	ret = mtd_add_partition(master, "slota_ubi", SLOTA_UBI_OFS, SLOTA_UBI_LEN);
	pr_info("slota: partitions added (%d)\n", ret);
	return ret;
}
module_init(slota_init);
MODULE_LICENSE("GPL");
