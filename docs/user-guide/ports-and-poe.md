# Ports, VLANs, STP, and PoE

Per-port configuration includes administrative state, name, PHY speed, flow control, Energy Efficient Ethernet, storm control, VLAN behavior, spanning tree, and PoE where the exact model exposes a verified controller path.

## VLAN modes

- Access: one untagged access/PVID VLAN
- Trunk: tagged allowed list and PVID
- Hybrid: tagged allowed list plus native/untagged VLAN

Required Click/PoE programming occurs before configuration is persisted. A missing mandatory handler rejects the transaction and triggers rollback rather than saving an unapplied desired state.

## PoE

Power and standard are independent:

- Power: enabled or disabled
- Standard: IEEE 802.3af or 802.3at
- Policy: normal or boot-prune

The PD690xx controller implements power control. Boot-prune enables configured ports, observes controller state/power for the configured window, and runtime-disables ports that remain unused without rewriting desired configuration. Reconnect after pruning requires manual re-enablement or Normal policy.

`/click/sw0_ctrl/poe_led_state` controls port LED indication only; it does **not** switch PoE power. It is registered only for MS220-8/MS220-8P.

The original PD690xx reset/enable GPIO initialization has been hardware-verified across the supported Luton26, Jaguar1 single-core, and Jaguar1 dual-core PoE models. Exact model profiles own the verified GPIO pair, and `S11poe` performs writes only when immutable board identity is exact and the model is PoE-capable. Non-PoE and unidentified systems remain write-disabled.
