#!/bin/sh
# Build the slot A (stock "image_update") images for the GPT-2541GNAC:
#   bootfs.bin : 6 MiB JFFS2 (cferam.036 + vmlinux.lz4) + "BcmFs-ubifs" marker
#   ubi.bin    : UBI with rootfs (squashfs) + rootfs_data
# usage: mkslot.sh <stock cferam file> <cferam number>
set -e
cd "$(dirname "$0")"
T=/home/builder/openwrt-bcm6838
K=$T/build_dir/target-mips_mips32_musl/linux-bmips_bcm6838
TC=$(ls -d $T/staging_dir/toolchain-mips_mips32_gcc-*_musl)
export STAGING_DIR=$T/staging_dir
export PATH=$T/staging_dir/host/bin:$PATH

CFERAM=${1:?stock cferam}
SEQ=${2:-036}
BOOTFS_SIZE=$((0x600000))
BLOCK=$((0x20000))
LOADER=0x83000000

# 1. kernel + DTB behind the relocation loader, packed like the stock vmlinux.lz4
rm -rf relocate
cp -r $T/target/linux/generic/image/relocate .
make -s -C relocate CACHELINE_SIZE=16 CROSS_COMPILE=$TC/bin/mips-openwrt-linux-musl- \
	KERNEL_ADDR=0x80010000 LZMA_TEXT_START=$LOADER >/dev/null
cat $K/vmlinux $K/image-bcm6838-mitrastar-gpt-2541gnac.dtb > kernel_dtb.bin
SZ=$(stat -c %s kernel_dtb.bin)
( dd if=relocate/loader.bin bs=32 conv=sync 2>/dev/null
  perl -e "print pack('N', $SZ)"
  cat kernel_dtb.bin ) > kernel_reloc.bin
./brcmlz4 kernel_reloc.bin vmlinux.lz4 $LOADER $LOADER

# 2. bootfs JFFS2. CFE ignores dirents with version 0, so a dummy file sorts
#    first and takes it (same trick as OpenWrt's cfe-jffs2-cferam).
rm -rf bootfs.dir && mkdir -p bootfs.dir/etc
touch bootfs.dir/1-openwrt
cp "$CFERAM" bootfs.dir/cferam.$SEQ
cp vmlinux.lz4 bootfs.dir/vmlinux.lz4
printf 'OpenWrt\n' > bootfs.dir/image_version
mkfs.jffs2 --big-endian --pad --no-cleanmarkers --eraseblock=128KiB \
	--compression-mode=none --root=bootfs.dir --output=bootfs.jffs2
JSZ=$(stat -c %s bootfs.jffs2)
[ $JSZ -le $((BOOTFS_SIZE - BLOCK)) ] || { echo "bootfs too big: $JSZ"; exit 1; }

# 3. pad to the partition and put the stock end marker in the last 256 bytes
python3 - "$BOOTFS_SIZE" <<'EOF'
import sys
size = int(sys.argv[1])
j = open('bootfs.jffs2', 'rb').read()
img = bytearray(b'\xff' * size)
img[:len(j)] = j
img[size - 0x100:size - 0x100 + 48] = b'BcmFs-ubifs\x00' * 4
open('bootfs.bin', 'wb').write(img)
EOF

# 4. UBI: rootfs (squashfs) + rootfs_data, PEB 128 KiB, page 2 KiB, VID at 2 KiB
TOPDIR=$T sh $T/scripts/ubinize-image.sh --rootfs $K/root.squashfs ubi.bin \
	-p 128KiB -m 2048 -O 2048 -E 5

ls -l vmlinux.lz4 bootfs.jffs2 bootfs.bin ubi.bin
md5sum bootfs.bin ubi.bin
