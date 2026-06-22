# Validation

Passed:

- authoritative loader-main test across two successive upstream commits;
- `LOADER_REF=latest` compatibility alias resolving to `main`;
- authoritative UI-main test across two successive upstream commits;
- clean-checkout and no-source-mutation assertions;
- static prohibition of meraki-redboot patch files and `git apply` logic;
- loader source contract test;
- documentation links across 46 Markdown files;
- meraki-redboot: 45 unit and source-contract tests;
- meraki-redboot Clang fixed-RAM structural builds for strict, development,
  permissive, and strict-UART configurations;
- Luton26 and Jaguar1 recovery payload structural builds;
- firmware flasher: 34 PMOSREC/UART protocol tests;
- interactive flasher scope, recovery-selection, and direct-preflight tests;
- six release artifact-manifest tests;
- board identity and postmerkos-hardware host tests;
- shell syntax and Python compilation checks.

The aggregate `make test-fwupdate`, `make test-image`, and `make test-modules`
wrappers exceeded the sandbox execution limit during long fixture/cleanup work.
Their independently run protocol, manifest, hardware, documentation, and
source-policy components listed above passed. No hardware flash was performed.
