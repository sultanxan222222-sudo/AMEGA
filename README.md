# AMEGA

**AMEGA** is a two-part local-network system for sending PS4 package files (.pkg) from a Windows PC to a compatible PS4 homebrew environment.

## Development status — engineering branch 0.2.2

Branch: `fix/0.2.2-hash-ui-ps4-capabilities`.

This engineering branch includes:
- Windows discovery can start before package selection and continue during background SHA-256 hashing.
- File selection remains responsive; hashing progress and cancellation are visible.
- PS4 screen displays its detected local IPv4 address.
- PS4 input adds SDL GameController, raw Joystick fallback, and controller hot-plug detection.
- Windows now has a server-side receiver-authorization endpoint and a streaming HTTP file route with byte-range support. The route requires a short-lived token and the authorized PS4 IP.
- PS4 CI is pinned to the exact OpenOrbis container image digest used for the successful build.
- Firmware/HEN/GoldHEN and installation research is recorded in the platform audit.

## Honest feature status

**Implemented in the prototype:** Windows package picker, asynchronous SHA-256, IPv4 adapter/subnet discovery, nonce-bound UDP discovery, PS4 receiver list, Russian UI, SDL controller input paths, startup diagnostics, Windows-side accept validation, and a Range-enabled file-serving route.

**Not yet complete end-to-end:** the PS4 app does not yet poll for offers, send the accept request, download the PKG, save/resume it, or verify the receiver-side SHA-256. Installation is also not implemented. This remains a development prototype, not a finished game-transfer or installation product.

## Platform strategy

- Windows 10/11 x64 sender.
- PS4 homebrew receiver built with OpenOrbis.
- Discovery and native AMEGA transfer must work without relying on optional GoldHEN services.
- GoldHEN built-in Remote Package Install, standalone Remote Package Installer, FTP, internal `/data/pkg`, and experimental BinLoader are distinct capabilities/adapters.
- HTTP Range semantics are mandatory for any remote installer route that reads arbitrary package offsets.
- Installation is always separate from transfer and requires explicit confirmation.
- Current control/data transport is plaintext HTTP for a trusted home LAN only; production use requires authenticated encrypted transport.

## Main engineering documents

- `docs/ps4-platform-engineering-audit-2026-11.md` — firmware, exploit/HEN layers, OpenOrbis APIs, storage and installation research.
- `docs/architecture.md` — system boundaries and lifecycle.
- `docs/protocol-v1.md` — concrete discovery, accept, and Range route contract.
- `docs/security-model.md` — pairing and safe file handling.
- `docs/ps4-hen-compatibility.md` — firmware and installer compatibility.
- `docs/open-source-research.md` — reference projects and licensing boundaries.
- `docs/roadmap.md` — implementation stages and acceptance criteria.

## Release gate

Do not describe AMEGA as release-ready until Windows and PS4 artifacts have both been installed and tested in a real end-to-end scenario: discover, explicitly accept, transfer a large test file, interrupt/resume, verify SHA-256, and verify any separate installation result.
