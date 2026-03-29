#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# build-image-v3.sh — Build a complete flashable eMMC image for secfirstNAS on UNVR
#
# Packages everything into a single GPT image ready to dd onto the UNVR eMMC:
#   - Kernel (uImage + DTB)
#   - Alpine Linux rootfs with all packages
#   - All kernel modules (in-tree + out-of-tree: al_eth, al_dma, al_ssm, al_sgpo)
#   - secfirstnas binary (cross-compiled static musl aarch64)
#   - web-nas static files
#   - OpenRC init script
#   - Module auto-load config
#   - Samba defaults
#   - Network config (eth0 DHCP)
#
# Does NOT rebuild the kernel — packages pre-built artifacts from kernel/build/.
#
# Partition layout (matches Ubiquiti stock for U-Boot compatibility):
#   boot1 (128M)  - kernel (uImage + DTB)
#   boot2 (2G)    - rootfs (Alpine Linux)
#   boot3 (4G)    - /data (persistent config, databases, www)
#   boot4 (4G)    - /var/log
#   boot5 (3G)    - overlay / future use
#
# Usage (run inside Docker with --privileged or with fakeroot):
#   docker run --rm --privileged \
#     -v /path/to/secfirstnas-rs:/input \
#     -v /path/to/secfirstgw-rs:/gw \
#     -v $(pwd)/output:/output \
#     alpine:3.21 /input/image/build-image-v3.sh
#
# Or directly (requires root for mount/losetup):
#   sudo ./build-image-v3.sh

set -euo pipefail

# ─── Configuration ────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAS_ROOT="$(dirname "${SCRIPT_DIR}")"

# Input paths — can be overridden via environment
KERNEL_IMAGE="${KERNEL_IMAGE:-${NAS_ROOT}/kernel/build/uImage}"
KERNEL_DTB="${KERNEL_DTB:-${NAS_ROOT}/kernel/build/alpine-v2-ubnt-unvr.dtb}"
KERNEL_MODULES_ROOTFS="${KERNEL_MODULES_ROOTFS:-${NAS_ROOT}/kernel/build/rootfs/lib/modules/6.12.77}"
KERNEL_MODULES_BUILD="${KERNEL_MODULES_BUILD:-${NAS_ROOT}/kernel/build}"
ROOTFS_OUTPUT="${ROOTFS_OUTPUT:-${NAS_ROOT}/rootfs/output}"
SECFIRSTNAS_BIN="${SECFIRSTNAS_BIN:-/gw/target/aarch64-unknown-linux-musl/release/secfirstnas}"
WEBNAS_DIST="${WEBNAS_DIST:-/gw/web-nas/dist}"
INITD_SCRIPT="${INITD_SCRIPT:-/gw/scripts/secfirstnas.initd}"
OUTPUT="${OUTPUT:-/output/secfirstnas-unvr.img}"

KVER="6.12.77"

# Partition sizes in MB — matches Ubiquiti stock layout
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

# Colors
if [ -t 1 ]; then
    G='\033[0;32m' Y='\033[1;33m' C='\033[0;36m' R='\033[0;31m' N='\033[0m'
else
    G='' Y='' C='' R='' N=''
fi

log()  { echo -e "${G}[img]${N} $*"; }
warn() { echo -e "${Y}[img]${N} $*"; }
err()  { echo -e "${R}[img]${N} $*" >&2; exit 1; }

# ─── Preflight checks ────────────────────────────────────────────────────────

log "Checking required inputs..."

for f in "${KERNEL_IMAGE}" "${KERNEL_DTB}"; do
    if [ ! -f "$f" ]; then
        err "Required file not found: $f"
    fi
done

if [ ! -d "${KERNEL_MODULES_ROOTFS}" ]; then
    err "Kernel modules directory not found: ${KERNEL_MODULES_ROOTFS}"
fi

# Binary is optional at image build time (can be injected via deploy)
if [ ! -f "${SECFIRSTNAS_BIN}" ]; then
    warn "secfirstnas binary not found at ${SECFIRSTNAS_BIN} — image will not include it"
    warn "Deploy via nas-deploy.sh after flashing"
    HAS_BIN=0
else
    HAS_BIN=1
fi

# Web UI is optional
if [ ! -d "${WEBNAS_DIST}" ] || [ ! -f "${WEBNAS_DIST}/index.html" ]; then
    warn "web-nas dist not found at ${WEBNAS_DIST} — image will not include web UI"
    HAS_WEBUI=0
else
    HAS_WEBUI=1
fi

# Check for Alpine rootfs
if [ ! -d "${ROOTFS_OUTPUT}" ]; then
    warn "Pre-built rootfs not found at ${ROOTFS_OUTPUT} — will create minimal rootfs"
    HAS_ROOTFS=0
else
    HAS_ROOTFS=1
fi

# Install tools if running in Alpine Docker
if command -v apk >/dev/null 2>&1; then
    apk add --no-cache parted e2fsprogs coreutils 2>/dev/null || true
fi

mkdir -p "$(dirname "${OUTPUT}")"

# ─── Step 1: Create sparse image ─────────────────────────────────────────────

log "Step 1/8: Creating sparse image (${IMG_SIZE}M)..."
dd if=/dev/zero of="${OUTPUT}" bs=1M count=0 seek=${IMG_SIZE} 2>/dev/null

# ─── Step 2: Partition ────────────────────────────────────────────────────────

log "Step 2/8: Partitioning (GPT, 5 partitions)..."
parted -s "${OUTPUT}" mklabel gpt
parted -s "${OUTPUT}" mkpart kernel  ext4 ${P1_START}MiB $((P1_START + P1_SIZE))MiB
parted -s "${OUTPUT}" mkpart rootfs  ext4 ${P2_START}MiB $((P2_START + P2_SIZE))MiB
parted -s "${OUTPUT}" mkpart data    ext4 ${P3_START}MiB $((P3_START + P3_SIZE))MiB
parted -s "${OUTPUT}" mkpart log     ext4 ${P4_START}MiB $((P4_START + P4_SIZE))MiB
parted -s "${OUTPUT}" mkpart overlay ext4 ${P5_START}MiB $((P5_START + P5_SIZE))MiB

# ─── Step 3: Create partition images ─────────────────────────────────────────

log "Step 3/8: Creating partition images..."

TMPDIR=$(mktemp -d)
trap "rm -rf ${TMPDIR}" EXIT

# --- P1: Kernel ---
log "  P1: kernel (uImage + DTB)..."
dd if=/dev/zero of="${TMPDIR}/p1.img" bs=1M count=${P1_SIZE} 2>/dev/null
mkfs.ext4 -q -L kernel "${TMPDIR}/p1.img"
mkdir -p "${TMPDIR}/p1"
mount "${TMPDIR}/p1.img" "${TMPDIR}/p1"
cp "${KERNEL_IMAGE}" "${TMPDIR}/p1/uImage"
# Also copy the raw Image for flexibility
if [ -f "${NAS_ROOT}/kernel/build/Image" ]; then
    cp "${NAS_ROOT}/kernel/build/Image" "${TMPDIR}/p1/Image"
fi
cp "${KERNEL_DTB}" "${TMPDIR}/p1/alpine-v2-ubnt-unvr.dtb"
ls -lh "${TMPDIR}/p1/"
umount "${TMPDIR}/p1"

# --- P2: Root filesystem ---
log "  P2: rootfs (Alpine Linux + secfirstnas)..."
dd if=/dev/zero of="${TMPDIR}/p2.img" bs=1M count=${P2_SIZE} 2>/dev/null
mkfs.ext4 -q -L rootfs "${TMPDIR}/p2.img"
mkdir -p "${TMPDIR}/p2"
mount "${TMPDIR}/p2.img" "${TMPDIR}/p2"

if [ "${HAS_ROOTFS}" -eq 1 ]; then
    cp -a "${ROOTFS_OUTPUT}"/* "${TMPDIR}/p2/"
else
    warn "No pre-built rootfs — creating skeleton"
    mkdir -p "${TMPDIR}/p2"/{bin,sbin,usr/bin,usr/sbin,etc,lib,var,tmp,proc,sys,dev,run,data,boot}
fi

# --- Install kernel modules (in-tree from rootfs) ---
log "  Installing kernel modules (in-tree)..."
DEST_KMOD="${TMPDIR}/p2/lib/modules/${KVER}"
mkdir -p "${DEST_KMOD}"

# Copy the complete pre-built module tree
if [ -d "${KERNEL_MODULES_ROOTFS}" ]; then
    cp -a "${KERNEL_MODULES_ROOTFS}"/* "${DEST_KMOD}/"
fi

# --- Install out-of-tree modules ---
log "  Installing kernel modules (out-of-tree: al_eth, al_dma, al_ssm, al_sgpo)..."
mkdir -p "${DEST_KMOD}/extra"

for mod in al_eth al_dma al_ssm al_sgpo; do
    # Try rootfs/extra first, then build dir
    if [ -f "${KERNEL_MODULES_ROOTFS}/extra/${mod}.ko" ]; then
        cp "${KERNEL_MODULES_ROOTFS}/extra/${mod}.ko" "${DEST_KMOD}/extra/"
        log "    ${mod}.ko (from rootfs)"
    elif [ -f "${KERNEL_MODULES_BUILD}/${mod}.ko" ]; then
        cp "${KERNEL_MODULES_BUILD}/${mod}.ko" "${DEST_KMOD}/extra/"
        log "    ${mod}.ko (from build)"
    else
        warn "    ${mod}.ko NOT FOUND — skipping"
    fi
done

# --- Install additional in-tree modules (md, dm, btrfs, etc.) ---
INTREE_MOD_DIR="${KERNEL_MODULES_BUILD}/modules"
if [ -d "${INTREE_MOD_DIR}" ]; then
    log "  Installing additional in-tree modules (md, dm, btrfs, xor, etc.)..."
    for mod_file in "${INTREE_MOD_DIR}"/*.ko; do
        if [ -f "${mod_file}" ]; then
            mod_name=$(basename "${mod_file}")
            # Place in kernel/extra for in-tree modules built out-of-tree
            cp "${mod_file}" "${DEST_KMOD}/extra/"
            log "    ${mod_name}"
        fi
    done
fi

# Run depmod (best-effort — may not work without chroot qemu)
depmod -b "${TMPDIR}/p2" "${KVER}" 2>/dev/null || true

# --- Install secfirstnas binary ---
if [ "${HAS_BIN}" -eq 1 ]; then
    log "  Installing secfirstnas binary..."
    cp "${SECFIRSTNAS_BIN}" "${TMPDIR}/p2/usr/local/bin/secfirstnas"
    chmod 0755 "${TMPDIR}/p2/usr/local/bin/secfirstnas"
fi

# --- Install OpenRC init script ---
if [ -f "${INITD_SCRIPT}" ]; then
    log "  Installing OpenRC init script..."
    mkdir -p "${TMPDIR}/p2/etc/init.d"
    cp "${INITD_SCRIPT}" "${TMPDIR}/p2/etc/init.d/secfirstnas"
    chmod 0755 "${TMPDIR}/p2/etc/init.d/secfirstnas"
    # Enable at default runlevel
    mkdir -p "${TMPDIR}/p2/etc/runlevels/default"
    ln -sf /etc/init.d/secfirstnas "${TMPDIR}/p2/etc/runlevels/default/secfirstnas" 2>/dev/null || true
fi

# --- Module auto-load config ---
log "  Configuring module auto-loading..."
mkdir -p "${TMPDIR}/p2/etc/modules-load.d"
cat > "${TMPDIR}/p2/etc/modules-load.d/secfirstnas.conf" <<'MODEOF'
# secfirstNAS kernel modules — loaded at boot
al_dma
al_ssm
al_eth
al_sgpo
MODEOF

# --- Network config (eth0 DHCP) ---
log "  Configuring network (eth0 DHCP)..."
mkdir -p "${TMPDIR}/p2/etc/network"
cat > "${TMPDIR}/p2/etc/network/interfaces" <<'NETEOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp

auto enp0s1
iface enp0s1 inet dhcp
NETEOF

# --- Samba defaults ---
log "  Installing Samba defaults..."
mkdir -p "${TMPDIR}/p2/etc/samba"
cat > "${TMPDIR}/p2/etc/samba/smb.conf" <<'SMBEOF'
[global]
   workgroup = WORKGROUP
   server string = SecFirstNAS
   server role = standalone server
   log file = /var/log/samba/log.%m
   max log size = 1000
   logging = file

   # Security
   map to guest = Bad User
   usershare allow guests = yes
   security = user
   passdb backend = tdbsam

   # SMB3 minimum — no legacy protocols
   server min protocol = SMB3
   server max protocol = SMB3

   # Performance
   socket options = TCP_NODELAY IPTOS_LOWDELAY
   read raw = yes
   write raw = yes
   use sendfile = yes
   aio read size = 16384
   aio write size = 16384

   # Disable printing
   load printers = no
   printing = bsd
   printcap name = /dev/null
   disable spoolss = yes

   # File creation
   create mask = 0664
   directory mask = 0775
SMBEOF

mkdir -p "${TMPDIR}/p2/var/log/samba"

# --- fstab ---
log "  Writing fstab..."
cat > "${TMPDIR}/p2/etc/fstab" <<'FSTABEOF'
LABEL=rootfs    /           ext4    rw,relatime     0 1
LABEL=kernel    /boot       ext4    ro,relatime     0 2
LABEL=data      /data       ext4    rw,relatime     0 2
LABEL=log       /var/log    ext4    rw,relatime     0 2
devtmpfs        /dev        devtmpfs defaults       0 0
proc            /proc       proc    defaults        0 0
sysfs           /sys        sysfs   defaults        0 0
tmpfs           /tmp        tmpfs   nosuid,nodev    0 0
FSTABEOF

# --- Hostname ---
echo "secfirstnas" > "${TMPDIR}/p2/etc/hostname"

# --- Console on serial ---
if [ -f "${TMPDIR}/p2/etc/inittab" ]; then
    if ! grep -q "ttyS0" "${TMPDIR}/p2/etc/inittab"; then
        echo "ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100" >> "${TMPDIR}/p2/etc/inittab"
    fi
fi

# --- Ensure mount points ---
mkdir -p "${TMPDIR}/p2/data" "${TMPDIR}/p2/var/log" "${TMPDIR}/p2/boot"

# --- secfirstnas log dir ---
mkdir -p "${TMPDIR}/p2/var/log/secfirstnas"

log "  Rootfs size: $(du -sh "${TMPDIR}/p2" | cut -f1)"
umount "${TMPDIR}/p2"

# --- P3: Data partition ---
log "  P3: data partition..."
dd if=/dev/zero of="${TMPDIR}/p3.img" bs=1M count=${P3_SIZE} 2>/dev/null
mkfs.ext4 -q -L data "${TMPDIR}/p3.img"
mkdir -p "${TMPDIR}/p3"
mount "${TMPDIR}/p3.img" "${TMPDIR}/p3"

# Create standard data directories
mkdir -p "${TMPDIR}/p3/samba"
mkdir -p "${TMPDIR}/p3/config"
mkdir -p "${TMPDIR}/p3/sfgw"
mkdir -p "${TMPDIR}/p3/www"

# Install web UI into data partition
if [ "${HAS_WEBUI}" -eq 1 ]; then
    log "  Installing web-nas static files to /data/www/..."
    cp -a "${WEBNAS_DIST}"/* "${TMPDIR}/p3/www/"
fi

umount "${TMPDIR}/p3"

# --- P4: Log ---
log "  P4: log partition..."
dd if=/dev/zero of="${TMPDIR}/p4.img" bs=1M count=${P4_SIZE} 2>/dev/null
mkfs.ext4 -q -L log "${TMPDIR}/p4.img"

# --- P5: Overlay ---
log "  P5: overlay partition..."
dd if=/dev/zero of="${TMPDIR}/p5.img" bs=1M count=${P5_SIZE} 2>/dev/null
mkfs.ext4 -q -L overlay "${TMPDIR}/p5.img"

# ─── Step 4: Write partitions to image ───────────────────────────────────────

log "Step 4/8: Writing partitions to image..."
dd if="${TMPDIR}/p1.img" of="${OUTPUT}" bs=1M seek=${P1_START} conv=notrunc 2>/dev/null
dd if="${TMPDIR}/p2.img" of="${OUTPUT}" bs=1M seek=${P2_START} conv=notrunc 2>/dev/null
dd if="${TMPDIR}/p3.img" of="${OUTPUT}" bs=1M seek=${P3_START} conv=notrunc 2>/dev/null
dd if="${TMPDIR}/p4.img" of="${OUTPUT}" bs=1M seek=${P4_START} conv=notrunc 2>/dev/null
dd if="${TMPDIR}/p5.img" of="${OUTPUT}" bs=1M seek=${P5_START} conv=notrunc 2>/dev/null

# ─── Step 5: Compress ────────────────────────────────────────────────────────

log "Step 5/8: Compressing image..."
gzip -1 -f "${OUTPUT}"

# ─── Step 6: Generate checksums ──────────────────────────────────────────────

log "Step 6/8: Generating checksums..."
sha256sum "${OUTPUT}.gz" > "${OUTPUT}.gz.sha256"

# ─── Step 7: Print manifest ──────────────────────────────────────────────────

log "Step 7/8: Build manifest..."

echo ""
echo "=============================================="
echo "  secfirstNAS UNVR Image Build Complete (v3)"
echo "=============================================="
echo ""
echo "  Image:    $(ls -lh "${OUTPUT}.gz" | awk '{print $5}')"
echo "  SHA-256:  $(cat "${OUTPUT}.gz.sha256" | cut -d' ' -f1)"
echo ""
echo "  Contents:"
echo "    Kernel:     uImage + DTB (6.12.77)"
if [ "${HAS_ROOTFS}" -eq 1 ]; then
    echo "    Rootfs:     Alpine Linux 3.21 (aarch64)"
else
    echo "    Rootfs:     Skeleton (install packages manually)"
fi
echo "    Modules:    in-tree ($(find "${KERNEL_MODULES_ROOTFS}/kernel" -name '*.ko' 2>/dev/null | wc -l)) + out-of-tree (al_eth, al_dma, al_ssm, al_sgpo)"
if [ "${HAS_BIN}" -eq 1 ]; then
    echo "    Binary:     /usr/local/bin/secfirstnas"
else
    echo "    Binary:     NOT INCLUDED (deploy via nas-deploy.sh)"
fi
if [ "${HAS_WEBUI}" -eq 1 ]; then
    echo "    Web UI:     /data/www/ ($(find "${WEBNAS_DIST}" -type f | wc -l) files)"
else
    echo "    Web UI:     NOT INCLUDED (deploy via nas-deploy.sh)"
fi
echo "    Init:       /etc/init.d/secfirstnas (OpenRC)"
echo "    Autoload:   /etc/modules-load.d/secfirstnas.conf"
echo "    Samba:      /etc/samba/smb.conf (SMB3-only)"
echo "    Network:    eth0/enp0s1 DHCP"
echo ""

# ─── Step 8: Flash instructions ──────────────────────────────────────────────

log "Step 8/8: Flash instructions..."

echo "  To flash via SSH (from running system):"
echo "    1. scp ${OUTPUT}.gz root@10.0.0.118:/tmp/"
echo "    2. ssh root@10.0.0.118"
echo "    3. gunzip /tmp/$(basename "${OUTPUT}").gz"
echo "    4. dd if=/tmp/$(basename "${OUTPUT}") of=/dev/boot bs=4M"
echo "    5. sync && reboot"
echo ""
echo "  To flash via UART recovery:"
echo "    1. Connect UART (115200 8N1)"
echo "    2. Hold reset button while powering on"
echo "    3. telnet into recovery"
echo "    4. Flash via dd"
echo ""
echo "  U-Boot env (set via UART or fw_setenv):"
echo "    setenv rootfs PARTLABEL=rootfs"
echo "    setenv bootargsextra boot=local rw"
echo "    saveenv"
echo ""
echo "=============================================="
