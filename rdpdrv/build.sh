#!/bin/sh
# Build the module inside the owrt-build container against the OpenWrt kernel.
#   build.sh            build everything
#   build.sh foo.o      build a single object (handy while porting)
set -e
cd "$(dirname "$0")"
TOP=/home/builder/openwrt-bcm6838
SDK=${SDK:-/home/builder/sdk416/sdk}
K=$(ls -d $TOP/build_dir/target-mips_mips32_musl/linux-bmips_bcm6838/linux-6.6.*)
TC=$(ls -d $TOP/staging_dir/toolchain-mips_mips32_gcc-*_musl)
export STAGING_DIR=$TOP/staging_dir

# kbuild wants the sources below M=, link the SDK files in as sdk/<name>.c
mkdir -p sdk
: > sdk-objs.txt
for f in $(cat sdk-files.txt); do
	ln -sf "$SDK/$f" "sdk/$(basename "$f")"
	echo "sdk/$(basename "$f")" >> sdk-objs.txt
done

make -C "$K" M="$PWD" SDK="$SDK" ARCH=mips \
	CROSS_COMPILE=$TC/bin/mips-openwrt-linux-musl- ${1:-modules}
[ -n "$1" ] || ls -l *.ko
