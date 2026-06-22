# Flash layout and persistence

The Vitesse switch image occupies a 16 MiB NOR device:

| Region | Start | End | Size | Purpose |
|---|---:|---:|---:|---|
| meraki-redboot | `0x00000000` | `0x0003ffff` | 256 KiB | source-built loader, fixed-RAM menu, embedded recovery |
| Kernel | `0x00040000` | `0x002fffff` | 2816 KiB | 32-byte SPIM header plus compressed kernel payload |
| SquashFS | `0x00300000` | `0x00afffff` | 8 MiB | read-only root filesystem |
| JFFS2 | `0x00b00000` | `0x00ffffff` | 5 MiB | persistent configuration/state |

## Kernel region contract

The kernel region begins with eight little-endian 32-bit words:

1. magic `0x4d495053` (`SPIM` in flash byte order);
2. load address `0x81000000`;
3. padded payload byte count;
4. entry point `0x81000000`;
5. IEEE CRC-32;
6. reserved zero;
7. reserved zero;
8. reserved zero.

The compressed kernel is zero-padded to a 32-byte boundary. Its padded size may
not exceed `0x002bffe0`, leaving the 32-byte header inside the `0x002c0000`
kernel region. CRC-32 is calculated over the complete header with word 5 set to
zero followed by the entire padded payload. Image generation and validation use
the canonical packer from the selected meraki-redboot source checkout.

The build accepts any SquashFS that fits within exactly 8,388,608 bytes. No
artificial reserve is enforced. JFFS2 stores switch, service, security, time,
account, key, repository, update-history, and compatibility-acknowledgement
state. Factory reset erases this region and leaves loader, kernel, and rootfs
intact.
