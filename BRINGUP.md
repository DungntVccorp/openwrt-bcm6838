# BCM6838 / Mitrastar GPT-2541GNAC - initramfs bring-up notes

Status (2026-10-08): OpenWrt 6.6 **initramfs boots to an interactive shell** over UART
when loaded through CFE + TFTP. **Ethernet works** with the Runner driver package
`kmod-bcm6838-rdp` (see "Ethernet"). Both hardware threads run (SMP, 2 CPUs).

## Build

Config used (`.config`):

```
CONFIG_TARGET_bmips=y
CONFIG_TARGET_bmips_bcm6838=y
CONFIG_TARGET_bmips_bcm6838_DEVICE_mitrastar_gpt-2541gnac=y
CONFIG_TARGET_ROOTFS_INITRAMFS=y
```

Built in a Docker container (Ubuntu 24.04, `make -j20`). Output:
`bin/targets/bmips/bcm6838/openwrt-bmips-bcm6838-mitrastar_gpt-2541gnac-initramfs.elf` (~5.6 MB).

Tip: on Windows build inside a Linux filesystem (container volume / `git clone` inside the
container). `git archive` with `core.autocrlf=true` converts scripts to CRLF and breaks the build
(`/usr/bin/env: 'perl\r'`).

## Loading it on the router (CFE)

* PC NIC `192.168.1.100`, router `192.168.1.1` (from CFE `p`).
* The ELF must be served as TFTP file `vmlinux` (CFE default run file name).
* CFE has no `boot`/`tftp` command. Use `r <hostip>:<file>`:

  ```
  CFE> r 192.168.1.100:vmlinux
  ```

  `r c` does not select "tftp", it asks for a file called `c`.
* Docker Desktop (Windows) NAT breaks TFTP (data packets come from a different port/IP and the
  CFE client drops them). Run the TFTP server directly on the host (tftpd64, or any small
  RFC1350 server bound to 192.168.1.100).
* The CFE stops reading once the ELF segments are loaded, so the server logging
  "aborted at the last block" is normal.

## Device tree changes (target/linux/bmips/dts)

| Change | Why |
|---|---|
| RAM `0x08000000` (128 MB) instead of 256 MB | Board has 128 MB. The stock kernel reserves 4 MB (MC) + 25 MB (TM) for the Runner. |
| `&nflash` disabled | brcmnand polls forever (no IRQ at that time) and, with `nand-on-flash-bbt`, may rewrite the bad-block table of the stock firmware. Not needed for initramfs. |
| `bootargs = "earlycon console=ttyS0,115200"` | console on UART0 (`maxcpus=1` no longer needed, see "CPU1"). |
| `periph_intc` register pairs / parent IRQs swapped | See "Interrupt routing". |

### Interrupt routing (the important finding)

Symptoms: UART RX dead, no `Please press Enter`, brcmnand command timeouts, `/proc/interrupts`
counters of peripherals stuck at 0 while the MIPS timer (IP7) works.

Debugging with `devmem` and CP0 `Cause`:

* L1 enable at `0x14e00040` + status at `0x14e00044` showed the UART pending, yet `Cause.IP`
  stayed 0, so that mask did not reach the thread we run on.
* Writing `0x12` to `0x14e00048` made `Cause = 0x00801000` -> **IP4** pending.

So on this SoC, running on TP0:

| Thread | Mask/status regs | CPU IRQ line |
|---|---|---|
| TP0 (Linux CPU0) | `0x14e00048` (+`0x350` for IRQs 32-63) | **IP4** |
| TP1 (Linux CPU1) | `0x14e00040` (+`0x348`) | **IP3** |

The previous DTS had them the other way round. Fixed with
`reg = <0x14e00048..>, <0x14e00040..>, <0x14e00350..>, <0x14e00348..>; interrupts = <4>, <3>;`.
After the fix `14e00500.serial` gets interrupts and the shell works.

## Default network role: host behind another router

The GPT-2541GNAC image is meant to sit on an existing LAN like a PC:

* `board.d/02_network`: `lan` = `eth0` (bridged as `br-lan`) with `proto dhcp`.
* `uci-defaults/99_gpt2541gnac_dhcp_client`: no DHCPv4/DHCPv6/RA service on `lan`
  (`dhcp.lan.ignore=1`), `lan6` = DHCPv6 client in the `lan` firewall zone, hostname
  `GPT-2541GNAC` (easy to spot in the upstream router's lease list).

There is no fixed address any more: find it in the upstream router's DHCP leases (or on the
serial console). Tested with a test DHCP server: lease, default route and DNS taken from it.

SSH keys are not part of the repository. For a local build, drop them in the build tree's
`files/` overlay:

```
files/etc/dropbear/authorized_keys          (mode 600)
files/etc/uci-defaults/98_dropbear_key_only (PasswordAuth/RootPasswordAuth off)
```

Without that, root has no password and SSH accepts anyone on the LAN: set one with `passwd`.

## Known issues / TODO

1. **PCIe** (`bcm6318-pcie` probe -2) and **hsspi** (probe -2): not supported yet.
2. **NAND install**: working, see "Installing to NAND" below. `sysupgrade` is not implemented for
   this layout yet: updates are done by hand (also below).

## Installing to NAND (dual image, OpenWrt in slot A)

Status (2026-10-09): OpenWrt boots from the NAND by itself (CFE -> `cferam.036` -> kernel -> UBI
squashfs + UBIFS overlay), 2 CPUs, `eth0` + DHCP. The stock firmware (image B) was removed on 2026-10-09 and its flash
space added to the UBI (see "Dropping the stock image B"); the original layout below is what
the board ships with.

**Back up first.** The whole NAND (with and without OOB) can be dumped from a running system by
enabling the DT-disabled controller; the DTS now has it `okay`. Keep `nand_ecc.bin`: every range
written below was compared with it before the first erase.

Broadcom layout of the 128 MB Spansion S34ML01G1 (page 2 KiB, 128 KiB blocks, BCH-4 with 16 OOB
bytes per 512 B, CFE keeps its own BBT in the last blocks):

| Range | Content | In the DTS |
|---|---|---|
| `0x0000000-0x0020000` | CFE ROM + NVRAM | `cfe`, read-only. **Never write.** |
| `0x0020000-0x0620000` | slot A bootfs (JFFS2 with `cferam.NNN` + kernel) | `bootfs` |
| `0x0620000-0x3d80000` | slot A rootfs (UBI) | stock layout; now part of `ubi` |
| `0x3d80000-0x42a0000` | slot B bootfs (stock) | removed, part of `ubi` |
| `0x42a0000-0x7ae0000` | slot B rootfs (stock) | removed, part of `ubi` |
| `0x7b00000-0x7f00000` | stock `data` (kept) | `stock_data`, read-only (renamed: patch 490 would auto-attach "data") |
| last 1 MB | CFE BBT | not mapped |

How CFE boots (from the CFE blob and the stock images):

* It mounts both bootfs JFFS2 partitions, which must end with the 256 byte marker
  `BcmFs-ubifs` + NUL byte, repeated 4 times, and boots the slot with the **highest `cferam.NNN`** (stock is `.035`, we
  use `.036`). Boot image `1` ("previous") in the CFE settings flips to the other slot: that is the
  way back to the stock firmware.
* `cferam` (the second stage) is byte for byte the stock one; it only accepts JFFS2 dirents with
  version > 0, so a dummy file `1-openwrt` goes first.
* The kernel is `vmlinux.lz4`: 20 byte big-endian header (load, entry, compressed length, `BRCM`,
  uncompressed length) and **one raw LZ4 block**. The stock kernel is not signed and CFE does not
  check it (the signature path only exists for the name `vmlinux.lz`, not used here).
* CFE and its own buffers occupy about `0x80a00000-0x82880000`, so a 10.8 MB kernel cannot be
  loaded at `0x80010000`. We pack OpenWrt's `relocate` loader + kernel + DTB with load/entry
  `0x83000000`; the loader copies the kernel down to `0x80010000`.

Images (`target/linux/bmips/bcm6838/nand-install/`): `mkslot.sh <stock cferam> 036` builds
`bootfs.bin` (6 MiB) and `ubi.bin` from a finished build (`brcmlz4.c` is the packer, needs the LZ4
sources; `jffs2x.py` and `brcm_lz4.py` check the result).

Writing slot A from a running system (the initramfs, never from the slot being written):

1. Boot into a system with the NAND DT enabled. Load `slota.ko` (`nand-install/slota.c`): it adds
   exactly two writable partitions, `slota_bootfs` and `slota_ubi`, on top of the read-only
   whole-chip device, so no other range can be touched.
2. Compare both ranges with the backup (md5), then write **UBI first, bootfs last**:
   `ubiformat /dev/mtdY -f ubi.bin -y`, test `ubiattach -m Y` (no `-O`), then
   `flash_erase /dev/mtdX 0 0 && nandwrite -p /dev/mtdX bootfs.bin`, read back and compare md5.
   The new `cferam` is the switch that makes CFE choose slot A.
3. Reboot. Check that CFE, cferam, kernel and UBI come up from serial.

Findings:

* `brcm,nand-oob-sector-size` must be `<16>` (the old DTS had 64). Controller values after CFE:
  ACC `0xe3441010`, CFG `0x15142200`. No `nand-on-flash-bbt`: Linux must not write a BBT over
  CFE's.
* Writes are page by page with ECC and read back exactly (md5 of the 6 MB bootfs and the UBI tail),
  0 bad blocks, 0 corrected bits in slot A.
* Do not leave a serial console typing at the CFE prompt while it autoboots: any key stops auto run.

### Runner needs more setup when CFE boots from flash

When the kernel comes from the TFTP command the CFE has just run its own network stack and left the
RDP block fully configured. Booting from flash skips that, and the first NAND boots had TX working
but **no RX at all** (`rx_packets 0`, DHCP never answered). `rdp_pre_init()`/`rdp_post_init()` of the
SDK (`bcm_misc_hw_init_impl3.c`) do more than releasing the soft resets, now done in `rdp_main.c` /
`rdp_net.c`:

* enable the three RDP UBUS masters (`0xb30d2000/2400/2800` bit 0) and the urgent->high priority
  forwarding of master 3 (`0xb30d280c |= 0xf0e01`);
* PMB `CHIP_CLKRST` registers `0xE = 0x33`, `0xF = 0xff`;
* the header-hold workaround `0xb200088c = 0x33`;
* `mac_hwapi_set_unimac_cfg()` (`gmii_direct`) for each EMAC.

With those in place RX works straight from a NAND boot.

Module load order: `kmod-bcm6838-rdp` is loaded at preinit from `/etc/modules-boot.d`, i.e. from
the read-only squashfs, **before the overlay is mounted**. A newer `.ko` copied to the overlay is
therefore ignored at that stage (and removing the `modules-boot.d` symlink on the overlay does not
help either). A temporary init script can `rmmod`/`insmod` the overlay copy before `network`, but
the real fix is a rebuilt image: the squashfs then carries the fixed module.

### Dropping the stock image B

Done once the stock firmware is not wanted any more (it can only be restored by writing the backup
back). The DTS then has a single big `ubi` partition, `0x620000` + `0x74c0000` (to the end of old
rootfs B, 116.75 MiB, 934 PEBs, 107 MiB of overlay), and no `stock_bootfs`/`stock_rootfs`:

1. In the initramfs, `wipeb.ko` (`nand-install/wipeb.c`) adds writable `bootfs_b`, `rootfs_b` and
   `ubi_big` partitions. Compare `bootfs_b`/`rootfs_b` with the backup, `flash_erase` both.
2. **CFE copes with a blank image B**: it prints `Booting from only image` and boots A, so the
   two images do not have to be present. Test this with a reboot after erasing only `bootfs_b`.
3. `ubiformat /dev/<ubi_big> -f ubi.bin -y` (ubi.bin carries `rootfs` and an autoresize
   `rootfs_data`), test `ubiattach`, then write the new `bootfs` of image A **last** (its kernel
   carries the new DTB). The old overlay is gone: first boot starts from the image defaults.
4. CFE/NVRAM, `data` and the BBT were compared with the backup afterwards and are unchanged.

### Updating the rootfs of slot A

The volume of the running system cannot be updated in place: `ubiupdatevol` fails with
`get_exclusive: 2 users for volume 0` because `ubiblock` holds the mounted root (nothing is written
in that case). Boot the initramfs from CFE instead (it uses no NAND), over a direct cable to the PC
(CFE is 192.168.1.1, which clashes with the main router on a shared LAN):

1. `r 192.168.1.100:<initramfs>` at the CFE prompt (the initramfs must have the NAND DTS).
2. The kernel auto-attaches `ubi` and creates `ubiblock0_0`; give the board an address, stop the
   firewall (the `lan` zone is not up without a DHCP lease) and copy the new `root.squashfs` over.
3. `ubiblock --remove /dev/ubi0_0`, then `ubiupdatevol /dev/ubi0_0 root.squashfs`, read the volume
   back and compare the md5. Only `rootfs` is touched: `rootfs_data` (the overlay with the
   configuration and keys) is kept and the bootfs does not change while the kernel is the same.
4. Reboot. Remove files left in the overlay that shadow the new rootfs (`rm` them in
   `/overlay/upper/...` directly, a plain `rm` of a file that exists in the rom creates a whiteout).

## CPU1 (TP1) - fixed, patch `902-bcm6838-boot-cpu1-with-shared-icache.patch`

The CFE parks TP1 like on the BCM6368 family, so the upstream `bcm63xx_fixup_cpu1()` path is
right: TP1 runs `bmips_smp_movevec`, sets its own relocated reset vector (`CBR+0x38000`,
`MIPS_TP1_ALT_BV` in the SDK, value `0xa0080000`) and waits for the boot IPI.

The hang at `SMP: Booting CPU1...` came from `bmips_smp_entry`: it wipes the I-cache tags
(`Index_Store_Tag_I` over 64 KB) before CPU1 runs cached code. On the 6838 both threads
**share** the L1 I-cache (BRCM config0 `0xe31f1406`, ICSHEN/DCSHEN set) and TP0 already
initialized it; the whole core freezes on CPU1's first cached fetch.

Found by single-stepping TP1 from TP0 (TP1 publishes a stage number in uncached RAM and waits,
TP0 prints it, hardware watchdog armed to recover): TP1 reached the jump to KSEG0 and never the
next instruction; skipping the wipe let it boot. The patch adds `bmips_cpu1_skip_icache_init`
and a `bcm6838_quirks()` that sets it before `bcm63xx_fixup_cpu1()`.

Tested: 2 CPUs, IPIs both ways, per-CPU timer, peripheral IRQs (UART, Runner) delivered on CPU1
through the TP1 L1 mask (IP3), both CPUs at 100% load while pinging without loss.

Dead ends on the way, for the record: PERF `AltBootConfig` (`0x14e002bc`) and releasing TP1
with CMT `RSTSE` (TP1 is not in reset, it is parked in CFE), copying config0/CBR to TP1.


## Ethernet (Runner) - working, package `kmod-bcm6838-rdp`

Status (2026-10-08): `eth0` up on the 4 LAN ports, LAN1 1000 Mb/s full duplex, ping to/from the
PC without loss. `eth0` joins OpenWrt's `br-lan` (192.168.1.1).

The 6838 has no DMA path from the switch to the CPU: the LAN UniMACs (`0x130d4000`) feed the
**Runner** network processor, which needs its microcode and DDR to forward packets to CPU rings.
`package/kernel/bcm6838-rdp` compiles the Broadcom GPL SDK 416L05 code for that (like CFE does for its TFTP), with
a small compatibility layer for Linux 6.6:

| Piece | Source |
|---|---|
| RDP power: release the 24 soft resets of PMB device 5 (`sr_control = 0xffffffff`) | `pmc6838.c` (PMC DQM mailbox, SDK `pmc/impl2` command table) |
| Data path init: Runner microcode, IH, BPM, SBPM, BBH, DMA for EMAC0..3 | SDK `drv/dpi/oren_data_path_init.c`, `rdp/*.c`, `rdd/*.c` built with `RDD_BASIC LEGACY_RDP OREN __OREN__`, `firmware_oren/*` |
| "Basic" forwarding: every CPU reason to CPU RX queue 0, unknown SA/DA to host | `f_initialize_basic_runner_parameters()` + `bridge_port_sa_da_cfg()` |
| RX: 128-entry CPU ring 0 registered in the Runner ring table, NAPI kicked by Runner interrupt 0 / sub-interrupt 0 (L1 hwirq 16, `rdd_interrupt_mask/clear/unmask`), 1 s safety timer; `rx_irq=0` falls back to timer polling | `rdp_net.c` |
| TX: `rdd_cpu_tx_write_eth_packet()` to every EMAC with link | `rdp_net.c` |
| PHY/MAC: `egphy_reset()` (quad EGPHY at MDIO 1..4), UniMAC init, speed/duplex from the PHY | SDK `drv/phys/egphy`, `drv/mdio`, `drv/unimac` |

Findings that matter:

* After CFE the RDP block is powered (zone 0 on) but all its soft resets are asserted; reading
  any RDP register before releasing them **hangs the bus** (watchdog reset ~25 s later).
* The 6838 PMC firmware has no `cmdRevision` (returns error 15) and needs `cmdTuneRunner` (69),
  which `rdd_init.c` calls.
* Runner DDR: TM 0x06600000 (25 MB) and MC 0x06200000 (4 MB), both 2 MB aligned, reserved in the
  DTS exactly like the stock kernel does.
* CPU RX descriptor (16 bytes, big endian): `word0[13:0]` length, `[18:14]` source port,
  `word2[31]` ownership (1 = host), `word2[28:0]` buffer address. Packet data starts at the
  buffer start.

Build: the package downloads the SDK tarball (`broadcom_sdk_416L05_pkg.tar.bz2`, checked by
SHA256), links the files of `src/sdk-files.txt` and builds `bcm6838_rdp.ko`. The device selects it
(`DEVICE_PACKAGES`), and it autoloads at boot with every stage enabled. For bring-up the stages
can be turned off with module parameters (`power_up=0`, `dp_init=0`, `net=0`, `probe=1`).
`src/build.sh` builds the same module outside the package against a built kernel tree.

The image needs the `reserved-memory` node of the board DTS (Runner DDR).

Also tested: `udhcpc -i eth0` gets a lease (DISCOVER/OFFER/REQUEST/ACK).

MAC address: `eth0` takes the base MAC of the board from the CFE NVRAM (read from the `cfe`
partition, `NVRAM_DATA` at flash offset `0x580`: `szBoardId` at +0x104, `ulNumMacAddrs` at +0x11c,
`ucaBaseMacAddr` at +0x120, checksum in the last 4 bytes of the 0x400 byte structure = raw
`crc32_le(~0, ...)` with the checksum field zeroed, which the driver verifies). The module
parameter `macaddr=` overrides it, and a random MAC is used (with a warning) if the NVRAM is not
readable, e.g. with the NAND disabled in the DTS. Every unit therefore keeps its own MAC.

Units that still have the factory NVRAM (base MAC `02:10:18:01:00:01`, GPON SN `BRCM12345678`)
would all share that placeholder MAC. `uci-defaults/97_bcm6838_default_mac` notices it at the first
boot and stores a random locally administered unicast MAC in `network.lan.macaddr`, so `br-lan`
gets its own address and keeps it across reboots (`eth0` itself keeps the NVRAM MAC).

Known limitation (not needed for the single-cable use case, left as is): there is one `eth0`
for the four LAN ports and TX is sent out of every port that has link (no per-port
`lan1..4` netdevs / DSA tagging). With one cable connected this is invisible.
