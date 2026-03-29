#!/bin/bash
# Build flashable eMMC image for secfirstNAS on UNVR
# Uses direct offset writes instead of loop devices (works in Docker without /dev/loop)
set -e

KERNEL="/input/Image"
DTB="/input/alpine-v2-ubnt-unvr.dtb"
MODULE="/input/al_eth.ko"
ROOTFS="/input/rootfs"
OUTPUT="/output/secfirstnas-unvr.img"

# Partition sizes in MB - matches Ubiquiti stock layout
P1_START=1       # kernel
P1_SIZE=128
P2_START=129     # rootfs
P2_SIZE=2048
P3_START=2177    # data
P3_SIZE=4096
P4_START=6273    # log
P4_SIZE=4096
P5_START=10369   # overlay
P5_SIZE=3072
IMG_SIZE=13450

echo "=== Creating sparse image (${IMG_SIZE}M) ==="
dd if=/dev/zero of="$OUTPUT" bs=1M count=0 seek=$IMG_SIZE 2>/dev/null

echo "=== Partitioning ==="
parted -s "$OUTPUT" mklabel gpt
parted -s "$OUTPUT" mkpart kernel  ext4 ${P1_START}MiB $((P1_START + P1_SIZE))MiB
parted -s "$OUTPUT" mkpart rootfs  ext4 ${P2_START}MiB $((P2_START + P2_SIZE))MiB
parted -s "$OUTPUT" mkpart data    ext4 ${P3_START}MiB $((P3_START + P3_SIZE))MiB
parted -s "$OUTPUT" mkpart log     ext4 ${P4_START}MiB $((P4_START + P4_SIZE))MiB
parted -s "$OUTPUT" mkpart overlay ext4 ${P5_START}MiB $((P5_START + P5_SIZE))MiB

echo "=== Creating individual partition images ==="

# boot1 - kernel + DTB
echo "--- boot1: kernel ---"
dd if=/dev/zero of=/tmp/p1.img bs=1M count=$P1_SIZE 2>/dev/null
mkfs.ext4 -q -L kernel /tmp/p1.img
mkdir -p /tmp/p1
mount /tmp/p1.img /tmp/p1
cp "$KERNEL" /tmp/p1/Image
cp "$DTB" /tmp/p1/alpine-v2-ubnt-unvr.dtb
ls -lh /tmp/p1/
umount /tmp/p1

# boot2 - rootfs
echo "--- boot2: rootfs ---"
dd if=/dev/zero of=/tmp/p2.img bs=1M count=$P2_SIZE 2>/dev/null
mkfs.ext4 -q -L rootfs /tmp/p2.img
mkdir -p /tmp/p2
mount /tmp/p2.img /tmp/p2
cp -a "$ROOTFS"/* /tmp/p2/

# Install kernel module
mkdir -p /tmp/p2/lib/modules/6.12.0/extra
cp "$MODULE" /tmp/p2/lib/modules/6.12.0/extra/al_eth.ko
depmod -b /tmp/p2 6.12.0 2>/dev/null || true

# Ensure mount points exist
mkdir -p /tmp/p2/data /tmp/p2/var/log /tmp/p2/boot

# Update fstab
cat > /tmp/p2/etc/fstab << 'EOF'
LABEL=rootfs    /           ext4    rw,relatime     0 1
LABEL=kernel    /boot       ext4    ro,relatime     0 2
LABEL=data      /data       ext4    rw,relatime     0 2
LABEL=log       /var/log    ext4    rw,relatime     0 2
devtmpfs        /dev        devtmpfs defaults       0 0
proc            /proc       proc    defaults        0 0
sysfs           /sys        sysfs   defaults        0 0
tmpfs           /tmp        tmpfs   nosuid,nodev    0 0
EOF

echo "Rootfs size:"
du -sh /tmp/p2
umount /tmp/p2

# boot3 - data
echo "--- boot3: data ---"
dd if=/dev/zero of=/tmp/p3.img bs=1M count=$P3_SIZE 2>/dev/null
mkfs.ext4 -q -L data /tmp/p3.img
mkdir -p /tmp/p3
mount /tmp/p3.img /tmp/p3
mkdir -p /tmp/p3/samba /tmp/p3/config /tmp/p3/sfgw
umount /tmp/p3

# boot4 - log
echo "--- boot4: log ---"
dd if=/dev/zero of=/tmp/p4.img bs=1M count=$P4_SIZE 2>/dev/null
mkfs.ext4 -q -L log /tmp/p4.img

# boot5 - overlay
echo "--- boot5: overlay ---"
dd if=/dev/zero of=/tmp/p5.img bs=1M count=$P5_SIZE 2>/dev/null
mkfs.ext4 -q -L overlay /tmp/p5.img

echo "=== Writing partitions to image ==="
dd if=/tmp/p1.img of="$OUTPUT" bs=1M seek=$P1_START conv=notrunc 2>/dev/null
dd if=/tmp/p2.img of="$OUTPUT" bs=1M seek=$P2_START conv=notrunc 2>/dev/null
dd if=/tmp/p3.img of="$OUTPUT" bs=1M seek=$P3_START conv=notrunc 2>/dev/null
dd if=/tmp/p4.img of="$OUTPUT" bs=1M seek=$P4_START conv=notrunc 2>/dev/null
dd if=/tmp/p5.img of="$OUTPUT" bs=1M seek=$P5_START conv=notrunc 2>/dev/null

echo "=== Compressing ==="
gzip -1 -f "$OUTPUT"

echo ""
echo "========================================="
echo "  secfirstNAS UNVR Image Build Complete"
echo "========================================="
ls -lh "${OUTPUT}.gz"
echo ""
echo "To flash (via UART + recovery, or from running system):"
echo "  1. Copy to device: scp secfirstnas-unvr.img.gz root@10.0.0.116:/tmp/"
echo "  2. SSH in: ssh root@10.0.0.116"
echo "  3. Flash: gunzip /tmp/secfirstnas-unvr.img.gz && dd if=/tmp/secfirstnas-unvr.img of=/dev/boot bs=4M && sync"
echo "  4. Set U-Boot env (via UART or fw_setenv):"
echo "     setenv rootfs PARTLABEL=rootfs"
echo "     setenv bootargsextra boot=local rw"
echo "     saveenv"
echo "  5. Reboot"
echo ""
echo "Recovery: Hold reset button while powering on -> telnet -> restore factory firmware"
