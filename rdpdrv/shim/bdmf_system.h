/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal stand-in for the SDK bdmf/system/linux/ce/bdmf_system.h, which
 * depends on the Broadcom kernel nbuff/FKB extensions. Only what the RDD
 * code built with RDD_BASIC needs.
 */
#ifndef _BDMF_SYSTEM_H_
#define _BDMF_SYSTEM_H_

#include <linux/slab.h>
#include <asm/io.h>
#include <asm/r4kcache.h>

static inline void *bdmf_alloc(size_t size)
{
	return kmalloc(size, GFP_ATOMIC);
}

static inline void bdmf_free(void *p)
{
	kfree(p);
}

/*
 * dma_cache_inv()/dma_cache_wback_inv() are function pointers that are not
 * exported to modules; use the r4k cache ops directly (BMIPS4350).
 */
static inline void bdmf_dcache_inv(unsigned long addr, unsigned long size)
{
	blast_inv_dcache_range(addr, addr + size);
}

static inline void bdmf_dcache_flush(unsigned long addr, unsigned long size)
{
	blast_dcache_range(addr, addr + size);
}

#endif
