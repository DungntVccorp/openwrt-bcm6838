// SPDX-License-Identifier: GPL-2.0
/* Minimal replacement for the SDK shared_utils.c helpers used by the PMC code. */
#include <linux/kernel.h>

#include "bcm_map_part.h"
#include "shared_utils.h"

unsigned int UtilGetChipRev(void)
{
	return PERF->RevID & REV_ID_MASK;
}
