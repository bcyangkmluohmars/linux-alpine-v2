#!/bin/bash
# serve-gw.sh — Build output via HTTP bereitstellen (Port 8888)
#
# Startet einen HTTP-Server auf Port 8888 mit den Build-Artefakten
# aus output-gw/. Von der Recovery-Shell:
#
#   wget http://<host-ip>:8888/uImage-secfirstgw -O /tmp/uImage
#   wget http://<host-ip>:8888/secfirstgw-rootfs.tar.gz -O /tmp/rootfs.tar.gz
#
# Usage:
#   ./serve-gw.sh          # Nur bereitstellen
#   ./serve-gw.sh --build  # Erst bauen, dann bereitstellen
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/rootfs/output-gw"
PORT=8888

G='\033[0;32m' Y='\033[1;33m' R='\033[0;31m' B='\033[1;34m' N='\033[0m'

if [[ "${1:-}" == "--build" ]]; then
    echo -e "${Y}>>> Starte Build...${N}"
    "${SCRIPT_DIR}/rootfs/build-rootfs-gw.sh"
    echo ""
fi

# Prüfe ob Build-Output existiert
if [[ ! -f "${OUTPUT_DIR}/secfirstgw-rootfs.tar.gz" ]] || [[ ! -f "${OUTPUT_DIR}/uImage-secfirstgw" ]]; then
    echo -e "${R}Fehler: Build-Output nicht gefunden in ${OUTPUT_DIR}${N}"
    echo -e "${R}Erst bauen: ./serve-gw.sh --build${N}"
    exit 1
fi

# Kill alter Server auf dem Port falls vorhanden
if lsof -ti:${PORT} >/dev/null 2>&1; then
    echo -e "${Y}Port ${PORT} belegt — beende alten Server...${N}"
    kill $(lsof -ti:${PORT}) 2>/dev/null || true
    sleep 1
fi

echo ""
echo -e "${G}══════════════════════════════════════════════════${N}"
echo -e "${G}  secfirstGW Recovery HTTP Server${N}"
echo -e "${G}══════════════════════════════════════════════════${N}"
echo ""

# Dateien anzeigen
echo -e "${B}Dateien:${N}"
for f in uImage-secfirstgw secfirstgw-rootfs.tar.gz alpine-v2-ubnt-udmpro.dtb secfirstgw-rootfs.tar.gz.sha256; do
    if [[ -f "${OUTPUT_DIR}/${f}" ]]; then
        SIZE=$(du -h "${OUTPUT_DIR}/${f}" | cut -f1)
        echo -e "  ${f}  (${SIZE})"
    fi
done
echo ""

# IPs anzeigen
echo -e "${B}Erreichbar unter:${N}"
ip -4 addr show scope global 2>/dev/null | grep -oP 'inet \K[\d.]+' | while read -r ip; do
    echo -e "  http://${ip}:${PORT}/"
done
echo ""

echo -e "${Y}Recovery-Shell Befehle:${N}"
HOST_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[\d.]+' || echo '<host-ip>')
cat << EOF
  # Kernel flashen
  wget http://${HOST_IP}:${PORT}/uImage-secfirstgw -O /tmp/uImage
  dd if=/tmp/uImage of=/dev/mtd5

  # Rootfs flashen
  wget http://${HOST_IP}:${PORT}/secfirstgw-rootfs.tar.gz -O /tmp/rootfs.tar.gz
  mkfs.ext4 -F /dev/sda3
  mount /dev/sda3 /mnt && cd /mnt
  tar xzf /tmp/rootfs.tar.gz
  sync && cd / && umount /mnt && reboot
EOF
echo ""
echo -e "${G}Strg+C zum Beenden${N}"
echo ""

cd "${OUTPUT_DIR}"
python3 -m http.server ${PORT}
