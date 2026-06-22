# Fixed-size embedded metadata correction

The complete postmerkOS release manifest exceeded the historical 4 KiB trailer
slot after UART recovery, direct-member lookup, SPI enable, and destructive
hardware-preflight contracts were added. The trailer is a fallback index, not
the authoritative manifest, so expanding it or deleting capability metadata
would be the wrong fix.

The post-image stage now writes a compact canonical JSON record containing:

- version and build identity;
- target family;
- image/API/schema versions;
- exact-model compatibility states;
- `metadata_profile: embedded-update-index-v1`.

The full release document remains:

- `/etc/postmerkos-release.json` inside SquashFS;
- `postmerkos-release.json` in Buildroot images;
- `<firmware>.manifest.json` with its SHA-256 sidecar.

The image-only updater fallback needs only version and model states. Modern
flashing paths continue to use the complete checksummed sidecar.
