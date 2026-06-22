# postmerkOS console

## Purpose

`postmerkos-console` is the primary interactive management interface. It runs over hardware serial and SSH, uses configd’s local Unix socket, and exposes only actions permitted by the authenticated administrator, operator, or viewer role.

## Session behavior

The hardware serial path uses getty and a persistent `postmerkos-serial-login` supervisor. JFFS2 security policy selects direct console, root-only login, or normal role-aware user login. Logging out returns to the serial `pmc:` prompt; entering a raw shell and exiting returns to the console. The direct serial prompt also supports `login`, `status`, `logs`, `retry`, `shell`, `reboot`, and `help`. Interactive SSH users are launched into the same interface, while non-interactive SSH commands are left untouched.

The console obtains one stable session record at launch and caches its role/capability list for menu presentation. Configd still enforces every operation independently. A failed local-socket request is reported as management-service unavailability; only a successful session explicitly returning role `none` is reported as a role denial. Menu startup retries the service for a bounded interval.

Aliases:

```text
pmc -> postmerkos-console
postmerkos-cli -> postmerkos-console
```

Non-interactive SSH, SCP/SFTP, and automation are not intercepted. Entering the raw shell prints instructions to run `pmc` to return.

## Main menu

```text
1) Status                         5) User Management
2) Port Configuration             6) Service Management
3) Firmware Update                7) Power Control
4) Backup & Restore               8) Shell
```

Choices are filtered by configd capabilities. Administrators see every entry, operators can manage switching, create backups, and reboot, and viewers receive read-only status. Raw service JSON is available only as an explicit diagnostic action; the normal Service Management entry uses a compact status table.

## Port handling

Status is paged in twelve-port copper groups followed by detected uplink/SFP ports. Port configuration includes VLAN, STP, speed, flow control, EEE, storm control, and capability-dependent PoE. Boot-pruned PoE ports can be re-enabled for the current boot or changed to Normal policy.

## Updates and backups

Before firmware installation, the console recommends a TFTP configuration backup, offers a copyable JSON display with SHA-256, allows an explicit skip, or cancels. The Firmware Update menu can receive a framed firmware image and optional manifest over hardware UART through `postmerkos-console firmware uart`. Untested artifacts require explicit acknowledgement; known-incompatible artifacts are identified separately and cannot be acknowledged. Update history is read from the persistent post-reboot record rather than a transient process check.

## Console output

While the menu owns hardware serial, routine kernel and service chatter is reduced and restored on exit. Critical factory-reset and firmware-update messages remain visible.

## Build integration

The package is included in every switch build. The optional browser interface is independent and shares the same configd backend.
