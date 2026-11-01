# PS4 HEN / GoldHEN compatibility

## Scope

The first console target is a PS4 with a compatible HEN environment, including GoldHEN where its documented capabilities are available. HEN versions, firmware versions, payloads, and enabled services differ. AMEGA must detect capabilities at runtime instead of assuming a fixed firmware number or endpoint.

## Two separate capabilities

**File reception** is AMEGA's own LAN transfer feature. It must work independently of whether remote package installation is available.

**Package installation** is an optional adapter to a supported console mechanism. Its availability must be reported separately from ordinary reception.

## Integration rules

- Prefer documented and version-checked interfaces.
- Keep GoldHEN-specific code behind an installation-adapter boundary.
- Detect availability before enabling the Install action.
- Never silently fall back to an undocumented port or payload.
- Show whether a package is merely received, verified, queued for installation, or confirmed installed.
- Do not remove received files after installation unless the user separately confirms deletion.
- Keep test packages available for safe transfer testing without attempting installation.

## Hardware validation matrix

Before release, test on real hardware and record:
- exact PS4 model;
- firmware version;
- HEN/GoldHEN version;
- enabled GoldHEN services;
- wired Ethernet versus Wi-Fi;
- package size tested;
- receive, resume, digest verification, and installation outcomes.

No firmware compatibility claim is valid until it has a reproducible test record.
