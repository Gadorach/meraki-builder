# config_apply module

`config_apply.c` is the transaction coordinator for desired state.

## Delta transaction

1. Load and deep-copy the current complete JSON.
2. Deep-merge and validate the requested delta.
3. Apply touched Click, PoE, network, and service sections to runtime.
4. Reject on any required failure and attempt rollback to the previous runtime state.
5. Atomically persist only after required application succeeds.
6. Refresh the known-good backup only from the validated committed primary.

At daemon startup `config_apply_full()` replays all settings. Storm control remains donor-dependent and is classified as optional because the binary `SwitchPortTable` does not provide reliable readback on every graph. Required port/VLAN/STP/PHY/PoE and management-network handlers are not downgraded to warnings. Results identify warnings, pending work, unsupported operations, failures, and runtime degradation.
