# Inputs for the NOR research image helper

This directory no longer accepts or recommends a downloaded patched RedBoot
binary. The supported boot region is always built from
`Gadorach/meraki-redboot` source by the repository-level builder:

```sh
cd ../../../../
make loader
```

`../make.sh` defaults to these generated files:

```text
../../../../artifacts/loader1.bin
../../../../artifacts/tools/mkvcoreiii_payload.py
```

The loader must be exactly 256 KiB. The kernel input must be the compressed
`vmlinuz.bin`; the helper uses meraki-redboot's canonical packer to add the
32-byte SPIM header, 32-byte payload alignment, and CRC-32. By default it reads
`vmlinuz.bin` from the current directory, `bin/bootubi.new` for SquashFS, and
`bin/jffs2` for the 5 MiB configuration region. Paths can be overridden with
`LOADER`, `PAYLOAD_PACKER`, `KERNEL_BIN`, `ROOTFS`, `JFFS2`, and `OUTPUT`.

The watchmysys donor remains permitted only as an input to the normal module
extraction workflow. No donor or historical RedBoot binary is copied into a
new image.
