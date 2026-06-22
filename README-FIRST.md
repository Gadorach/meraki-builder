# Authoritative upstream-main builder correction

The failure was caused by meraki-builder resolving `LOADER_REF=latest` to the
highest release tag (`0.7.0`) and then attempting to replay loader/recovery
patches against that old checkout.

The corrected builder now:

- tracks `Gadorach/meraki-redboot:main` by default;
- tracks `Gadorach/postmerkos-ui:ms42p-dev` by default;
- treats `LOADER_REF=latest` only as a compatibility alias for `main`;
- fetches and resets to the selected upstream branch on every preparation;
- never applies patches or creates repair commits in either upstream checkout;
- rejects dirty or contract-incompatible upstream source;
- records the exact selected commit in artifact provenance.

Only meraki-builder required modification. The included meraki-redboot tree is
the supplied current source snapshot and is intentionally unchanged.

## Install

Copy `repos/meraki-builder/` over the existing meraki-builder working tree
without replacing its `.git` directory, then commit the changes normally.

Existing clean `.work/sources/meraki-redboot` and postmerkos-ui checkouts may be
kept. The next source preparation will fetch `origin/main` for meraki-redboot and
`origin/ms42p-dev` for postmerkos-ui, then reset each checkout to that branch.

```sh
cd ~/src/ms42p-firmware/meraki-builder
make sources
REBUILD_LOADER=1 make all
```

A source checkout with uncommitted changes now stops the build. Commit those
changes to the appropriate upstream repository first.
