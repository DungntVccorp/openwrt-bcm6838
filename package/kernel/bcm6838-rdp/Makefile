# SPDX-License-Identifier: GPL-2.0-only
#
# BCM6838 Runner ethernet, built from the Broadcom GPL SDK 416L05 sources
# plus the glue in src/ (see BRINGUP.md).

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=bcm6838-rdp
PKG_VERSION:=416L05
PKG_RELEASE:=1

PKG_SOURCE:=broadcom_sdk_$(PKG_VERSION)_pkg.tar.bz2
PKG_SOURCE_URL:=http://www.datashed.science/misc/bcm/gpl/
PKG_HASH:=56767dadfc396acc6886df6d9dec66548a243b58a5c67e842750dc1b5e94ef80
PKG_LICENSE:=GPL-2.0-only

PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_UNPACK=$(HOST_TAR) -C $(PKG_BUILD_DIR) --strip-components=1 -xjf $(DL_DIR)/$(PKG_SOURCE)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/bcm6838-rdp
  SUBMENU:=Network Devices
  TITLE:=BCM6838 Runner (RDP) ethernet
  DEPENDS:=@TARGET_bmips_bcm6838
  FILES:=$(PKG_BUILD_DIR)/drv/bcm6838_rdp.ko
  AUTOLOAD:=$(call AutoLoad,30,bcm6838_rdp,1)
endef

define KernelPackage/bcm6838-rdp/description
  Brings up the BCM6838 Runner network processor the way CFE does
  (basic data path, one CPU ring) and registers eth0 on the LAN ports.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	mkdir -p $(PKG_BUILD_DIR)/drv
	$(CP) ./src/* $(PKG_BUILD_DIR)/drv/
	sh $(PKG_BUILD_DIR)/drv/link-sdk.sh $(PKG_BUILD_DIR)
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)/drv" SDK="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,bcm6838-rdp))
