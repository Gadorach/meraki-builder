# High-RAM flat-binary entry contract

meraki-redboot stage 1 loads PMOSREC and PMOSLIVE flat binaries at
`0x86c00000` and jumps to byte zero of the selected binary. The Linux kernel
retains its normal load and entry address at `0x81000000`, so the UART service
payload remains resident while it receives and prepares the kernel.

The fixed-RAM stage is stored once at boot-region flash offset `0x20000` and is
shared by the independent active and fallback loader bodies. This keeps the
complete PMOSREC/PMOSLIVE stage within the 256 KiB dual-loader wrapper.

ELF entry metadata alone is not sufficient: MIPS metadata sections must not
precede executable startup code in the `objcopy -O binary` output. Each payload
uses a dedicated `entry.S` in `.text.start`, links it first, discards
non-runtime MIPS metadata, asserts `_start == 0x86c00000`, initializes its own
high-RAM stack/GP/BSS, and calls the payload entry point. Every generated
descriptor and release manifest declares:

```text
entry_contract: flat-binary-byte-zero-v1
load_address: 0x86c00000
entry_address: 0x86c00000
```

PMOSLIVE additionally declares a fixed non-overlapping runtime map:

```text
boot params:  physical 0x00000400-0x00000fff (KSEG1 0xa0000400)
kernel:       0x81000000
PMOSLIVE:     0x86c00000 (physical 108-112 MiB)
squashfs:     0x87000000 (physical 112-120 MiB)
linux memory: 120 MiB
reserved top: 8 MiB (physical 120-128 MiB)
```

The SquashFS region is intentionally inside the `mem=120M` range so the legacy
initrd parser can reserve it. The low-page boot parameters are above the MIPS
exception-vector area and below the decompressed kernel start at physical
`0x1000`.

The authoritative meraki-redboot repository owns these contracts. The builder
fetches the selected upstream revision, verifies the declarations and
structural builds, and refuses to emit a loader or complete image when the
contracts are absent. The builder never patches the upstream checkout.

Menu option 1 uploads a manifest-matched external high-RAM payload. Menu option
2 executes embedded PMOSREC. Jaguar1 menu option 3 executes embedded PMOSLIVE
only when its descriptor, digest, no-flash policy, and RAM layout match the
release manifest.
