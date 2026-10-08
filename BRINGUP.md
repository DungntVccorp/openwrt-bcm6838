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

## Known issues / TODO

1. **PCIe** (`bcm6318-pcie` probe -2) and **hsspi** (probe -2): not supported yet.
2. **NAND**: kept disabled (see above).

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

Known limitation (not needed for the single-cable use case, left as is): there is one `eth0`
for the four LAN ports and TX is sent out of every port that has link (no per-port
`lan1..4` netdevs / DSA tagging). With one cable connected this is invisible.
