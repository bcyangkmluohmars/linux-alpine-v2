#!/bin/bash
# Build Alpine Linux aarch64 rootfs for secfirstNAS on UNVR
set -e

ROOTFS_DIR="/build/rootfs"
ALPINE_VERSION="3.23"
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine"

echo "=== Creating Alpine Linux rootfs ==="
mkdir -p "$ROOTFS_DIR"

# Download and extract Alpine minirootfs
MINIROOTFS="alpine-minirootfs-${ALPINE_VERSION}.0-aarch64.tar.gz"
wget -q "${ALPINE_MIRROR}/v${ALPINE_VERSION}/releases/aarch64/${MINIROOTFS}" -O /tmp/minirootfs.tar.gz
tar xzf /tmp/minirootfs.tar.gz -C "$ROOTFS_DIR"

# Configure APK repos
cat > "$ROOTFS_DIR/etc/apk/repositories" << EOF
${ALPINE_MIRROR}/v${ALPINE_VERSION}/main
${ALPINE_MIRROR}/v${ALPINE_VERSION}/community
EOF

# Install packages via qemu-aarch64
cp /usr/bin/qemu-aarch64-static "$ROOTFS_DIR/usr/bin/" 2>/dev/null || true

chroot "$ROOTFS_DIR" /bin/sh -c '
apk update && apk install --no-cache \
  openrc \
  busybox-initscripts \
  openssh \
  openssh-server \
  mdadm \
  cryptsetup \
  lvm2 \
  btrfs-progs \
  e2fsprogs \
  samba \
  rsync \
  smartmontools \
  hdparm \
  lsblk \
  parted \
  util-linux \
  kmod \
  eudev \
  iproute2 \
  iptables \
  ip6tables \
  nftables \
  dhcpcd \
  chrony \
  doas \
  ca-certificates \
  curl \
  jq \
  i2c-tools \
  && \
# Enable essential services
rc-update add devfs sysinit && \
rc-update add dmesg sysinit && \
rc-update add mdev sysinit && \
rc-update add hwclock boot && \
rc-update add modules boot && \
rc-update add sysctl boot && \
rc-update add hostname boot && \
rc-update add bootmisc boot && \
rc-update add networking boot && \
rc-update add syslog boot && \
rc-update add sshd default && \
rc-update add chronyd default && \
rc-update add mount-ro shutdown && \
rc-update add killprocs shutdown && \
rc-update add savecache shutdown && \
# Set hostname
echo "secfirstnas" > /etc/hostname && \
# Configure networking (DHCP on enp0s1)
cat > /etc/network/interfaces << NETEOF
auto lo
iface lo inet loopback

auto enp0s1
iface enp0s1 inet dhcp
NETEOF
# Set root password (temporary, will be changed on first boot)
echo "root:secfirstnas" | chpasswd && \
# Allow root SSH (temporary for dev)
sed -i "s/#PermitRootLogin.*/PermitRootLogin yes/" /etc/ssh/sshd_config && \
# Console on serial
echo "ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100" >> /etc/inittab && \
# fstab
cat > /etc/fstab << FSTABEOF
/dev/boot2      /               ext4    rw,relatime     0 1
/dev/boot3      /data           ext4    rw,relatime     0 2
/dev/boot4      /var/log        ext4    rw,relatime     0 2
proc            /proc           proc    defaults        0 0
sysfs           /sys            sysfs   defaults        0 0
devtmpfs        /dev            devtmpfs defaults       0 0
tmpfs           /tmp            tmpfs   nosuid,nodev    0 0
FSTABEOF
# Module loading
mkdir -p /etc/modules-load.d && \
echo "al_eth" > /etc/modules-load.d/al_eth.conf
'

# Remove qemu
rm -f "$ROOTFS_DIR/usr/bin/qemu-aarch64-static"

# Create data directories
mkdir -p "$ROOTFS_DIR/data"
mkdir -p "$ROOTFS_DIR/var/log"

echo "=== Rootfs build complete ==="
du -sh "$ROOTFS_DIR"
