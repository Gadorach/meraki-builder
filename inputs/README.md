# Local binary inputs

This directory is for untracked donor firmware used by the VCore-III build.
The donor is retained only to extract the currently required proprietary
kernel modules and supporting runtime files.

Accepted donor names include:

```text
postmerkOS-20240818.bin   complete 16 MiB donor firmware
donor-firmware.bin       complete 16 MiB donor firmware
good-rootfs.squashfs     standalone donor SquashFS
```

The default complete donor remains available from watchmysys when
`AUTO_DOWNLOAD_DONOR=1` is enabled or the interactive build authorizes the
download.

Bootloader binaries are not accepted here. The supported VCore-III loader is
always cloned from `Gadorach/meraki-redboot` and compiled from source, using the
selected release's own toolchain and payload packer.

Do not commit donor firmware, proprietary modules, or generated images unless
redistribution is explicitly permitted.
