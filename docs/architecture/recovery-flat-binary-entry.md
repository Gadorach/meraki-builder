# Recovery flat-binary entry contract

meraki-redboot stage 1 loads a recovery `.bin` at `0x81000000` and jumps to
that exact address. An ELF `ENTRY(_start)` declaration alone is insufficient:
MIPS ABI metadata such as `.reginfo` or `.MIPS.abiflags` may occupy the lowest
allocated address and therefore become byte zero after `objcopy -O binary`.

The original v0.7.0 payload could consequently produce:

```text
PMOSBOOT PASS-RECOVERY-COPY: LOAD: 0x81000000
PMOSBOOT PASS-RECOVERY-EXEC: ENTRY: 0x81000000
```

without ever reaching `PMOSREC READY 3`.

The corrected source has a dedicated `entry.S` in `.text.start`, links it first,
discards non-runtime MIPS metadata, asserts `_start == 0x81000000`, initializes
stack/GP/BSS, and then calls `recovery_main`. Every generated descriptor and
release manifest declares `entry_contract: flat-binary-byte-zero-v1`.

The correction lives in the authoritative meraki-redboot repository. The builder
fetches `origin/main`, validates that the entry contract is present, and refuses
to emit a loader or complete image when it is absent. It never patches the
checkout. For hardware still running the original loader, use menu option 1 to
upload the corrected external stage. Once a corrected loader is flashed, menu
option 2 is safe for later recovery.
