# PMOSLIVE memory and boot-region map

## DRAM physical map

| Physical range | KSEG alias / purpose |
|---|---|
| `0x00000000-0x000003ff` | exception-vector reservation |
| `0x00000400-0x00000fff` | boot parameters via uncached `0xa0000400-0xa0000fff` |
| `0x00001000+` | decompressed kernel destination begins here |
| `0x01000000` | compressed SPIM kernel destination, KSEG0 `0x81000000` |
| `0x01400000-0x023fffff` | unchanged 16 MiB image staging, KSEG0 `0x81400000` |
| `0x02400000+` | manifest/work area, KSEG0 `0x82400000` |
| `0x06c00000` | PMOSREC/PMOSLIVE load, KSEG0 `0x86c00000` |
| `0x07000000-0x077fffff` | SquashFS external initrd, KSEG0 `0x87000000` |
| `0x07800000-0x07efffff` | excluded top-of-RAM headroom |
| `0x07f00000-0x07ffffff` | fixed stage-1 reservation, uncached `0xa7f00000` |

Linux command line: `mem=120M`.

## 256 KiB RedBoot flash region

| Flash offset | Purpose |
|---|---|
| `0x00000-0x0045b` | vectors, dispatcher, active/fallback descriptors |
| below `0x20000` | active and fallback compact loader bodies |
| `0x20000` | shared stage-1 blob |
| after stage through `0x3ffff` | reserved/padding |
| `0x40000` | SPIM kernel payload header |
