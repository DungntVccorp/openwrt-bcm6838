# SPDX-License-Identifier: GPL-2.0-or-later

define Device/mitrastar_gpt-2541gnac
  $(Device/bcm63xx-cfe-nand)
  DEVICE_VENDOR := Mitrastar
  DEVICE_MODEL := GPT-2541GNAC
  DEVICE_LOADADDR := $(KERNEL_LOADADDR)
  CFE_BOARD_ID := GPT-2541GNAC
  CHIP_ID := 6838
  SOC := bcm6838
  BLOCKSIZE := 128k
  PAGESIZE := 2048
  SUBPAGESIZE := 512
  VID_HDR_OFFSET := 2048
  DEVICE_PACKAGES += $(USB2_PACKAGES) \
    kmod-leds-bcm6328 kmod-bcm6838-rdp
  IMAGES :=
endef
TARGET_DEVICES += mitrastar_gpt-2541gnac

