// SPDX-License-Identifier: GPL-2.0
/*
 * BCM6838 Runner Ethernet, step 1c: a single "eth0" on top of the CFE-style
 * basic Runner data path (all LAN traffic is trapped to CPU RX queue 0).
 *
 *  - RX: one CPU ring (ring 0) of 16-byte descriptors in uncached memory,
 *        registered in the Runner ring descriptor table. NAPI is kicked by
 *        Runner interrupt 0 / sub-interrupt 0 (L1 hwirq 16), with a slow
 *        timer as a safety net; falls back to timer polling without IRQ.
 *  - TX: rdd_cpu_tx_write_eth_packet() to every LAN EMAC with link.
 *  - MAC/PHY: UniMAC 0..3 + the quad internal EGPHY at MDIO address 1..4.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/mii.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <asm/r4kcache.h>

#include "rdd.h"
#include "hwapi_mac.h"
#include "unimac_drv.h"
#include "egphy_drv_impl1.h"

#include "rdp_net.h"

#define RDP_NUM_EMACS		4
#define RX_RING_SIZE		128
#define RX_BUF_SIZE		2048
#define RX_RING_ID		0	/* basic config maps every reason to queue 0 */
#define POLL_INTERVAL		1	/* jiffies, when running without IRQ */
#define RUNNER_IRQ_HWIRQ	16	/* BCM6838_IRQ_RDP_RUNNER, Runner int0 */
#define RUNNER_INT		0	/* Runner interrupt 0 ... */
#define RUNNER_SUB_INT		RX_RING_ID	/* ... sub-interrupt = ring id */

/* CPU RX descriptor as seen by the (big endian) Runner */
struct rdp_rx_desc {
	u32 word0;	/* [31:20] flow id, [18:14] source port, [13:0] length */
	u32 word1;	/* [30:25] cpu reason */
	u32 word2;	/* [31] ownership (1 = host), [28:0] buffer address */
	u32 word3;
};
#define RX_DESC_OWN_HOST	BIT(31)

struct rdp_priv {
	struct net_device *ndev;
	struct napi_struct napi;
	struct timer_list poll_timer;
	struct timer_list link_timer;

	struct rdp_rx_desc *ring_cached;		/* kmalloc address */
	volatile struct rdp_rx_desc *ring;	/* uncached alias */
	void *rx_buf[RX_RING_SIZE];
	unsigned int rx_head;

	u32 link_map;
	int irq;	/* 0: timer polling */
};

static struct rdp_priv *rdp;

static int rx_debug = 4;
module_param(rx_debug, int, 0644);
MODULE_PARM_DESC(rx_debug, "Number of received packets to dump");

static bool rx_irq = true;
module_param(rx_irq, bool, 0444);
MODULE_PARM_DESC(rx_irq, "Use the Runner RX interrupt instead of timer polling");

static char *macaddr = "a0:65:18:b6:3e:ee";
module_param(macaddr, charp, 0444);
MODULE_PARM_DESC(macaddr, "eth0 MAC address (CFE base MAC by default)");

/* ---------------------------------------------------------------- PHY/MAC */

static u16 rdp_phy_read(int emac, int reg)
{
	return egphy_read_register(emac + 1, reg);
}

static void rdp_phy_write(int emac, int reg, u16 val)
{
	egphy_write_register(emac + 1, reg, val);
}

static void rdp_phy_mac_init(void)
{
	int e;

	/* power up and release the quad EGPHY; MDIO addresses start at 1 */
	egphy_reset((1 << RDP_NUM_EMACS) - 1);
	mdelay(10);

	for (e = 0; e < RDP_NUM_EMACS; e++) {
		pr_info("rdp: phy%d id %04x:%04x bmsr %04x\n", e + 1,
			rdp_phy_read(e, MII_PHYSID1), rdp_phy_read(e, MII_PHYSID2),
			rdp_phy_read(e, MII_BMSR));
		rdp_phy_write(e, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

		mac_hwapi_init_emac(e);
		mac_hwapi_set_rxtx_enable(e, 1, 1);
	}
}

static void rdp_link_poll(struct timer_list *t)
{
	struct rdp_priv *p = from_timer(p, t, link_timer);
	u32 map = 0;
	int e;

	for (e = 0; e < RDP_NUM_EMACS; e++) {
		u16 bmsr, lpa, adv, stat1000, ctrl1000;
		rdpa_emac_rate rate;
		int fd;

		rdp_phy_read(e, MII_BMSR);		/* latched low */
		bmsr = rdp_phy_read(e, MII_BMSR);
		if (!(bmsr & BMSR_LSTATUS))
			continue;
		map |= BIT(e);
		if (p->link_map & BIT(e))
			continue;

		adv = rdp_phy_read(e, MII_ADVERTISE);
		lpa = rdp_phy_read(e, MII_LPA);
		ctrl1000 = rdp_phy_read(e, MII_CTRL1000);
		stat1000 = rdp_phy_read(e, MII_STAT1000);
		if ((ctrl1000 & ADVERTISE_1000FULL) && (stat1000 & LPA_1000FULL)) {
			rate = rdpa_emac_rate_1g;
			fd = 1;
		} else if (adv & lpa & (ADVERTISE_100FULL | ADVERTISE_100HALF)) {
			rate = rdpa_emac_rate_100m;
			fd = !!(adv & lpa & ADVERTISE_100FULL);
		} else {
			rate = rdpa_emac_rate_10m;
			fd = !!(adv & lpa & ADVERTISE_10FULL);
		}
		mac_hwapi_set_speed(e, rate);
		mac_hwapi_set_duplex(e, fd);
		netdev_info(p->ndev, "LAN%d link up, %s %s duplex\n", e + 1,
			    rate == rdpa_emac_rate_1g ? "1000" :
			    rate == rdpa_emac_rate_100m ? "100" : "10",
			    fd ? "full" : "half");
	}

	for (e = 0; e < RDP_NUM_EMACS; e++)
		if ((p->link_map & BIT(e)) && !(map & BIT(e)))
			netdev_info(p->ndev, "LAN%d link down\n", e + 1);

	p->link_map = map;
	if (map)
		netif_carrier_on(p->ndev);
	else
		netif_carrier_off(p->ndev);

	mod_timer(&p->link_timer, jiffies + HZ);
}

/* ------------------------------------------------------------------- RX */

static void rdp_ring_register(phys_addr_t ring_phys)
{
	RDD_RING_DESCRIPTORS_TABLE_DTS *table;
	RDD_RING_DESCRIPTOR_DTS *d;

	/* same as rdd_ring_init(), which RDD_BASIC leaves out */
	table = (RDD_RING_DESCRIPTORS_TABLE_DTS *)(DEVICE_ADDRESS(RUNNER_COMMON_0_OFFSET) +
						   RING_DESCRIPTORS_TABLE_ADDRESS);
	d = &table->entry[RX_RING_ID];
	RDD_RING_DESCRIPTOR_ENTRIES_COUNTER_WRITE(0, d);
	RDD_RING_DESCRIPTOR_SIZE_OF_ENTRY_WRITE(sizeof(struct rdp_rx_desc), d);
	RDD_RING_DESCRIPTOR_NUMBER_OF_ENTRIES_WRITE(RX_RING_SIZE, d);
	RDD_RING_DESCRIPTOR_INTERRUPT_ID_WRITE(1 << RX_RING_ID, d);
	RDD_RING_DESCRIPTOR_RING_POINTER_WRITE((u32)ring_phys, d);
}

static void rdp_rx_give(struct rdp_priv *p, unsigned int i)
{
	unsigned long b = (unsigned long)p->rx_buf[i];

	blast_inv_dcache_range(b, b + RX_BUF_SIZE);
	p->ring[i].word0 = 0;
	p->ring[i].word1 = 0;
	p->ring[i].word3 = 0;
	wmb();
	p->ring[i].word2 = virt_to_phys(p->rx_buf[i]);	/* owner: Runner */
}

static int rdp_rx_setup(struct rdp_priv *p)
{
	size_t sz = RX_RING_SIZE * sizeof(struct rdp_rx_desc);
	unsigned int i;

	p->ring_cached = kzalloc(sz, GFP_KERNEL | GFP_DMA);
	if (!p->ring_cached)
		return -ENOMEM;
	blast_dcache_range((unsigned long)p->ring_cached,
			   (unsigned long)p->ring_cached + sz);
	p->ring = (void *)CKSEG1ADDR(virt_to_phys(p->ring_cached));

	for (i = 0; i < RX_RING_SIZE; i++) {
		p->rx_buf[i] = kmalloc(RX_BUF_SIZE, GFP_KERNEL | GFP_DMA);
		if (!p->rx_buf[i])
			return -ENOMEM;
		rdp_rx_give(p, i);
	}
	p->rx_head = 0;
	rdp_ring_register(virt_to_phys(p->ring_cached));
	return 0;
}

static void rdp_rx_free(struct rdp_priv *p)
{
	unsigned int i;

	for (i = 0; i < RX_RING_SIZE; i++)
		kfree(p->rx_buf[i]);
	kfree(p->ring_cached);
}

static int rdp_poll(struct napi_struct *napi, int budget)
{
	struct rdp_priv *p = container_of(napi, struct rdp_priv, napi);
	struct net_device *ndev = p->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int i = p->rx_head;
		u32 w0, w1, w2, len, port;
		struct sk_buff *skb;
		void *buf;

		w2 = p->ring[i].word2;
		if (!(w2 & RX_DESC_OWN_HOST))
			break;
		rmb();
		w0 = p->ring[i].word0;
		w1 = p->ring[i].word1;
		len = w0 & 0x3fff;
		port = (w0 >> 14) & 0x1f;
		buf = p->rx_buf[i];
		blast_inv_dcache_range((unsigned long)buf, (unsigned long)buf + len);

		if (rx_debug > 0) {
			rx_debug--;
			netdev_info(ndev, "rx: w0=%08x w1=%08x w2=%08x len=%u port=%u reason=%u\n",
				    w0, w1, w2, len, port, (w1 >> 25) & 0x3f);
			print_hex_dump(KERN_INFO, "rdp rx: ", DUMP_PREFIX_OFFSET,
				       16, 1, buf, min(len, 48U), false);
		}

		if (len >= ETH_HLEN && len <= RX_BUF_SIZE) {
			skb = napi_alloc_skb(napi, len);
			if (skb) {
				skb_put_data(skb, buf, len);
				skb->protocol = eth_type_trans(skb, ndev);
				ndev->stats.rx_packets++;
				ndev->stats.rx_bytes += len;
				napi_gro_receive(napi, skb);
			} else {
				ndev->stats.rx_dropped++;
			}
		} else {
			ndev->stats.rx_errors++;
		}

		rdp_rx_give(p, i);
		p->rx_head = (i + 1) % RX_RING_SIZE;
		done++;
	}

	if (done < budget && napi_complete_done(napi, done)) {
		if (p->irq)
			rdd_interrupt_unmask(RUNNER_INT, RUNNER_SUB_INT);
		mod_timer(&p->poll_timer,
			  jiffies + (p->irq ? HZ : POLL_INTERVAL));
	}
	return done;
}

static void rdp_poll_timer(struct timer_list *t)
{
	struct rdp_priv *p = from_timer(p, t, poll_timer);

	napi_schedule(&p->napi);
}

static irqreturn_t rdp_rx_isr(int irq, void *dev_id)
{
	struct rdp_priv *p = dev_id;

	rdd_interrupt_mask(RUNNER_INT, RUNNER_SUB_INT);
	rdd_interrupt_clear(RUNNER_INT, RUNNER_SUB_INT);
	napi_schedule(&p->napi);
	return IRQ_HANDLED;
}

/* map the Runner interrupt through the peripheral L1 controller */
static int rdp_rx_irq_setup(struct rdp_priv *p)
{
	struct of_phandle_args args = { .args_count = 1,
					.args = { RUNNER_IRQ_HWIRQ } };
	int irq, ret;

	args.np = of_find_compatible_node(NULL, NULL, "brcm,bcm6345-l1-intc");
	if (!args.np)
		return -ENODEV;
	irq = irq_create_of_mapping(&args);
	of_node_put(args.np);
	if (!irq)
		return -ENXIO;

	rdd_interrupt_mask(RUNNER_INT, RUNNER_SUB_INT);
	rdd_interrupt_clear(RUNNER_INT, RUNNER_SUB_INT);
	ret = request_irq(irq, rdp_rx_isr, 0, "bcm6838-rdp", p);
	if (ret) {
		irq_dispose_mapping(irq);
		return ret;
	}
	p->irq = irq;
	return 0;
}

/* ------------------------------------------------------------------- TX */

static netdev_tx_t rdp_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct rdp_priv *p = netdev_priv(ndev);
	u32 map = p->link_map;
	int e, sent = 0;

	if (skb_put_padto(skb, ETH_ZLEN)) {
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	/* no switch learning yet: send the frame out of every port with link */
	for (e = 0; e < RDP_NUM_EMACS; e++) {
		if (!(map & BIT(e)))
			continue;
		if (rdd_cpu_tx_write_eth_packet(skb->data, skb->len,
						BL_LILAC_RDD_EMAC_ID_0 + e, 0,
						BL_LILAC_RDD_QUEUE_0) == BL_LILAC_RDD_OK)
			sent++;
	}

	if (sent) {
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += skb->len;
	} else {
		ndev->stats.tx_dropped++;
	}
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static int rdp_open(struct net_device *ndev)
{
	struct rdp_priv *p = netdev_priv(ndev);

	napi_enable(&p->napi);
	if (p->irq) {
		rdd_interrupt_clear(RUNNER_INT, RUNNER_SUB_INT);
		rdd_interrupt_unmask(RUNNER_INT, RUNNER_SUB_INT);
	}
	mod_timer(&p->poll_timer, jiffies + POLL_INTERVAL);
	mod_timer(&p->link_timer, jiffies + HZ / 10);
	netif_start_queue(ndev);
	return 0;
}

static int rdp_stop(struct net_device *ndev)
{
	struct rdp_priv *p = netdev_priv(ndev);

	netif_stop_queue(ndev);
	if (p->irq)
		rdd_interrupt_mask(RUNNER_INT, RUNNER_SUB_INT);
	del_timer_sync(&p->link_timer);
	del_timer_sync(&p->poll_timer);
	napi_disable(&p->napi);
	return 0;
}

static const struct net_device_ops rdp_netdev_ops = {
	.ndo_open		= rdp_open,
	.ndo_stop		= rdp_stop,
	.ndo_start_xmit		= rdp_xmit,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

int rdp_net_init(void)
{
	struct net_device *ndev;
	u8 mac[ETH_ALEN];
	int ret;

	ndev = alloc_etherdev(sizeof(struct rdp_priv));
	if (!ndev)
		return -ENOMEM;
	rdp = netdev_priv(ndev);
	rdp->ndev = ndev;
	ndev->netdev_ops = &rdp_netdev_ops;
	if (mac_pton(macaddr, mac) && is_valid_ether_addr(mac))
		eth_hw_addr_set(ndev, mac);
	else
		eth_hw_addr_random(ndev);

	netif_napi_add(ndev, &rdp->napi, rdp_poll);
	timer_setup(&rdp->poll_timer, rdp_poll_timer, 0);
	timer_setup(&rdp->link_timer, rdp_link_poll, 0);

	ret = rdp_rx_setup(rdp);
	if (ret)
		goto err;

	rdp_phy_mac_init();
	netif_carrier_off(ndev);

	if (rx_irq) {
		ret = rdp_rx_irq_setup(rdp);
		if (ret)
			netdev_warn(ndev, "no Runner IRQ (%d), polling\n", ret);
	}

	ret = register_netdev(ndev);
	if (ret)
		goto err;
	netdev_info(ndev, "BCM6838 Runner ethernet, MAC %pM, %s\n",
		    ndev->dev_addr, rdp->irq ? "Runner IRQ" : "timer polling");
	return 0;

err:
	rdp_rx_free(rdp);
	free_netdev(ndev);
	rdp = NULL;
	return ret;
}

void rdp_net_exit(void)
{
	if (!rdp)
		return;
	unregister_netdev(rdp->ndev);
	if (rdp->irq)
		free_irq(rdp->irq, rdp);
	/* the Runner keeps the ring address, leave the buffers allocated */
	free_netdev(rdp->ndev);
	rdp = NULL;
}
