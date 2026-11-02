# AMEGA

**AMEGA** is a two-part local-network system for sending PS4 package files (`.pkg`) from a Windows PC to a compatible PS4 homebrew environment.

## Development status — engineering branch 0.2.2

Branch: `fix/0.2.2-hash-ui-ps4-capabilities`.

This branch separates device discovery from long-file hashing:
- the user can start discovery before choosing a PKG;
- discovery can continue while SHA-256 is calculated;
- the UI keeps file selection available and visibly marks the active cancellation action;
- a completed hash updates the offer metadata without restarting discovery;
- the PS4 screen displays its detected local IPv4 address to make network troubleshooting easier.

These changes are being built by CI. A successful compile is not a substitute for real PS4 hardware validation.

## Honest feature status

**Implemented in the prototype:** Windows package picker, asynchronous SHA-256, IPv4 adapter/subnet detection, nonce-bound UDP discovery, PS4 receiver list, Russian UI, DualShock navigation, startup diagnostics and capability research.

**Not yet implemented end-to-end:** pairing authorization, package-byte transfer, receiver-side staging, resume/pause, receiver-side SHA-256 verification, and an installer adapter. The current application does not yet send the selected PKG to the PS4. Do not treat it as a finished game-transfer or installation product.

## Main engineering documents

- `docs/ps4-platform-engineering-audit-2026-11.md` — firmware, GoldHEN/HEN layers, OpenOrbis APIs, network, storage and installation research.
- `docs/architecture.md` — system boundaries and lifecycle.
- `docs/protocol-v1.md` — discovery and transfer protocol.
- `docs/security-model.md` — pairing and safe file handling.
- `docs/ps4-hen-compatibility.md` — firmware and installer compatibility.
- `docs/open-source-research.md` — prior projects, references and licensing boundaries.
- `docs/roadmap.md` — staged implementation and acceptance criteria.

## Platform strategy

- Windows 10/11 x64 sender.
- PS4 homebrew receiver built with OpenOrbis.
- Discovery and native AMEGA transfer must work without relying on optional GoldHEN services.
- GoldHEN, its built-in Remote Package Install feature, FTP, internal `/data/pkg`, and a separate Remote Package Installer app are distinct capability checks/adapters.
- HTTP Range semantics are mandatory for any remote installer route that reads arbitrary package offsets.
- Installation is always separate from transfer and requires explicit confirmation.

## Release gate

Do not describe AMEGA as release-ready until a Windows and PS4 build have been installed and exercised in the same real-hardware test: discover, pair, transfer a large test file, interrupt/resume, verify SHA-256, and verify the installation state if installation is included.