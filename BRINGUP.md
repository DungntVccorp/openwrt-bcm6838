# BCM6838 / Mitrastar GPT-2541GNAC - initramfs bring-up notes

Status (2026-10-07): OpenWrt 6.6 **initramfs boots to an interactive shell** over UART
when loaded through CFE + TFTP. Ethernet is **not** working (no driver), single CPU only.

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
| `bootargs = "earlycon console=ttyS0,115200 maxcpus=1"` | console on UART0; CPU1 bring-up hangs (see below). |
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

1. **CPU1 hang**: with SMP the log stops at `SMP: Booting CPU1...`. Workaround `maxcpus=1`.
   Not investigated since the IRQ fix (the earlier IRQ misrouting may or may not be related);
   retry without `maxcpus=1`.
2. **PCIe** (`bcm6318-pcie` probe -2) and **hsspi** (probe -2): not supported yet.
3. **Ethernet** - see below.

## Ethernet analysis (not implemented)

Stock firmware prints `Broadcom BCM68380_B0 Ethernet Network Device`, 4 ports eth0-3,
PHY ids `0x0180000x`, base MAC in CFE `p`.

From the Broadcom GPL SDK 416L05 (`bcmdrivers/opensource/net/enet/impl5`,
`shared/opensource/include/bcm963xx/6838_map_part.h`):

* The 6838 builds `bcmenet` in **Runner/RDPA** mode (`bcmenet_runner.o`, `ethsw_runner.o`,
  `bcmsw_runner.o`), not the plain DMA mode.
* LAN ports sit on **UniMAC** blocks at `0x130d4000` (RDP), packets reach the CPU through the
  Runner into CPU rings in RAM.
* Other bases: PERF `0x14e00000`, TIMER `0x14e000c0`, GPIO `0x14e00100`, MDIO ext `0x14e00600`
  (EGPHY `+0x10`), LED `0x14e00f00`, HS-SPI `0x14e01000`, NAND `0x14e02200`,
  USB EHCI/OHCI `0x15400300/0x15400400`, PCIe0 `0x12800000`, PCIe1 `0x12a00000`.
* The Runner microcode is published as C arrays (`shared/broadcom/rdp/impl1/firmware_oren/runner_fw_{a,b,c,d}.c`)
  and the init code as source (`rdd_init.c`, `rdp_drv_{ih,bbh,bpm,sbpm}.c`, `rdp_cpu_ring.c`),
  roughly 1-1.5 MB of code written for kernel 3.4.

Options: (a) USB Ethernet dongle (EHCI/OHCI at `0x15400300` are in the stock log) - quickest;
(b) port a minimal Runner init + one CPU ring + PHY, in stages: PMC/clock bring-up and UniMAC
register read, Runner init, CPU ring + PHY, netdev + DHCP.
