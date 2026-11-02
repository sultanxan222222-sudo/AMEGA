# AMEGA

**AMEGA** is a two-part local-network system for sending PlayStation 4 package files (.pkg) from a Windows PC to a PS4 homebrew receiver.

## Development status — engineering branch 0.2.2

Branch: `fix/0.2.2-hash-ui-ps4-capabilities`.

The current development build implements the complete native PC-to-PS4 file-transfer path:

1. Windows selects a PKG and calculates its whole-file SHA-256.
2. Windows discovers PS4 receivers over the local network.
3. The user selects one console and explicitly presses **Send PKG**.
4. PS4 requests the offer, displays filename/size/hash, and the user presses Cross to accept.
5. Windows issues a short-lived receiver-bound access token.
6. PS4 downloads the file through HTTP byte ranges into an app-owned staging folder.
7. If the link drops, the partial file is preserved. Pressing Cross again resumes from the actual partial-file length.
8. PS4 hashes the completed file independently. Only a matching SHA-256 is renamed to its final `.pkg` file.

## Implemented in the prototype

**Windows:** package picker, background SHA-256, LAN discovery, explicit target selection, receiver-bound offer, access-token gate, streamed HTTP file route with Range and ETag support.

**PS4:** Russian UI, DualShock navigation, discovered-PC list, offer preview, explicit Accept, transfer progress, safe staging, resume after interruption/restart, receiver-side SHA-256 verification, and a final saved-file screen.

The receiver writes temporary data under `/data/AMEGA/Download/<transfer_id>.part`, with matching resume metadata in a `.resume` file. After the full digest matches, the file becomes `/data/AMEGA/Download/<transfer_id>.pkg`. The token remains in memory and is never written to disk.

The transfer ID is derived from the PKG's SHA-256, so the same package can resume after the PC application is restarted and offers the same content again. A fresh access token is still required.

## What is not yet complete

- No automatic or built-in package installation action is implemented. Transfer and installation remain separate.
- No RPI / GoldHEN / FTP installer adapter is enabled yet.
- Pairing code and authenticated encrypted transport are still required before calling the protocol secure on an untrusted network. Current HTTP is plaintext and intended only for a trusted private LAN.
- Real-hardware testing on the user's PS4 Pro is still required. CI success and local mock-server tests do not prove the app works on every firmware, HEN build, router, or PS4 model.

## Automated validation

The PS4 CI workflow currently runs:
- SHA-256 known-answer tests (empty input, `abc`, and one million `a` bytes);
- a local HTTP integration test that deliberately interrupts a 4,194,427-byte transfer after 600,000 bytes, verifies that the receiver resumes from that exact offset, and verifies the final SHA-256;
- the OpenOrbis PS4 build and PKG packaging.

These tests exercise the portable transfer module on the CI host; the PS4-specific package still needs installation and a real console test.

## Platform strategy

- Windows 10/11 x64 sender.
- PS4 receiver built with OpenOrbis and SDL2.
- Core native discovery and file transfer do not require optional GoldHEN FTP, BinLoader, or Remote Package Install services.
- GoldHEN built-in Remote Package Install, standalone Remote Package Installer, FTP, internal `/data/pkg`, and experimental BinLoader are separate optional capabilities.
- Installer adapters will be added only after application-level health checks and hardware validation.
- Installation must always be a separate user-approved action.

## Engineering documents

- `docs/ps4-platform-engineering-audit-2026-11.md` — firmware, exploit/HEN layers, OpenOrbis APIs, storage and installation research.
- `docs/architecture.md` — system boundaries and lifecycle.
- `docs/protocol-v1.md` — discovery, authorization, Range download and resume contract.
- `docs/security-model.md` — access control and release security requirements.
- `docs/ps4-hen-compatibility.md` — firmware and installer compatibility.
- `docs/open-source-research.md` — reference projects and licensing boundaries.
- `docs/roadmap.md` — implementation stages and hardware acceptance criteria.

## Release gate

Do not call AMEGA release-ready until both apps have been installed and tested together on a real PS4: discover, select, accept, transfer a large PKG, interrupt/resume, verify SHA-256, then separately validate any future installation adapter.
