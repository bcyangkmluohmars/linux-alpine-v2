#!/bin/bash
# Build Alpine Linux aarch64 rootfs for secfirstGW on UDM Pro
# Uses docker buildx with native aarch64 emulation.
#
# Usage:
#   ./build-rootfs-udmpro.sh
#
# Outputs:
#   /tmp/output-udmpro/alpine-rootfs-udmpro.tar.gz
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_DIR="${SCRIPT_DIR}/../kernel/build"
GW_BINARY="${SCRIPT_DIR}/../../secfirstgw-rs/target/aarch64-unknown-linux-musl/release/sfgw"
OUTPUT_DIR="/tmp/output-udmpro"
SSH_KEYS="${HOME}/.ssh/id_ed25519.pub"
ROOT_PASSWORD="secfirstgw"

mkdir -p "${OUTPUT_DIR}"

# ─── Step 1: Build rootfs in aarch64 Docker container ──────────────────────

echo ">>> Step 1: Building aarch64 Alpine rootfs..."

cat > /tmp/Dockerfile.udmpro-rootfs << 'DOCKERFILE'
FROM --platform=linux/arm64 alpine:3.21

RUN apk update && apk add --no-cache \
  openrc \
  openssh \
  openssh-server \
  e2fsprogs \
  util-linux \
  kmod \
  eudev \
  iproute2 \
  iptables \
  ip6tables \
  nftables \
  bridge-utils \
  dhcpcd \
  dnsmasq \
  chrony \
  doas \
  ca-certificates \
  curl \
  jq \
  i2c-tools \
  wireguard-tools \
  tcpdump \
  ethtool \
  smartmontools \
  parted \
  sgdisk

# OpenRC services (symlinks — rc-update doesn't work in Docker)
RUN mkdir -p /etc/runlevels/sysinit /etc/runlevels/boot /etc/runlevels/default /etc/runlevels/shutdown && \
    ln -s /etc/init.d/devfs      /etc/runlevels/sysinit/ && \
    ln -s /etc/init.d/dmesg      /etc/runlevels/sysinit/ && \
    ln -s /etc/init.d/mdev       /etc/runlevels/sysinit/ && \
    ln -s /etc/init.d/hwclock    /etc/runlevels/boot/ && \
    ln -s /etc/init.d/modules    /etc/runlevels/boot/ && \
    ln -s /etc/init.d/sysctl     /etc/runlevels/boot/ && \
    ln -s /etc/init.d/hostname   /etc/runlevels/boot/ && \
    ln -s /etc/init.d/bootmisc   /etc/runlevels/boot/ && \
    ln -s /etc/init.d/networking /etc/runlevels/boot/ && \
    ln -s /etc/init.d/syslog     /etc/runlevels/boot/ && \
    ln -s /etc/init.d/sshd       /etc/runlevels/default/ && \
    ln -s /etc/init.d/chronyd    /etc/runlevels/default/ && \
    ln -s /etc/init.d/mount-ro   /etc/runlevels/shutdown/ && \
    ln -s /etc/init.d/killprocs  /etc/runlevels/shutdown/ && \
    ln -s /etc/init.d/savecache  /etc/runlevels/shutdown/

# SSH config
RUN mkdir -p /etc/ssh && \
    sed -i 's/#PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config && \
    sed -i 's/#PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Fix sshd /var/empty permissions
RUN mkdir -p /var/empty && chmod 755 /var/empty && chown root:root /var/empty

# Serial console
RUN sed -i '/^ttyS0/d' /etc/inittab && \
    echo "ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100" >> /etc/inittab

# fstab (UDM Pro eMMC: sda1=kernel, sda3=rootfs, sda4=log, sda5=data)
RUN printf '/dev/sda3\t/\text4\trw,relatime\t0 1\n\
/dev/sda4\t/var/log\text4\trw,relatime\t0 2\n\
/dev/sda5\t/data\text4\trw,relatime\t0 2\n\
proc\t/proc\tproc\tdefaults\t0 0\n\
sysfs\t/sys\tsysfs\tdefaults\t0 0\n\
devtmpfs\t/dev\tdevtmpfs\tdefaults\t0 0\n\
tmpfs\t/tmp\ttmpfs\tnosuid,nodev\t0 0\n' > /etc/fstab

# Network: DHCP on eth0 (WAN RJ45)
RUN printf 'auto lo\niface lo inet loopback\n\nauto eth0\niface eth0 inet dhcp\n' \
    > /etc/network/interfaces

# IP forwarding (gateway)
RUN mkdir -p /etc/sysctl.d && \
    printf 'net.ipv4.ip_forward = 1\nnet.ipv6.conf.all.forwarding = 1\n' \
    > /etc/sysctl.d/01-gateway.conf

# Create dirs
RUN mkdir -p /data /var/log /root/.ssh && chmod 700 /root/.ssh

CMD ["/bin/sh"]
DOCKERFILE

docker buildx build \
  --platform linux/arm64 \
  -t udmpro-rootfs \
  -f /tmp/Dockerfile.udmpro-rootfs \
  --load \
  .

# ─── Step 2: Extract rootfs from container ─────────────────────────────────

echo ">>> Step 2: Extracting rootfs..."

CONTAINER_ID=$(docker create --platform linux/arm64 udmpro-rootfs)
docker export "${CONTAINER_ID}" | gzip > "${OUTPUT_DIR}/alpine-rootfs-udmpro.tar.gz"
docker rm "${CONTAINER_ID}" > /dev/null

# ─── Step 3: Post-export fixes (Docker-managed files) ──────────────────────

echo ">>> Step 3: Post-export fixes..."

TMPROOT=$(mktemp -d)
cd "${TMPROOT}"
tar xzf "${OUTPUT_DIR}/alpine-rootfs-udmpro.tar.gz"

# --- Fix Docker-managed files ---
echo "secfirstgw" > etc/hostname
echo "127.0.0.1 secfirstgw localhost" > etc/hosts
echo "nameserver 1.1.1.1" > etc/resolv.conf

# --- Fix root password (write shadow directly, chpasswd unreliable in Docker) ---
# Generate password hash using openssl in a quick container
HASH=$(docker run --rm alpine:3.21 sh -c "apk add --no-cache openssl >/dev/null 2>&1 && openssl passwd -6 '${ROOT_PASSWORD}'")
sed -i "s|^root:.*|root:${HASH}:0:0:::::|" etc/shadow
echo "  Password hash set"

# --- Fix /etc/modules (Alpine uses /etc/modules, not modules-load.d) ---
printf 'al_eth\nal_dma\nal_ssm\nal_sgpo\n' > etc/modules
# Also keep modules-load.d for systemd compat
mkdir -p etc/modules-load.d
printf 'al_eth\nal_dma\nal_ssm\nal_sgpo\n' > etc/modules-load.d/secfirstgw.conf

# --- Fix /var/empty for sshd ---
mkdir -p var/empty
chmod 755 var/empty

# --- Kernel modules ---
if [ -d "${KERNEL_DIR}/rootfs/lib/modules" ]; then
  rm -rf lib/modules
  cp -a "${KERNEL_DIR}/rootfs/lib/modules" lib/
  [ -f "${KERNEL_DIR}/al_sgpo.ko" ] && cp "${KERNEL_DIR}/al_sgpo.ko" lib/modules/*/extra/ 2>/dev/null || true
  echo "  Modules: $(ls lib/modules/*/extra/ 2>/dev/null)"
else
  echo "  WARNING: No kernel modules found at ${KERNEL_DIR}/rootfs/lib/modules"
fi

# --- sfgw binary ---
if [ -f "${GW_BINARY}" ]; then
  mkdir -p usr/local/bin
  cp "${GW_BINARY}" usr/local/bin/sfgw
  chmod 755 usr/local/bin/sfgw
  echo "  sfgw: $(du -h usr/local/bin/sfgw | cut -f1)"
else
  echo "  WARNING: sfgw binary not found at ${GW_BINARY}"
fi

# --- SSH authorized keys ---
if [ -f "${SSH_KEYS}" ]; then
  mkdir -p root/.ssh
  chmod 700 root/.ssh
  cp "${SSH_KEYS}" root/.ssh/authorized_keys
  chmod 600 root/.ssh/authorized_keys
  echo "  SSH key installed"
else
  echo "  WARNING: SSH key not found at ${SSH_KEYS}"
fi

# ─── Step 4: Repack ────────────────────────────────────────────────────────

echo ">>> Step 4: Repacking rootfs..."
tar czf "${OUTPUT_DIR}/alpine-rootfs-udmpro.tar.gz" .
cd /
rm -rf "${TMPROOT}"

echo ""
echo "============================================"
echo "  secfirstGW rootfs build complete"
echo "============================================"
echo ""
echo "  Output: ${OUTPUT_DIR}/alpine-rootfs-udmpro.tar.gz"
echo "  Size:   $(du -h ${OUTPUT_DIR}/alpine-rootfs-udmpro.tar.gz | cut -f1)"
echo ""
echo "  Root password: ${ROOT_PASSWORD}"
echo "  SSH:           key-based (+ password on serial)"
echo "  Serial:        ttyS0 @ 115200"
echo "  Modules:       al_eth, al_dma, al_ssm, al_sgpo"
echo ""
echo "  Flash to UDM Pro (from recovery shell):"
echo "    ip addr add 10.0.0.1/24 dev eth0 && ip link set eth0 up"
echo "    mkfs.ext4 -F /dev/boot3"
echo "    mount /dev/boot3 /mnt && cd /mnt"
echo "    wget -O /tmp/r.tar.gz http://10.0.0.10:9000/alpine-rootfs-udmpro.tar.gz"
echo "    tar xzf /tmp/r.tar.gz && sync && cd / && umount /mnt"
echo "    reboot"
echo ""
