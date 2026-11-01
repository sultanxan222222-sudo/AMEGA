# PS4 HEN / GoldHEN compatibility

## Scope

The first console target is a PS4 with a compatible HEN environment, including GoldHEN where its documented capabilities are available. HEN versions, firmware versions, payloads, and enabled services differ. AMEGA must detect capabilities at runtime instead of assuming a fixed firmware number or endpoint.

**Important distinction:** HEN/GoldHEN and the separate "Remote Package Installer" homebrew are not the same application. HEN being active does not prove that the RPI app is installed, open, or listening.

## Two separate capabilities

**File reception** is AMEGA's own LAN transfer feature. It must work independently of whether remote package installation is available.

**Package installation** is an optional adapter to a supported console mechanism. Its availability must be reported separately from ordinary reception.

## Integration modes

### Native AMEGA receiver — primary target

The AMEGA PS4 application discovers Windows senders, asks the user to accept or decline, receives into a staging directory, verifies the file, and exposes a separate installation action when a supported installer capability is detected.

### Existing Remote Package Installer — optional compatibility

A documented third-party Remote Package Installer workflow uses an HTTP API on port 12800, including `/api/is_exists` and `/api/install`. The receiver app must remain in the foreground for that API workflow. The Windows sender must implement correct HTTP Range responses because package parsing requires arbitrary byte offsets.

This compatibility path is optional. AMEGA must never silently assume the RPI app is installed merely because HEN is active.

## Integration rules

- Prefer documented and version-checked interfaces.
- Keep GoldHEN-specific and existing-RPI code behind separate installation-adapter boundaries.
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
- whether a separate Remote Package Installer app is installed;
- wired Ethernet versus Wi-Fi;
- package size tested;
- receive, resume, digest verification, and installation outcomes.

No firmware compatibility claim is valid until it has a reproducible test record.
