// SPDX-License-Identifier: GPL-2.0
/*
 * BCM6838 RDP (Runner) bring-up.
 *
 * Everything is enabled by default (kmod autoload). For bring-up the
 * stages can be switched off, nothing touches an RDP register before the
 * soft resets are released (that hangs the UBUS):
 *   insmod bcm6838_rdp.ko power_up=0               read-only PMC / RDP status
 *   insmod bcm6838_rdp.ko dp_init=0 probe=1        reset release + UniMAC read
 *   insmod bcm6838_rdp.ko net=0                    data_path_init() + go only
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>

#include "bcm_map_part.h"
#include "pmc_drv.h"
#include "BPCM.h"
#include "shared_utils.h"
#include "rdd.h"
#include "data_path_init.h"
#include "rdp_net.h"

/* implemented in pmc6838.c */
int Ping(void);

static bool power_up = true;
module_param(power_up, bool, 0444);
MODULE_PARM_DESC(power_up, "Release the RDP block soft resets via the PMC");

static bool probe;
module_param(probe, bool, 0444);
MODULE_PARM_DESC(probe, "Read UniMAC registers after a successful power up");

static bool dp_init = true;
module_param(dp_init, bool, 0444);
MODULE_PARM_DESC(dp_init, "Run the SDK data_path_init()/data_path_go()");

static bool net = true;
module_param(net, bool, 0444);
MODULE_PARM_DESC(net, "Register eth0 on top of the data path");

/* UniMAC 0 registers (RDP block), see 6838_map_part.h */
#define UNIMAC0_BASE		0xb30d4000
#define UNIMAC_MISC_CFG		0xb30db800

/* DDR for the Runner, reserved in the DTS (rdp-tm / rdp-mc), 2 MB aligned */
#define RDP_TM_BASE		0x06600000
#define RDP_MC_BASE		0x06200000

static void rdp_dump_power(const char *when)
{
	uint32 v = 0;
	int ret, zone;

	ret = ReadBPCMRegister(PMB_ADDR_RDP, BPCMRegOffset(capabilities), &v);
	pr_info("rdp: [%s] capabilities ret=%d val=%08x\n", when, ret, (u32)v);
	ret = ReadBPCMRegister(PMB_ADDR_RDP, BPCMRegOffset(sr_control), &v);
	pr_info("rdp: [%s] sr_control   ret=%d val=%08x\n", when, ret, (u32)v);
	for (zone = 0; zone < PMB_ZONES_RDP; zone++) {
		ret = ReadZoneRegister(PMB_ADDR_RDP, zone, 0, &v);
		pr_info("rdp: [%s] zone%d ctrl   ret=%d val=%08x\n", when, zone,
			ret, (u32)v);
	}
}

/*
 * The RDP block is still powered after CFE, only its soft resets are held:
 * same sequence as the 63138 pmc_rdp_init(), minus the RDP PLL the 6838 lacks.
 */
static int rdp_power_up(void)
{
	int ret;

	ret = WriteBPCMRegister(PMB_ADDR_RDP, BPCMRegOffset(sr_control), 0);
	if (ret)
		return ret;
	ret = PowerOnDevice(PMB_ADDR_RDP);
	if (ret)
		return ret;
	return WriteBPCMRegister(PMB_ADDR_RDP, BPCMRegOffset(sr_control),
				 0xffffffff);
}

static int rdp_data_path_start(void)
{
	static S_DPI_CFG cfg = {
		.wan_bbh		= DRV_BBH_GPON,	/* no WAN port is set up */
		.mtu_size		= 1536,
		.enabled_port_map	= 0x0f,		/* EMAC0..3 = LAN1..4 */
		.runner_tm_base_addr	= CKSEG1ADDR(RDP_TM_BASE),
		.runner_mc_base_addr	= CKSEG1ADDR(RDP_MC_BASE),
		.bpm_buffer_size	= RDPA_BPM_BUFFER_2K,
		.bpm_buffers_number	= DRV_BPM_GLOBAL_THRESHOLD_7_5K,
	};
	u32 rc;

	rc = data_path_init(&cfg);
	pr_info("rdp: data_path_init() = %u\n", rc);
	if (rc)
		return -EIO;
	rc = data_path_go();
	pr_info("rdp: data_path_go() = %u\n", rc);
	return rc ? -EIO : 0;
}

static int __init rdp_init(void)
{
	int ret;
	int i;

	pr_info("rdp: chip rev %u, PERF RevID %08x\n",
		UtilGetChipRev(), (u32)PERF->RevID);

	ret = Ping();
	pr_info("rdp: PMC ping ret=%d\n", ret);
	if (ret) {
		pr_err("rdp: PMC does not answer, stopping\n");
		return -EIO;
	}

	rdp_dump_power("before");

	if (!power_up)
		return 0;

	ret = rdp_power_up();
	pr_info("rdp: rdp_power_up() = %d\n", ret);
	rdp_dump_power("after");
	if (ret)
		return 0;

	if (probe) {
		for (i = 0; i < 0x20; i += 4)
			pr_info("rdp: UNIMAC0+%02x = %08x\n", i,
				__raw_readl((void __iomem *)(UNIMAC0_BASE + i)));
		pr_info("rdp: UNIMAC_MISC_CFG = %08x\n",
			__raw_readl((void __iomem *)UNIMAC_MISC_CFG));
	}

	if (dp_init && !rdp_data_path_start() && net) {
		ret = rdp_net_init();
		pr_info("rdp: rdp_net_init() = %d\n", ret);
	}

	return 0;
}

static void __exit rdp_exit(void)
{
	rdp_net_exit();
}

module_init(rdp_init);
module_exit(rdp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BCM6838 RDP bring-up");
