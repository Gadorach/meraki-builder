# Vitesse switch kernel modules

The integrated build replaces this placeholder with the complete binary vendor
module tree extracted from the donor firmware. One image must contain all
supported platform families:

- common: `elts_meraki.ko`, `merakiclick.ko`, `proclikefs.ko`;
- Luton26: `luton26/{vtss_core,vc_click}.ko`;
- Jaguar1: `jaguar/{vtss_core,vc_click}.ko`;
- Jaguar Dual: `jaguar_dual/{vtss_core,vc_click}.ko`;
- every additional donor `.ko` recorded by the complete module inventory.

`S08kmods` identifies the switch at boot and loads only the matching family.
The build does not create model-specific rootfs images.

The generated tree includes:

- `postmerkos-required-modules.txt` — mandatory common and family objects;
- `postmerkos-all-modules.txt` — every donor kernel object included;
- `postmerkos-modules.sha256` — exact hashes for that complete inventory.

Source archives intentionally do not contain the proprietary module binaries.
They are materialized from the donor during the build, and every build stage
fails if the platform-complete contract is not satisfied.
