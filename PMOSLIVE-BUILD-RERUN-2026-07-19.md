# PMOSLIVE build-fix rerun instructions

Date: 2026-07-19

## 1. Replace or commit the corrected meraki-redboot source

The builder log shows that meraki-builder fetched `origin/main` from
`Gadorach/meraki-redboot`. Push the corrected RedBoot tree to the selected
branch before using the default remote-source workflow, or point
`LOADER_REPO_URL` and `LOADER_REF` at a clean local Git commit.

## 2. Rebuild meraki-redboot cleanly

```bash
cd ~/src/ms42p-firmware/meraki-redboot
make clean
make all
```

The corrected PMOSLIVE source contains local freestanding `memcpy` and `memset`
implementations and initializes `rootfs_size` before validation.

## 3. Rebuild meraki-builder cleanly

Replace the builder source with the corrected archive, then run:

```bash
cd ~/src/ms42p-firmware/meraki-builder
make clean
REBUILD_KERNEL=1 REBUILD_LOADER=1 CLEAN_KERNEL=1 CLEAN_BUILDROOT=1 make all
```

On CachyOS, accept the Ubuntu 22.04 Distrobox prompt. `make clean` retains
source downloads but removes build stamps and generated Buildroot output.
`REBUILD_KERNEL=1` and `CLEAN_KERNEL=1` ensure the corrected parent Kconfig
symbols are resolved into a fresh Linux `.config`; `REBUILD_LOADER=1` prevents
reuse of an earlier RedBoot artifact.

For a local committed RedBoot checkout instead of GitHub:

```bash
./scripts/distrobox-run.sh env \
  LOADER_REPO_URL="file://$HOME/src/ms42p-firmware/meraki-redboot" \
  LOADER_REF="$(git -C "$HOME/src/ms42p-firmware/meraki-redboot" rev-parse HEAD)" \
  REBUILD_KERNEL=1 REBUILD_LOADER=1 CLEAN_KERNEL=1 CLEAN_BUILDROOT=1 \
  INCLUDE_UI=ask \
  ./scripts/build-all.sh
```

The local RedBoot tree must be a clean Git repository containing a commit.
