# PMOSLIVE device validation

A successful PMOSLIVE boot must report all of the following:

- `VCOREIII PROM ARGV-ACCEPTED`
- `MIPS CMDLINE-SOURCE=firmware`
- `Initial ramdisk at: 0x87000000`
- `root=/dev/ram0`, `mem=120M`, and `postmerkos.live=1`
- `VFS: Mounted root ... device 1:0`
- `PMOSLIVE USERSPACE-READY ROOT=ram0 OVERLAY=tmpfs FLASH_MOUNTED=0`

The command line also carries the exact package target as
`postmerkos.model=MS42` or `postmerkos.model=MS42P`.  Userspace prefers the
physical board EEPROM but uses this signed-package-derived hint when live mode
cannot read the EEPROM or a board-config flash partition.  It is ignored for
normal flash boots and rejected unless it names a supported exact model.

On the target, inspect:

```sh
cat /proc/cmdline
cat /run/postmerkos/live-mode
cat /run/postmerkos/boardinfo
cat /run/postmerkos/module-profile
postmerkosctl summary
mount
cat /proc/mtd
lsmod
ip address show
```

The serial and management consoles display:

```text
*** PMOSLIVE RAM SESSION ***
Firmware is running from RAM. Changes will not survive a reboot.
```

To verify volatility, create files under `/etc` and `/root`, power-cycle the
switch, and confirm that the files are absent after the normal flash boot.
Never write directly to `/dev/mtd*` during a PMOSLIVE validation run.

The Linux 3.18 RAM-disk loader emits an animated `| / - \\` spinner using
backspace characters.  The host flasher collapses those edits into one clean
progress line; they are not a kernel crash.


Platform completion attestation:

```text
PMOSLIVE PLATFORM-READY MODEL=MS42P SOURCE=pmoslive-command-line
```

The host flasher waits for this marker after the RAM-root userspace attestation and verifies that the reported model matches the selected firmware target.
