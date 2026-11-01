# AMEGA

**AMEGA** is a two-part local-network transfer system for moving PS4 game package files (`.pkg`) from a Windows PC to a PS4 running a compatible HEN environment.

The product has two native applications:
- **AMEGA for Windows** — selects packages, discovers paired consoles, offers transfers, and reports progress.
- **AMEGA for PS4** — discovers Windows senders, asks the user to accept, receives and verifies packages, and exposes compatible installation actions.

AMEGA is intended to work over a trusted home Wi-Fi/LAN without a cloud account or internet connection.

## Project status

<blockquote>
This repository currently contains the project foundation and protocol design. It does **not** yet contain installable Windows or PS4 builds. No transfer or installation flow is claimed to work until it has been implemented and tested on real hardware.
</blockquote>

## Design priorities

1. **User approval on both ends.** Nothing transfers or installs silently.
2. **Integrity first.** Every package is checked with SHA-256 before it is marked complete.
3. **Resume, do not restart.** Interrupted transfers should continue from verified byte ranges.
4. **No destructive cleanup.** AMEGA never deletes the source package automatically.
5. **Separate transfer from installation.** Receiving a PKG and installing it are distinct actions.
6. **HEN compatibility is explicit.** Installation is provided only through a detected and supported GoldHEN/HEN mechanism; unsupported firmware must fail safely.
7. **Local-first.** Discovery and transfer stay on the local network.

## Repository map

- `docs/architecture.md` — system boundaries and transfer lifecycle.
- `docs/protocol-v1.md` — discovery and transfer protocol.
- `docs/security-model.md` — pairing, trust, and safe file handling.
- `docs/ps4-hen-compatibility.md` — HEN/GoldHEN integration constraints.
- `docs/roadmap.md` — implementation stages and acceptance criteria.
- `protocol/schemas/` — machine-readable protocol schemas.
- `src/windows/` — Windows application.
- `src/ps4/` — PS4 homebrew application.

## Planned first milestone

A real end-to-end LAN test:
1. Windows app starts and advertises itself on the LAN.
2. PS4 app discovers the PC and displays a pairing/transfer request.
3. User approves on PS4.
4. PS4 downloads a selected test file in resumable chunks.
5. Both sides verify the final SHA-256 digest.
6. UI reports success only after verification.

PKG installation will be a later, separately tested milestone.

## Compatibility

The initial target is Windows 10/11 x64 and a PS4 running a compatible HEN/GoldHEN setup. Exact firmware and GoldHEN capabilities must be detected and verified; AMEGA will not assume that every HEN version exposes the same services.

## Contributing

Read the architecture, protocol, and security documents before changing network behavior. Any protocol change must update its schema and test vectors together.
