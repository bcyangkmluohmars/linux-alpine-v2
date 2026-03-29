# RTL8370MB Switch API for sfgw

## Overview

sfgw muss den RTL8370MB Switch über SMI-Register konfigurieren um
VLANs, Port-Isolation, Trunking etc. zu steuern. Der Switch macht
das Forwarding in Hardware — sfgw sagt ihm nur WAS er tun soll.

Das Kernel-Modul `rtl8370mb_init` macht die Basis-Init (flat L2 bridge).
sfgw übernimmt danach die volle Kontrolle.

## Zugriff

### Von Userspace (sfgw)

SMI-Register via `SIOCSMIIREG` / `SIOCGMIIREG` ioctl auf `eth1`:

```rust
// Pseudo-code
let sock = socket(AF_INET, SOCK_DGRAM, 0);
let mut ifr = ifreq::new("eth1");

// SMI write: reg_addr -> val
ifr.phy_id = 0x1D;  // RTL8370MB MDIO address
ifr.reg_num = 31; ifr.val_in = 0x000E; ioctl(sock, SIOCSMIIREG, &ifr);  // addr mode
ifr.reg_num = 23; ifr.val_in = reg_addr; ioctl(sock, SIOCSMIIREG, &ifr); // target reg
ifr.reg_num = 24; ifr.val_in = value;    ioctl(sock, SIOCSMIIREG, &ifr); // data
ifr.reg_num = 21; ifr.val_in = 0x0003;   ioctl(sock, SIOCSMIIREG, &ifr); // write op

// SMI read: reg_addr -> value
ifr.reg_num = 31; ifr.val_in = 0x000E; ioctl(sock, SIOCSMIIREG, &ifr);
ifr.reg_num = 23; ifr.val_in = reg_addr; ioctl(sock, SIOCSMIIREG, &ifr);
ifr.reg_num = 21; ifr.val_in = 0x0001; ioctl(sock, SIOCSMIIREG, &ifr);  // read op
ifr.reg_num = 25; ioctl(sock, SIOCGMIIREG, &ifr); // -> ifr.val_out = data
```

Unlock vor Zugriff: `smi_write(0x13C2, 0x0249)`
Lock danach: `smi_write(0x13C2, 0x0000)`

### Voraussetzungen

- eth1 muss UP sein (erstellt den shared MDIO bus)
- PCA9575 GPIO Pin 4+8 müssen HIGH sein (Switch aus Reset)
- Root-Rechte nötig für SIOCSMIIREG

## VLAN Konfiguration

### Register

| Register | Adresse | Beschreibung |
|----------|---------|-------------|
| VLAN Member Config | 0x0728 + (idx * 4) | VLAN-Tabelle, 32 Einträge |
| VLAN Port PVID | 0x0700 + port | Port Default VLAN ID |
| VLAN Ingress Filter | 0x07A0 | Ingress VLAN Filtering enable per port |

### VLAN Member Config (4 Register pro Eintrag)

Jeder VLAN-Tabelleneintrag hat 4 16-Bit-Register:

```
Reg 0 (base+0): VLAN ID [11:0]
Reg 1 (base+1): Member ports [10:0] (bitmask, bit N = port N)
Reg 2 (base+2): Untag ports [10:0] (bitmask)
Reg 3 (base+3): FID [3:0] (filtering database ID)
```

### Beispiel: VLAN 10 auf Ports 0-3 + CPU, VLAN 20 auf Ports 4-7 + CPU

```
// VLAN 10: ID=10, members=ports 0,1,2,3,9  untag=ports 0,1,2,3
smi_write(0x0728, 10);        // VLAN ID
smi_write(0x0729, 0x020F);    // Members: bits 0-3 + bit 9
smi_write(0x072A, 0x000F);    // Untag: bits 0-3
smi_write(0x072B, 0x0001);    // FID 1

// VLAN 20: ID=20, members=ports 4,5,6,7,9  untag=ports 4,5,6,7
smi_write(0x072C, 20);        // VLAN ID
smi_write(0x072D, 0x02F0);    // Members: bits 4-7 + bit 9
smi_write(0x072E, 0x00F0);    // Untag: bits 4-7
smi_write(0x072F, 0x0002);    // FID 2

// Port PVIDs
smi_write(0x0700, 10);  // Port 0 → VLAN 10
smi_write(0x0701, 10);  // Port 1 → VLAN 10
smi_write(0x0702, 10);  // Port 2 → VLAN 10
smi_write(0x0703, 10);  // Port 3 → VLAN 10
smi_write(0x0704, 20);  // Port 4 → VLAN 20
smi_write(0x0705, 20);  // Port 5 → VLAN 20
smi_write(0x0706, 20);  // Port 6 → VLAN 20
smi_write(0x0707, 20);  // Port 7 → VLAN 20
smi_write(0x0709, 1);   // Port 9 (CPU) → VLAN 1 (trunk)
```

CPU-Port (9) bekommt tagged Frames für alle VLANs. sfgw erstellt
dann VLAN-Subinterfaces auf eth3 (eth3.10, eth3.20).

## Port Isolation

| Register | Adresse |
|----------|---------|
| Port N isolation | 0x08A2 + N |

Bitmask: Bit M gesetzt = Port N darf an Port M forwarden.

```
// Ports 0-3 nur untereinander + CPU:
smi_write(0x08A2, 0x020F);  // Port 0 → 0,1,2,3,9
smi_write(0x08A3, 0x020F);  // Port 1 → 0,1,2,3,9
smi_write(0x08A4, 0x020F);  // Port 2 → 0,1,2,3,9
smi_write(0x08A5, 0x020F);  // Port 3 → 0,1,2,3,9
```

## Port Link Status (read-only)

| Register | Adresse |
|----------|---------|
| Port N status | 0x1352 + N |

```
Bit 4: Link (1=up)
Bit 2: Duplex (1=full)
Bits 1:0: Speed (0=10M, 1=100M, 2=1000M)
Bits 6:5: Pause
```

## CPU Port / Tagging

| Register | Adresse | Beschreibung |
|----------|---------|-------------|
| CPU_PORT_MASK | 0x1219 | Welche Ports sind CPU-Ports (bitmask) |
| CPU_CTRL | 0x121A | CPU-Tag-Modus |

CPU_CTRL Bits:
- Bit 0: Enable
- Bits 2:1: Insert mode (0=all, 1=trapping, 2=none)
- Bits 5:3: Trap port (0-7)
- Bit 9: Tag format (0=8bytes, 1=4bytes)

Für VLAN-Trunk zum CPU: `INSERT_TO_ALL` (mode 0) mit 8-byte Tags.
Für flat bridge: `INSERT_TO_NONE` (mode 2), kein Tagging.

## EXT Port Konfiguration

| Register | Adresse | Beschreibung |
|----------|---------|-------------|
| DI_SELECT0 | 0x1305 | EXT0 bits[3:0], EXT1 bits[7:4] |
| DI_FORCE1 | 0x1311 | EXT1 force link config |
| EXT_RGMXF1 | 0x1307 | EXT1 RGMII TX/RX delay |

EXT Port Modes: 0=disable, 1=RGMII, 9=SGMII, 10=HSGMII(2.5G)

DI_FORCE Bits:
- Bit 12: Force enable
- Bit 7: Nway
- Bits 6:5: Pause
- Bit 4: Link
- Bit 2: Duplex
- Bits 1:0: Speed (0=10M, 1=100M, 2=1000M)

## Internal PHY Access

Für PHY-Level Konfiguration der 8 LAN-Ports:

```
Indirect access registers:
  0x1F00: Control (bit 0=CMD, bit 1=RW)
  0x1F01: Status (busy flag)
  0x1F02: Address = 0x2000 + (port * 32) + phy_reg
  0x1F03: Write data
  0x1F04: Read data
```

## STP State

| Register | Adresse | Beschreibung |
|----------|---------|-------------|
| MSTI0 ports 0-7 | 0x0A00 | 2 bits per port (0=disabled, 3=forwarding) |
| MSTI0 ports 8-10 | 0x0A01 | 2 bits per port |

## MAC Learning

| Register | Adresse |
|----------|---------|
| Port N learn limit | 0x0A20 + N |

Max: 2112 entries. Set to 0 to disable learning on a port.

## MIB Counters

Per-port traffic statistics. Register-Adressen sind chip-spezifisch
und müssen aus dem OpenWrt rtl8367c SDK referenziert werden.

## Interface Renaming

Die Stock-Firmware renamed Interfaces nach Ubiquiti-Konvention.
Unser Kernel hält die PCI-Probe-Reihenfolge.

| PCI Device | Unser Name | Stock Name | Funktion |
|-----------|------------|------------|----------|
| 00:00.0 | eth0 | eth9 | 10G SFP+ WAN |
| 00:01.0 | eth1 | eth8 | 1G WAN RJ45 |
| 00:02.0 | eth2 | eth10 | 10G SFP+ LAN |
| 00:03.0 | eth3 | eth0/switch0 | Switch Uplink |

sfgw muss entweder:
- Die Interfaces beim Start renamen (`ip link set ethX name ethY`)
- Oder intern über PCI-Adressen arbeiten statt über Interface-Namen

Das Renaming muss NACH den GPIO-Init aber VOR dem Networking passieren.
Die Stock-Firmware macht es in `/init.d/08-rename-if`.

## Migration von swconfig

sfgw nutzt aktuell `swconfig` (OpenWrt CLI-Tool) für die Switch-Config:
```
swconfig dev switch0 vlan 1 set ports "0t 1t 2t 3t 4t 5t 6t 7t 8t 9t"
```

`swconfig` gibt es auf unserem Kernel 6.12 nicht — das ist ein OpenWrt-
spezifisches Kernel-Framework. sfgw muss die `swconfig`-Aufrufe durch
direkte SMI-Register-Writes ersetzen (siehe API oben).

Der `swconfig`-Befehl `vlan X set ports "0t 1t ..."` mapped auf:
- VLAN Member Config Register (tagged/untagged pro Port)
- Port PVID Register
- Port Isolation Register

## sfgw Implementierung

sfgw sollte ein `Switch` Trait haben:

```rust
trait Switch {
    fn vlan_create(&self, vid: u16, ports: &[u8], untagged: &[u8]) -> Result<()>;
    fn vlan_delete(&self, vid: u16) -> Result<()>;
    fn port_set_pvid(&self, port: u8, vid: u16) -> Result<()>;
    fn port_set_isolation(&self, port: u8, mask: u16) -> Result<()>;
    fn port_get_link(&self, port: u8) -> Result<PortLink>;
    fn port_get_stats(&self, port: u8) -> Result<PortStats>;
}

struct Rtl8370mb {
    sock: RawFd,       // AF_INET socket for ioctl
    ifname: String,    // "eth1"
    phy_addr: u8,      // 0x1D
}
```

Referenz-SDK: OpenWrt `target/linux/generic/files/drivers/net/phy/rtl8367.c`
Mainline: `drivers/net/dsa/realtek/rtl8365mb.c`
