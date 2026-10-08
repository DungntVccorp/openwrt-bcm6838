/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility glue force-included into every file of the module, so the
 * Broadcom SDK 416L05 sources (written for Linux 3.4) build on Linux 6.6.
 */
#ifndef RDPDRV_COMPAT_H
#define RDPDRV_COMPAT_H

/* bl_os_wraper.h includes the slab allocator internals, gone since 5.x */
#define _LINUX_SLAB_DEF_H

/*
 * Pull in every kernel header bl_os_wraper.h uses before any SDK header:
 * access_macros.h defines its own FIELD_GET, which must not leak into the
 * kernel headers (asm/mips-cm.h uses the <linux/bitfield.h> one).
 */
#include <linux/version.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/irq.h>
#include <linux/uaccess.h>

/* system buffer handle used by rdpa_cpu.h (skb or FKB in the SDK kernel) */
typedef void *bdmf_sysb;

/* CFE-style allocators used by the RDD_BASIC code paths */
#define KMALLOC(size, align)	kmalloc(size, GFP_ATOMIC)
#define KFREE(ptr)		kfree(ptr)

#endif
