#!/bin/bash
# build-rootfs-gw.sh — Complete secfirstGW rootfs build (UDM Pro)
#
# Builds EVERYTHING from source in ONE script:
#   1. Cross-compiles Linux 6.12.77 kernel + DTB + all modules (UDM Pro defconfig)
#   2. Builds sfgw binary natively on aarch64 (Docker buildx)
#   3. Builds Alpine Linux 3.21 aarch64 rootfs with all GW packages
#   4. Combines everything into ready-to-flash rootfs tarball
#
# Does NOT use any pre-built modules from the NAS/UNVR build.
# Everything is built fresh from source.
#
# Requirements:
#   - Docker with buildx
#   - qemu-user-static registered (for aarch64 emulation)
#   - SELinux: all volume mounts use :z
#
# Usage:
#   ./build-rootfs-gw.sh              # Full build
#   ./build-rootfs-gw.sh --modules    # Rebuild only out-of-tree modules + rootfs
#
set -euo pipefail
MODULES_ONLY="${1:-}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "${SCRIPT_DIR}")"

# ─── Paths ────────────────────────────────────────────────────────────────────
KERNEL_SRC="${PROJECT_ROOT}/kernel/reference/linux-6.12-stable"
KERNEL_CONFIGS="${PROJECT_ROOT}/kernel/configs"
KERNEL_MODULES_SRC="${PROJECT_ROOT}/kernel/modules"
GW_SRC="${PROJECT_ROOT}/../secfirstgw-rs"
SSH_KEYS="${HOME}/.ssh/id_ed25519.pub"
OUTPUT_DIR="${PROJECT_ROOT}/rootfs/output-gw"

KVER="6.12.77"
ROOT_PASSWORD="secfirstgw"

# Colors
if [ -t 1 ]; then
    G='\033[0;32m' Y='\033[1;33m' R='\033[0;31m' N='\033[0m'
else
    G='' Y='' R='' N=''
fi
log()  { echo -e "${G}[gw]${N} $*"; }
warn() { echo -e "${Y}[gw]${N} $*"; }
err()  { echo -e "${R}[gw]${N} $*" >&2; exit 1; }

# ─── Preflight ────────────────────────────────────────────────────────────────
log "Preflight checks..."

[ -d "${KERNEL_SRC}" ]         || err "Kernel source not found: ${KERNEL_SRC}"
[ -f "${KERNEL_CONFIGS}/udmpro_defconfig" ] || err "UDM Pro defconfig not found"
[ -d "${KERNEL_MODULES_SRC}/al_eth" ]  || err "al_eth module source not found"
[ -d "${KERNEL_MODULES_SRC}/al_dma" ]  || err "al_dma module source not found"
[ -d "${KERNEL_MODULES_SRC}/al_ssm" ]  || err "al_ssm module source not found"
[ -d "${KERNEL_MODULES_SRC}/al_sgpo" ] || err "al_sgpo module source not found"
[ -d "${KERNEL_MODULES_SRC}/rtl8370mb" ] || err "rtl8370mb module source not found"
[ -d "${GW_SRC}" ]                     || err "secfirstgw-rs source not found at ${GW_SRC}"

docker info >/dev/null 2>&1 || err "Docker not running"

# Clean previous build (skip if --modules to reuse kernel build cache)
if [ "${MODULES_ONLY}" != "--modules" ]; then
    docker run --rm -v "${OUTPUT_DIR}:/out:z" alpine:3.21 rm -rf /out/* 2>/dev/null || rm -rf "${OUTPUT_DIR}"
fi
mkdir -p "${OUTPUT_DIR}"

# ═══════════════════════════════════════════════════════════════════════════════
# STAGE 1: Build kernel + ALL modules (cross-compile in Debian container)
# ═══════════════════════════════════════════════════════════════════════════════

log "══════════════════════════════════════════════════"
if [ "${MODULES_ONLY}" = "--modules" ]; then
    log "  STAGE 1/4: Out-of-tree modules ONLY (reusing cached kernel)"
else
    log "  STAGE 1/4: Kernel + modules (cross-compile)"
fi
log "══════════════════════════════════════════════════"

if [ "${MODULES_ONLY}" = "--modules" ]; then
    # Modules-only: use cached kernel build tree from named volume
    docker run --rm \
      -v "secfirstgw-kbuild:/build:z" \
      -v "${KERNEL_MODULES_SRC}:/src/modules:ro,z" \
      -v "${OUTPUT_DIR}:/output:z" \
      debian:bookworm-slim bash -c '
    set -euo pipefail
    KVER="6.12.77"
    apt-get update -qq && apt-get install -y -qq --no-install-recommends \
      build-essential gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
      bc bison flex libssl-dev libelf-dev kmod >/dev/null 2>&1

    export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

    [ -f /build/vmlinux ] || { echo "ERROR: No cached kernel build. Run full build first."; exit 1; }
    cd /build

    # Rebuild out-of-tree modules against cached kernel
    KDIR=/build
    for mod in al_eth al_dma al_ssm al_sgpo rtl8370mb; do
      echo ">>> Building out-of-tree: ${mod}..."
      rm -rf /tmp/${mod}
      cp -a /src/modules/${mod} /tmp/${mod}
      if [ "${mod}" = "rtl8370mb" ] && [ -f /tmp/al_eth/Module.symvers ]; then
        make -C ${KDIR} M=/tmp/${mod} KBUILD_EXTRA_SYMBOLS=/tmp/al_eth/Module.symvers modules
      else
        make -C ${KDIR} M=/tmp/${mod} modules
      fi
      cp /tmp/${mod}/*.ko /output/modules-root/lib/modules/${KVER}/extra/
      echo "    OK: $(ls /tmp/${mod}/*.ko | xargs -I{} du -h {} | cut -f1)"
    done

    depmod -b /output/modules-root ${KVER}
    echo "=== Modules-only rebuild done ==="
    '
    log "Stage 1 done (modules only)."
else
docker run --rm \
  -v "secfirstgw-kbuild:/build:z" \
  -v "${KERNEL_SRC}:/src/linux:ro,z" \
  -v "${KERNEL_CONFIGS}:/src/configs:ro,z" \
  -v "${KERNEL_MODULES_SRC}:/src/modules:ro,z" \
  -v "${OUTPUT_DIR}:/output:z" \
  debian:bookworm-slim bash -c '
set -euo pipefail
KVER="6.12.77"
NPROC=$(nproc)

# ── Install toolchain ──
echo ">>> Installing cross-compile toolchain..."
apt-get update -qq && apt-get install -y -qq --no-install-recommends \
  build-essential gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
  bc bison flex libssl-dev libelf-dev kmod cpio device-tree-compiler \
  python3 ca-certificates u-boot-tools gzip rsync >/dev/null 2>&1

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

# ── Copy kernel source (mount is read-only, use rsync for incremental) ──
echo ">>> Syncing kernel source tree..."
if [ -f /build/Makefile ]; then
  echo "    (incremental update)"
  rsync -a --delete /src/linux/ /build/
else
  echo "    (fresh copy)"
  cp -a /src/linux/* /build/
fi
cd /build

# ── Apply defconfig (skip mrproper for incremental builds) ──
echo ">>> Applying udmpro_defconfig..."
if [ ! -f .config ]; then
  make -s mrproper
fi
cp /src/configs/udmpro_defconfig arch/arm64/configs/
make -s udmpro_defconfig

# ── Build kernel Image ──
echo ">>> Building kernel Image (${NPROC} jobs)..."
make -j${NPROC} Image

# ── Build device tree ──
echo ">>> Building DTBs..."
make -j${NPROC} dtbs

# ── Build ALL in-tree modules ──
echo ">>> Building ALL in-tree modules..."
make -j${NPROC} modules

# ── Install modules to staging ──
echo ">>> Installing in-tree modules..."
make modules_install INSTALL_MOD_PATH=/output/modules-root

# ── Build out-of-tree modules ──
KDIR=/build

# Build al_eth first (exports alpine_shared_mdio_bus), then dependents
for mod in al_eth al_dma al_ssm al_sgpo rtl8370mb; do
  echo ">>> Building out-of-tree: ${mod}..."
  cp -a /src/modules/${mod} /tmp/${mod}
  # rtl8370mb depends on al_eth symbols
  if [ "${mod}" = "rtl8370mb" ] && [ -f /tmp/al_eth/Module.symvers ]; then
    cp /tmp/al_eth/Module.symvers /tmp/${mod}/Module.symvers
    make -C ${KDIR} M=/tmp/${mod} KBUILD_EXTRA_SYMBOLS=/tmp/al_eth/Module.symvers modules
  else
    make -C ${KDIR} M=/tmp/${mod} modules
  fi
  mkdir -p /output/modules-root/lib/modules/${KVER}/extra
  cp /tmp/${mod}/*.ko /output/modules-root/lib/modules/${KVER}/extra/
  echo "    OK: $(ls /tmp/${mod}/*.ko | xargs -I{} du -h {} | cut -f1)"
done


# ── Run depmod ──
echo ">>> Running depmod..."
depmod -b /output/modules-root ${KVER}

# ── Copy kernel artifacts ──
echo ">>> Copying kernel artifacts..."
cp arch/arm64/boot/Image /output/Image-udmpro
gzip -c /output/Image-udmpro > /output/Image-udmpro.gz
cp arch/arm64/boot/dts/amazon/alpine-v2-ubnt-udmpro.dtb /output/

# ── Create FIT image ──
echo ">>> Creating FIT image (uImage-secfirstgw)..."
cat > /output/udmpro.its << ITSEOF
/dts-v1/;

/ {
    description = "secfirstGW Linux 6.12 UDM Pro";
    #address-cells = <1>;

    images {
        kernel {
            description = "Linux ${KVER}";
            data = /incbin/("Image-udmpro.gz");
            type = "kernel";
            arch = "arm64";
            os = "linux";
            compression = "gzip";
            load = <0x04080000>;
            entry = <0x04080000>;
        };
        fdt {
            description = "UDM Pro DTB";
            data = /incbin/("alpine-v2-ubnt-udmpro.dtb");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
            load = <0x04078000>;
        };
    };

    configurations {
        default = "udmpro";
        udmpro {
            description = "secfirstGW UDM Pro";
            kernel = "kernel";
            fdt = "fdt";
        };
    };
};
ITSEOF
cd /output
mkimage -f udmpro.its uImage-secfirstgw

# ── Summary ──
echo ""
echo "=== Stage 1 complete ==="
INTREE=$(find /output/modules-root/lib/modules/${KVER}/kernel -name "*.ko" 2>/dev/null | wc -l)
OOT=$(ls /output/modules-root/lib/modules/${KVER}/extra/*.ko 2>/dev/null | wc -l)
echo "  Kernel Image: $(du -h /output/Image-udmpro | cut -f1)"
echo "  FIT image:    $(du -h /output/uImage-secfirstgw | cut -f1)"
echo "  DTB:          $(du -h /output/alpine-v2-ubnt-udmpro.dtb | cut -f1)"
echo "  In-tree modules:    ${INTREE}"
echo "  Out-of-tree modules: ${OOT}"
echo "  Out-of-tree:"
ls -1 /output/modules-root/lib/modules/${KVER}/extra/
echo ""
'

log "Stage 1 done."
fi  # end MODULES_ONLY check

# ═══════════════════════════════════════════════════════════════════════════════
# STAGE 2: Build sfgw binary (native aarch64 via Docker buildx)
# ═══════════════════════════════════════════════════════════════════════════════

if [ "${MODULES_ONLY}" != "--modules" ]; then
log "══════════════════════════════════════════════════"
log "  STAGE 2/4: Build sfgw binary (native aarch64)"
log "══════════════════════════════════════════════════"

cat > "${OUTPUT_DIR}/Dockerfile.sfgw-build" << 'DOCKERFILE'
FROM messense/rust-musl-cross:aarch64-musl AS builder

WORKDIR /build
COPY Cargo.toml Cargo.lock ./
COPY crates crates
COPY patches patches

RUN cargo build --release --bin sfgw --target aarch64-unknown-linux-musl

FROM scratch
COPY --from=builder /build/target/aarch64-unknown-linux-musl/release/sfgw /sfgw
DOCKERFILE

docker buildx build \
  -t secfirstgw-build \
  -f "${OUTPUT_DIR}/Dockerfile.sfgw-build" \
  -o "type=local,dest=${OUTPUT_DIR}/sfgw-out" \
  "${GW_SRC}"

log "  sfgw: $(du -h "${OUTPUT_DIR}/sfgw-out/sfgw" | cut -f1)"

log "Stage 2 done."

# ═══════════════════════════════════════════════════════════════════════════════
# STAGE 3: Build Alpine Linux aarch64 rootfs
# ═══════════════════════════════════════════════════════════════════════════════

log "══════════════════════════════════════════════════"
log "  STAGE 3/4: Alpine rootfs (aarch64)"
log "══════════════════════════════════════════════════"

cat > "${OUTPUT_DIR}/Dockerfile.gw-rootfs" << 'DOCKERFILE'
FROM --platform=linux/arm64 alpine:3.21

RUN apk update && apk add --no-cache \
  openrc openssh openssh-server e2fsprogs util-linux kmod eudev \
  iproute2 iptables ip6tables nftables bridge-utils \
  dhcpcd dnsmasq chrony doas ca-certificates curl jq \
  i2c-tools wireguard-tools tcpdump ethtool smartmontools \
  parted sgdisk btrfs-progs cryptsetup lvm2 mdadm rsync samba

# OpenRC services
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
    ln -s /etc/init.d/dnsmasq    /etc/runlevels/default/ && \
    ln -s /etc/init.d/nftables   /etc/runlevels/default/ && \
    ln -s /etc/init.d/mount-ro   /etc/runlevels/shutdown/ && \
    ln -s /etc/init.d/killprocs  /etc/runlevels/shutdown/ && \
    ln -s /etc/init.d/savecache  /etc/runlevels/shutdown/

RUN mkdir -p /etc/ssh && \
    sed -i 's/#PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config && \
    sed -i 's/#PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config

RUN mkdir -p /var/empty && chmod 755 /var/empty && chown root:root /var/empty

RUN sed -i '/^ttyS0/d' /etc/inittab && \
    echo "ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100" >> /etc/inittab

RUN printf '/dev/sda3\t/\text4\trw,relatime\t0 1\n\
/dev/sda1\t/boot\text4\tro,relatime\t0 2\n\
/dev/sda4\t/var/log\text4\trw,relatime\t0 2\n\
/dev/sda5\t/data\text4\trw,relatime\t0 2\n\
proc\t/proc\tproc\tdefaults\t0 0\n\
sysfs\t/sys\tsysfs\tdefaults\t0 0\n\
devtmpfs\t/dev\tdevtmpfs\tdefaults\t0 0\n\
tmpfs\t/tmp\ttmpfs\tnosuid,nodev\t0 0\n' > /etc/fstab

RUN printf 'auto lo\niface lo inet loopback\n' \
    > /etc/network/interfaces

RUN mkdir -p /etc/sysctl.d && \
    printf 'net.ipv4.ip_forward = 1\nnet.ipv6.conf.all.forwarding = 1\n' \
    > /etc/sysctl.d/01-gateway.conf

RUN mkdir -p /data /var/log /boot /root/.ssh && chmod 700 /root/.ssh

CMD ["/bin/sh"]
DOCKERFILE

docker buildx build \
  --platform linux/arm64 \
  -t secfirstgw-rootfs \
  -f "${OUTPUT_DIR}/Dockerfile.gw-rootfs" \
  --load \
  "${OUTPUT_DIR}"

log "Stage 3 done."
fi  # end MODULES_ONLY skip for stages 2+3

# Set GW_BINARY for both full and modules-only builds
GW_BINARY="${OUTPUT_DIR}/sfgw-out/sfgw"
if [ ! -f "${GW_BINARY}" ] && [ "${MODULES_ONLY}" = "--modules" ]; then
    # Modules-only: extract sfgw from existing rootfs tarball
    log "Extracting sfgw from existing rootfs..."
    mkdir -p "${OUTPUT_DIR}/sfgw-out"
    tar xzf "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz" -C "${OUTPUT_DIR}/sfgw-out" ./usr/local/bin/sfgw 2>/dev/null \
        && mv "${OUTPUT_DIR}/sfgw-out/usr/local/bin/sfgw" "${GW_BINARY}" \
        && rm -rf "${OUTPUT_DIR}/sfgw-out/usr" \
        || err "No sfgw binary and no existing rootfs — run full build first"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# STAGE 4: Combine everything
# ═══════════════════════════════════════════════════════════════════════════════

log "══════════════════════════════════════════════════"
log "  STAGE 4/4: Combine rootfs + kernel + modules + sfgw"
log "══════════════════════════════════════════════════"

# ── Extract Alpine rootfs ──
log "Extracting Alpine rootfs..."
mkdir -p "${OUTPUT_DIR}/rootfs-tmp"
CONTAINER_ID=$(docker create --platform linux/arm64 secfirstgw-rootfs)
docker export "${CONTAINER_ID}" | tar x -C "${OUTPUT_DIR}/rootfs-tmp/"
docker rm "${CONTAINER_ID}" > /dev/null

ROOTFS="${OUTPUT_DIR}/rootfs-tmp"

# ── Remove Docker artifacts ──
log "Removing Docker artifacts..."
rm -f "${ROOTFS}/.dockerenv"
# Docker export creates /dev/console as regular file (not char device).
# This prevents devtmpfs from mounting. Clean /dev so the kernel can
# mount devtmpfs cleanly at boot.
rm -rf "${ROOTFS}/dev"
mkdir -p "${ROOTFS}/dev"

# ── Fix Docker-managed files ──
log "Fixing Docker-managed files..."
echo "secfirstgw" > "${ROOTFS}/etc/hostname"
# No static IPs — sfgw manages all interfaces
printf 'auto lo\niface lo inet loopback\n' > "${ROOTFS}/etc/network/interfaces"
echo "127.0.0.1 secfirstgw localhost" > "${ROOTFS}/etc/hosts"
echo "nameserver 1.1.1.1" > "${ROOTFS}/etc/resolv.conf"

# ── Root password ──
log "Setting root password..."
HASH=$(docker run --rm alpine:3.21 sh -c "apk add --no-cache openssl >/dev/null 2>&1 && openssl passwd -6 '${ROOT_PASSWORD}'")
sed -i "s|^root:.*|root:${HASH}:0:0:::::|" "${ROOTFS}/etc/shadow"

# ── Install ALL kernel modules ──
log "Installing kernel modules..."
rm -rf "${ROOTFS}/lib/modules"
cp -a "${OUTPUT_DIR}/modules-root/lib/modules" "${ROOTFS}/lib/"

INTREE=$(find "${ROOTFS}/lib/modules/${KVER}/kernel" -name '*.ko' 2>/dev/null | wc -l)
OOT=$(ls "${ROOTFS}/lib/modules/${KVER}/extra/"*.ko 2>/dev/null | wc -l)
log "  In-tree:  ${INTREE} modules"
log "  Out-of-tree: ${OOT} modules"
ls -1 "${ROOTFS}/lib/modules/${KVER}/extra/"

# ── Module auto-loading ──
log "Configuring module auto-loading..."
cat > "${ROOTFS}/etc/modules" << 'EOF'
al_dma
al_ssm
al_eth
al_sgpo
marvell
marvell10g
nf_conntrack
nf_tables
wireguard
EOF

mkdir -p "${ROOTFS}/etc/modules-load.d"
cp "${ROOTFS}/etc/modules" "${ROOTFS}/etc/modules-load.d/secfirstgw.conf"

# ── Install sfgw binary (natively built aarch64) ──
log "Installing sfgw binary..."
mkdir -p "${ROOTFS}/usr/local/bin"
cp "${GW_BINARY}" "${ROOTFS}/usr/local/bin/sfgw"
chmod 755 "${ROOTFS}/usr/local/bin/sfgw"
log "  sfgw: $(du -h "${ROOTFS}/usr/local/bin/sfgw" | cut -f1)"

# ── OpenRC init script for sfgw ──
cat > "${ROOTFS}/etc/init.d/sfgw" << 'INITEOF'
#!/sbin/openrc-run

name="sfgw"
description="secfirstGW gateway service"
command="/usr/local/bin/sfgw"
command_args=""
command_background=true
pidfile="/run/${RC_SVCNAME}.pid"
output_log="/var/log/sfgw/sfgw.log"
error_log="/var/log/sfgw/sfgw.err"

export SFGW_WEB_DIR="/usr/local/share/sfgw/web"
export SFGW_PLATFORM="bare-metal"

depend() {
    need net
    after firewall nftables
}

start_pre() {
    mkdir -p /var/log/sfgw
    mkdir -p /data/sfgw
    mkdir -p /data/sfgw-tls
}
INITEOF
chmod 755 "${ROOTFS}/etc/init.d/sfgw"
mkdir -p "${ROOTFS}/etc/runlevels/default"
ln -sf /etc/init.d/sfgw "${ROOTFS}/etc/runlevels/default/sfgw"
mkdir -p "${ROOTFS}/var/log/sfgw" "${ROOTFS}/data/sfgw"

# ── Install Web UI assets ──
WEB_DIST="${GW_SRC}/web/dist"
if [ -d "${WEB_DIST}" ]; then
    log "Installing Web UI assets..."
    mkdir -p "${ROOTFS}/usr/local/share/sfgw/web"
    cp -a "${WEB_DIST}/." "${ROOTFS}/usr/local/share/sfgw/web/"
    log "  Web UI: $(find "${ROOTFS}/usr/local/share/sfgw/web" -type f | wc -l) files"
else
    warn "No web/dist/ found at ${WEB_DIST} — run 'cd web && npm run build' first"
fi

# ── RTL8370MB switch GPIO init (boot service) ──
# PCA9575 pins 4+8 must be output HIGH to release switch from reset.
# Mainline gpio-hog doesn't work on this driver, so we use i2c-tools.
cat > "${ROOTFS}/etc/init.d/switch-gpio" << 'INITEOF'
#!/sbin/openrc-run

name="switch-gpio"
description="Release RTL8370MB switch from reset via PCA9575 GPIO"

depend() {
    before networking
    need localmount
}

start() {
    ebegin "Setting PCA9575 GPIOs for RTL8370MB switch"
    # Unbind pca953x driver for raw I2C access
    echo 0-0028 > /sys/bus/i2c/drivers/pca953x/unbind 2>/dev/null
    # Pin 4 (Port0 bit4) + Pin 8 (Port1 bit0) = output HIGH
    i2cset -y 0 0x28 0x02 0x10  # Output Port 0: pin 4 HIGH
    i2cset -y 0 0x28 0x03 0x01  # Output Port 1: pin 8 HIGH
    i2cset -y 0 0x28 0x06 0xef  # Config Port 0: pin 4 = output
    i2cset -y 0 0x28 0x07 0xfe  # Config Port 1: pin 8 = output
    # Rebind driver
    echo 0-0028 > /sys/bus/i2c/drivers/pca953x/bind 2>/dev/null
    eend 0
}
INITEOF
chmod 755 "${ROOTFS}/etc/init.d/switch-gpio"
ln -sf /etc/init.d/switch-gpio "${ROOTFS}/etc/runlevels/boot/switch-gpio"

# ── Interface renaming (match stock firmware names) ──
# Stock firmware: PCI 00:03.0 (switch uplink) = eth0
# Our kernel probe order: eth0=SFP+, eth1=WAN, eth2=SFP+LAN, eth3=switch
# Rename to match stock convention so sfgw port mapping works.
cat > "${ROOTFS}/etc/init.d/rename-if" << 'INITEOF'
#!/sbin/openrc-run

name="rename-if"
description="Rename network interfaces to match UDM Pro stock layout"

depend() {
    after switch-gpio
    before networking
    need localmount
}

start() {
    ebegin "Renaming network interfaces"
    # Map PCI devices to stock names:
    #   00:00.0 (10G SFP+ WAN)   → eth9
    #   00:01.0 (1G WAN RJ45)    → eth8
    #   00:02.0 (10G SFP+ LAN)   → eth10
    #   00:03.0 (Switch uplink)   → eth0 (switch0)
    #
    # First rename all to temp names to avoid conflicts
    for pci_dir in /sys/bus/pci/devices/0000:00:0[0-3].0/net/*; do
        [ -d "$pci_dir" ] || continue
        ifname=$(basename "$pci_dir")
        ip link set "$ifname" down 2>/dev/null
        ip link set "$ifname" name "tmp_${ifname}" 2>/dev/null
    done

    # Now rename to final names based on PCI address
    for pci_addr in 0000:00:00.0 0000:00:01.0 0000:00:02.0 0000:00:03.0; do
        net_dir="/sys/bus/pci/devices/${pci_addr}/net"
        [ -d "$net_dir" ] || continue
        current=$(ls "$net_dir" | head -1)
        [ -n "$current" ] || continue

        case "$pci_addr" in
            0000:00:00.0) target="eth9"  ;;  # 10G SFP+ WAN
            0000:00:01.0) target="eth8"  ;;  # 1G WAN RJ45
            0000:00:02.0) target="eth10" ;;  # 10G SFP+ LAN
            0000:00:03.0) target="eth0"  ;;  # Switch uplink
        esac

        if [ "$current" != "$target" ]; then
            ip link set "$current" name "$target" 2>/dev/null
        fi
        ip link set "$target" up 2>/dev/null
    done

    # Load switch module now that MDIO bus exists (eth8 is UP)
    sleep 1
    modprobe rtl8370mb_init 2>/dev/null || true
    eend 0
}
INITEOF
chmod 755 "${ROOTFS}/etc/init.d/rename-if"
ln -sf /etc/init.d/rename-if "${ROOTFS}/etc/runlevels/boot/rename-if"

# ── SSH authorized keys ──
if [ -f "${SSH_KEYS}" ]; then
    log "Installing SSH key..."
    mkdir -p "${ROOTFS}/root/.ssh"
    chmod 700 "${ROOTFS}/root/.ssh"
    cp "${SSH_KEYS}" "${ROOTFS}/root/.ssh/authorized_keys"
    chmod 600 "${ROOTFS}/root/.ssh/authorized_keys"
else
    warn "SSH key not found at ${SSH_KEYS} — skipping"
fi

# ── Install kernel + DTB to /boot ──
log "Installing kernel to /boot..."
mkdir -p "${ROOTFS}/boot"
cp "${OUTPUT_DIR}/uImage-secfirstgw" "${ROOTFS}/boot/"
cp "${OUTPUT_DIR}/alpine-v2-ubnt-udmpro.dtb" "${ROOTFS}/boot/"

# ── Fix /var/empty for sshd ──
mkdir -p "${ROOTFS}/var/empty"
chmod 755 "${ROOTFS}/var/empty"

# ── Ensure mount points ──
mkdir -p "${ROOTFS}/data" "${ROOTFS}/var/log" "${ROOTFS}/boot"

# ═══════════════════════════════════════════════════════════════════════════════
# Pack final rootfs tarball
# ═══════════════════════════════════════════════════════════════════════════════

log "Packing rootfs tarball..."
tar czf "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz" --numeric-owner --owner=0 --group=0 -C "${ROOTFS}" .

# ── Cleanup ──
rm -rf "${ROOTFS}" "${OUTPUT_DIR}/sfgw-out"
rm -f "${OUTPUT_DIR}/Dockerfile.gw-rootfs" "${OUTPUT_DIR}/Dockerfile.sfgw-build"

# ── Checksums ──
sha256sum "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz" > "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz.sha256"

# ═══════════════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════════════

echo ""
echo "══════════════════════════════════════════════════"
echo "  secfirstGW rootfs build complete"
echo "══════════════════════════════════════════════════"
echo ""
echo "  Output:   ${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz"
echo "  Size:     $(du -h "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz" | cut -f1)"
echo "  SHA-256:  $(cut -d' ' -f1 "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz.sha256")"
echo ""
echo "  Kernel:   Linux ${KVER} (UDM Pro defconfig)"
echo "  FIT:      uImage-secfirstgw"
echo "  DTB:      alpine-v2-ubnt-udmpro.dtb"
echo "  Rootfs:   Alpine Linux 3.21 (aarch64)"
echo ""
echo "  In-tree modules:     ${INTREE}"
echo "  Out-of-tree modules: ${OOT} (al_eth, al_dma, al_ssm, al_sgpo, rtl8370mb)"
echo "  sfgw:     /usr/local/bin/sfgw (native aarch64)"
echo "  sfgw init: /etc/init.d/sfgw (OpenRC, default runlevel)"
echo ""
echo "  Root password: ${ROOT_PASSWORD}"
echo "  SSH:           key-based (+ password)"
echo "  Serial:        ttyS0 @ 115200"
echo "  Network:       eth0 DHCP (WAN)"
echo "  Gateway:       ip_forward=1, ipv6 forwarding=1"
echo ""
echo "  Flash to UDM Pro eMMC:"
echo "    scp secfirstgw-rootfs.tar.gz root@<ip>:/tmp/"
echo "    ssh root@<ip>"
echo "    mkfs.ext4 -F /dev/sda3"
echo "    mount /dev/sda3 /mnt && cd /mnt"
echo "    tar xzf /tmp/secfirstgw-rootfs.tar.gz"
echo "    sync && cd / && umount /mnt && reboot"
echo ""
echo "══════════════════════════════════════════════════"
