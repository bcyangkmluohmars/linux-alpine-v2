# Kernel / Boot TODO

## Sicherheit
- [ ] U-Boot Signaturprüfung: eigenen Signing-Key flashen oder Mainline U-Boot mit eigenem Key bauen (aktuell: Signaturprüfung übersprungen)
- [ ] Secure Boot Chain: U-Boot → signiertes uImage → verifiziertes rootfs

## Architektur
- [ ] rtl8370mb Kernel-Modul raus — Switch-Init komplett in sfgw (Rust, via ioctl)
- [ ] Alpine auf Minimal-Initramfs reduzieren: Kernel + initramfs + sfgw als PID 1 (optional, Alpine ist aktuell ~110MB RAM idle)
- [ ] GPIO-Init (PCA9575) in sfgw statt Shell-Script (über /dev/i2c-0)
- [ ] Interface-Renaming in sfgw statt Shell-Script (über netlink)

## Build
- [ ] Inkrementeller Kernel-Build: Docker Volume caching funktioniert, aber `--modules` braucht noch vollen `modules_prepare` Lauf
- [ ] Host-Key persistent machen (jeder Reboot generiert neue SSH Host-Keys → known_hosts Warnung)

## Switch
- [ ] VLAN-Support in sfgw über SMI-Register (SWITCH-API.md)
- [ ] Port-Mirroring für Monitoring
- [ ] MIB-Counter auslesen für Port-Statistiken
- [ ] Link-Status Polling / Interrupts für Port-Events

## Netzwerk
- [ ] nftables persistent: sfgw muss SSH-Regel beim Start setzen (aktuell manuell)
- [ ] WAN-Port(s) Konfiguration durch sfgw
- [ ] Bridge/VLAN-Interfaces automatisch anlegen

## Performance / XDP
- [ ] XDP-Support in al_eth einbauen (ndo_bpf + ndo_xdp_xmit) — zero-copy Paket-Forwarding
- [ ] AF_XDP Socket-Integration in sfgw (Rust) — Routing/Firewall direkt auf DMA-Ring-Buffern
- [ ] IPsec mit AES-256-GCM über al_ssm HW-Crypto (PCI 00:04.0) — verschlüsseltes Line-Rate Routing
- [ ] Combo: XDP + al_ssm = 10G verschlüsseltes Routing auf Hardware die Ubiquiti mit 1G verkauft hat

## Hardware
- [ ] PCA9575 GPIO Pin-Mapping vollständig dokumentieren (nur Pin 4, 5, 8 bekannt)
- [ ] LCM Display (ttyACM0) — Front-Panel-LCD ansteuern
- [ ] HDD Power Control verifizieren (Stock nutzt gpio3:5, nicht pca9575:5)
- [ ] SATA LED (Stock nutzt gpio2:6, nicht sgpo:6)
