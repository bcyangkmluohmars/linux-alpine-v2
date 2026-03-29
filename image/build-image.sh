#!/bin/bash
# Build flashable eMMC image for secfirstNAS on UNVR
# Partition layout matches Ubiquiti stock layout for U-Boot compatibility:
#   boot1 (128M)  - kernel (Image + DTB)
#   boot2 (2G)    - rootfs (Alpine Linux)
#   boot3 (4G)    - /data (persistent config, databases)
#   boot4 (4G)    - /var/log
#   boot5 (rest)  - overlay / future use
set -e

KERNEL="/input/Image"
DTB="/input/alpine-v2-ubnt-unvr.dtb"
MODULE="/input/al_eth.ko"
ROOTFS="/input/rootfs"
OUTPUT="/output/secfirstnas-unvr.img"

IMG_SIZE_MB=14336  # ~14 GB (leave room on 29 GB eMMC)
BOOT1_SIZE_MB=128
BOOT2_SIZE_MB=2048
BOOT3_SIZE_MB=4096
BOOT4_SIZE_MB=4096
BOOT5_SIZE_MB=3072

echo "=== Creating eMMC image ==="
dd if=/dev/zero of="$OUTPUT" bs=1M count=0 seek=$IMG_SIZE_MB 2>/dev/null

echo "=== Partitioning ==="
parted -s "$OUTPUT" mklabel gpt
parted -s "$OUTPUT" mkpart kernel ext4 1MiB ${BOOT1_SIZE_MB}MiB
parted -s "$OUTPUT" mkpart rootfs ext4 ${BOOT1_SIZE_MB}MiB $((BOOT1_SIZE_MB + BOOT2_SIZE_MB))MiB
parted -s "$OUTPUT" mkpart data ext4 $((BOOT1_SIZE_MB + BOOT2_SIZE_MB))MiB $((BOOT1_SIZE_MB + BOOT2_SIZE_MB + BOOT3_SIZE_MB))MiB
parted -s "$OUTPUT" mkpart log ext4 $((BOOT1_SIZE_MB + BOOT2_SIZE_MB + BOOT3_SIZE_MB))MiB $((BOOT1_SIZE_MB + BOOT2_SIZE_MB + BOOT3_SIZE_MB + BOOT4_SIZE_MB))MiB
parted -s "$OUTPUT" mkpart overlay ext4 $((BOOT1_SIZE_MB + BOOT2_SIZE_MB + BOOT3_SIZE_MB + BOOT4_SIZE_MB))MiB $((BOOT1_SIZE_MB + BOOT2_SIZE_MB + BOOT3_SIZE_MB + BOOT4_SIZE_MB + BOOT5_SIZE_MB))MiB
parted -s "$OUTPUT" print

# Setup loop device
LOOP=$(losetup --find --show --partscan "$OUTPUT")
echo "Loop device: $LOOP"

# Wait for partition devices
sleep 1
partprobe "$LOOP"
sleep 1

echo "=== Formatting partitions ==="
mkfs.ext4 -q -L kernel  "${LOOP}p1"
mkfs.ext4 -q -L rootfs  "${LOOP}p2"
mkfs.ext4 -q -L data    "${LOOP}p3"
mkfs.ext4 -q -L log     "${LOOP}p4"
mkfs.ext4 -q -L overlay "${LOOP}p5"

# Mount rootfs partition
MOUNT_ROOT="/mnt/rootfs"
MOUNT_KERNEL="/mnt/kernel"
MOUNT_DATA="/mnt/data"
mkdir -p "$MOUNT_ROOT" "$MOUNT_KERNEL" "$MOUNT_DATA"

echo "=== Populating kernel partition (boot1) ==="
mount "${LOOP}p1" "$MOUNT_KERNEL"
cp "$KERNEL" "$MOUNT_KERNEL/Image"
cp "$DTB" "$MOUNT_KERNEL/alpine-v2-ubnt-unvr.dtb"
ls -lh "$MOUNT_KERNEL/"
umount "$MOUNT_KERNEL"

echo "=== Populating rootfs partition (boot2) ==="
mount "${LOOP}p2" "$MOUNT_ROOT"
cp -a "$ROOTFS"/* "$MOUNT_ROOT/"

# Install kernel module
KVER="6.12"
mkdir -p "$MOUNT_ROOT/lib/modules/$KVER/extra"
cp "$MODULE" "$MOUNT_ROOT/lib/modules/$KVER/extra/al_eth.ko"

# Generate modules.dep
chroot "$MOUNT_ROOT" /bin/sh -c "depmod -a $KVER 2>/dev/null" || true

# Create data mount point
mkdir -p "$MOUNT_ROOT/data" "$MOUNT_ROOT/var/log"

# Update fstab with partition labels
cat > "$MOUNT_ROOT/etc/fstab" << 'EOF'
LABEL=rootfs    /           ext4    rw,relatime     0 1
LABEL=data      /data       ext4    rw,relatime     0 2
LABEL=log       /var/log    ext4    rw,relatime     0 2
devtmpfs        /dev        devtmpfs defaults       0 0
proc            /proc       proc    defaults        0 0
sysfs           /sys        sysfs   defaults        0 0
tmpfs           /tmp        tmpfs   nosuid,nodev    0 0
EOF

echo "Rootfs contents:"
du -sh "$MOUNT_ROOT"
umount "$MOUNT_ROOT"

echo "=== Populating data partition (boot3) ==="
mount "${LOOP}p3" "$MOUNT_DATA"
mkdir -p "$MOUNT_DATA/samba" "$MOUNT_DATA/config"
umount "$MOUNT_DATA"

echo "=== Cleanup ==="
losetup -d "$LOOP"

echo "=== Compressing ==="
gzip -f "$OUTPUT"

echo "=== DONE ==="
ls -lh "${OUTPUT}.gz"
echo ""
echo "Flash with:"
echo "  gunzip ${OUTPUT}.gz"
echo "  dd if=secfirstnas-unvr.img of=/dev/boot bs=4M"
echo "  sync && reboot"
