# AMEGA

**AMEGA** is a two-part local-network transfer system for moving PS4 game package files (`.pkg`) from a Windows PC to a PS4 running a compatible HEN environment.

The product has two native applications:
- **AMEGA for Windows** — selects packages, discovers paired consoles, offers transfers, and reports progress.
- **AMEGA for PS4** — discovers Windows senders, asks the user to accept, receives and verifies packages, and exposes compatible installation actions.

AMEGA is intended to work over a trusted home Wi-Fi/LAN without a cloud account or internet connection.

## Project status

> The repository contains the project foundation and a Windows discovery prototype. The Windows code is being built by GitHub Actions. There is not yet an installable PS4 AMEGA package, and end-to-end transfer has not been validated on real hardware.

## Design priorities

1. **User approval on both ends.** Nothing transfers or installs silently.
2. **Integrity first.** Every package is checked with SHA-256 before it is marked complete.
3. **Resume, do not restart.** Interrupted transfers should continue from verified byte ranges.
4. **HTTP Range correctness.** PS4 package-install workflows can request arbitrary byte ranges; a normal whole-file-only HTTP response is not sufficient.
5. **No destructive cleanup.** AMEGA never deletes the source package automatically.
6. **Separate transfer from installation.** Receiving a PKG and installing it are distinct actions.
7. **HEN compatibility is explicit.** HEN does not automatically mean a separate Remote Package Installer app is installed or running.
8. **Local-first.** Discovery and transfer stay on the local network.

## Repository map

- `docs/architecture.md` — system boundaries and transfer lifecycle.
- `docs/protocol-v1.md` — discovery and transfer protocol.
- `docs/security-model.md` — pairing, trust, and safe file handling.
- `docs/ps4-hen-compatibility.md` — HEN/GoldHEN integration constraints.
- `docs/open-source-research.md` — reusable references and licensing boundaries.
- `docs/roadmap.md` — implementation stages and acceptance criteria.
- `protocol/schemas/` — machine-readable protocol schemas.
- `src/windows/` — Windows application.
- `src/ps4/` — PS4 homebrew application.

## Current Windows prototype

The first Windows prototype can choose a PKG, calculate SHA-256, announce the PC over UDP, and listen for AMEGA PS4 receiver beacons. It exposes health and offer metadata only; it does not transfer or install files yet.

## Planned first end-to-end milestone

1. Windows app and PS4 app discover one another.
2. Both sides complete a short-lived pairing confirmation.
3. User accepts a package on the PS4.
4. PS4 receives the package with resumable byte ranges.
5. Both sides verify the final SHA-256 digest.
6. UI reports success only after verification.

PKG installation will be a later, separately tested milestone.

## Compatibility

The initial target is Windows 10/11 x64 and a PS4 running a compatible HEN/GoldHEN setup. Exact firmware and GoldHEN capabilities must be detected and verified; AMEGA will not assume that every HEN version exposes the same services.

## Contributing

Read the architecture, protocol, and security documents before changing network behavior. Any protocol change must update its schema and test vectors together.
