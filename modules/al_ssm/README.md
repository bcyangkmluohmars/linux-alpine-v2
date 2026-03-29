# Annapurna Labs Alpine V2 SSM Crypto Engine Driver

Out-of-tree Linux kernel module for the Alpine V2 SoC hardware crypto accelerator
(SSM = Security Services Module). Provides hardware-accelerated AES encryption for
dm-crypt/LUKS, registered via the Linux kernel crypto framework.

## Hardware

- **PCI device:** vendor 0x1c36, device 0x0022, class 0x100000
- **BAR 0:** UDMA registers (128KB at 0xfe080000)
- **BAR 4:** Application/crypto registers (64KB at 0xfe0a0000)
- **SoC:** Annapurna Labs (Amazon) Alpine V2 (aarch64)

The SSM crypto engine supports:
- **AES-128/192/256** in ECB, CBC, CTR, CCM, GCM, XTS modes
- **3DES** in ECB, CBC modes
- **SHA-1, SHA-2** (256/384/512), **SHA-3** (224/256/384/512)
- **MD5** (legacy)
- **GMAC**, CRC-8/16/32
- **LZ77/LZSS/LZ4/Deflate** compression (V3 only)

This driver currently registers:
- `xts(aes)` — AES-XTS (used by dm-crypt default)
- `cbc(aes)` — AES-CBC (used by dm-crypt legacy)

## Architecture

```
dm-crypt / LUKS
       |
   Linux crypto API  (crypto_register_skciphers)
       |
   al_ssm_main.c     PCI driver + crypto framework glue
       |
   AL HAL SSM         al_hal_ssm_crypto.c  (DMA prepare/action/complete)
       |               al_hal_ssm_crypto_cfg.c (SA configuration)
       |               al_hal_ssm.c  (DMA init, register mapping)
       |
   AL HAL UDMA        al_hal_udma_main.c, al_hal_m2m_udma.c
       |               (Unified DMA engine, shared with Ethernet and RAID)
       |
   Hardware           SSM crypto engine at PCI 0000:00:04.0
```

## Building

```bash
# Cross-compile for Alpine V2 (aarch64)
make KDIR=/path/to/linux-6.12-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# Install
make KDIR=/path/to/linux-6.12-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_install

# Or copy manually
scp al_ssm.ko root@10.0.0.1:/lib/modules/$(uname -r)/extra/
ssh root@10.0.0.1 depmod -a
```

## Loading

```bash
modprobe al_ssm

# Verify registration
cat /proc/crypto | grep -A5 "xts-aes-al-ssm"
# Should show:
#   driver       : xts-aes-al-ssm
#   module       : al_ssm
#   priority     : 400
#   type         : skcipher

# The priority (400) is higher than ARM64 NEON (200),
# so dm-crypt will automatically use hardware crypto.
```

## dm-crypt / LUKS Usage

Once loaded, dm-crypt automatically uses the hardware engine:

```bash
# Create LUKS volume with AES-XTS (hardware accelerated)
cryptsetup luksFormat /dev/sda1 --cipher aes-xts-plain64 --key-size 256

# Open
cryptsetup luksOpen /dev/sda1 secure_data

# The /dev/mapper/secure_data device now uses hardware AES
```

## Notes

- Ubiquiti firmware has `# CONFIG_AL_SSM_PCIE is not set` — the crypto engine
  is present but disabled in stock firmware. This driver enables it.
- The SSM and DMA/RAID engines share PCI device ID 0x0022 but have different
  class codes: 0x100000 (crypto) vs 0x010400 (RAID).
- The ARM64 NEON AES (`aes-arm64`) is the software fallback. The hardware
  engine should be faster for large block operations (dm-crypt 4KB+ sectors).

## Source

- **HAL code:** From [delroth/alpine_hal](https://github.com/delroth/alpine_hal)
  (GPLv2 dual-licensed by Annapurna Labs Ltd.)
- **Driver glue:** Written for kernel 6.12 LTS
- **UDMA infrastructure:** Shared with al_eth (Ethernet) driver

## License

GPL-2.0 (HAL code is dual-licensed GPL/Annapurna Commercial; driver glue is GPL-2.0)
