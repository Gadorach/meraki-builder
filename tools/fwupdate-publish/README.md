# Firmware publication

Publishes a full 16 MiB firmware image to an HTTP(S) repository and atomically
updates `index.tsv`.

- `--modern` is the default and validates/publishes the firmware checksum,
  artifact manifest, and manifest checksum.
- `--checksum-only` publishes only the original firmware and `.sha256` files
  for the current updater's explicit `--no-manifest` path.
- `--legacy` publishes the two-file checksum-only artifact contract.

The two-file modes never synthesize a manifest. A version argument is therefore
recommended when the image has no embedded `PMOSMETA` release record.
