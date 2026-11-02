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

An optional Windows-side **Install through RPI** path is also present in the development branch. It checks the real Standalone Remote Package Installer API on the selected PS4, requires a second explicit confirmation, and gives RPI a temporary HTTP Range URL for the selected PKG. AMEGA shows the unique byte coverage served to RPI; 100% means the source file has been completely read by HTTP requests, **not** that PS4 installation has finished.

## Implemented in the prototype

**Windows:** package picker, background SHA-256, LAN discovery, explicit target selection, receiver-bound offer, access-token gate, streamed HTTP file route with Range and ETag support.

**PS4:** Russian UI, DualShock navigation, discovered-PC list, offer preview, explicit Accept, transfer progress, safe staging, resume after interruption/restart, receiver-side SHA-256 verification, and a final saved-file screen.

The receiver writes temporary data under `/data/AMEGA/Download/<transfer_id>.part`, with matching resume metadata in a `.resume` file. After the full digest matches, the file becomes `/data/AMEGA/Download/<transfer_id>.pkg`. The token remains in memory and is never written to disk.

The transfer ID is derived from the PKG's SHA-256, so the same package can resume after the PC application is restarted and offers the same content again. A fresh access token is still required.

## What is not yet complete

- A native AMEGA/GoldHEN installer is not implemented. The optional installer path uses the separate Standalone Remote Package Installer app (RPI) on TCP 12800; RPI must be installed and kept open in the PS4 foreground.
- When RPI returns a task ID, AMEGA polls `/api/get_task_progress` for console-reported transfer and local-copy progress. If RPI is busy and stops answering during the download, AMEGA falls back to unique HTTP byte coverage and retries. It reports completion only after RPI reports all bytes transferred and `local_copy_percent` at 100%; this remains an API report and should be checked on the console.
- GoldHEN's separate built-in Remote Package Install, FTP and BinLoader adapters are not enabled. HEN alone does not imply that Standalone RPI is installed or running.
- Pairing code and authenticated encrypted transport are still required before calling the protocol secure on an untrusted network. Both native transfer and temporary RPI file hosting use plaintext HTTP on a trusted private LAN.
- Real-hardware testing on the user's PS4 Pro is still required. CI success and local mock-server tests do not prove the apps work on the console, on every firmware, or through every router.

## Optional Standalone RPI installation

1. On PS4, install the separate Remote Package Installer homebrew and leave it open in the foreground. Its documented API listens on TCP 12800.
2. In AMEGA for Windows, select the discovered console and press **Проверить RPI**.
3. Select a PKG and press **Установить через RPI**. Review the target and filename and confirm.
4. AMEGA temporarily shares only that selected PKG from the PC. RPI requests the file through HTTP Range. The Windows UI shows unique byte coverage served.
5. Keep AMEGA open until RPI has finished downloading. At 100% byte coverage, the file has been read by the HTTP server, but the console may still be unpacking/installing it. Verify the final state on the PS4 before closing or removing the temporary source.

If the API times out after an install request, AMEGA keeps the source open because the task may already have been accepted. Do not send the same package again until you have checked the PS4.

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
- Standalone RPI is an optional adapter: API readiness is checked with `POST /api/is_exists`, and installation is requested with `POST /api/install` only after confirmation.
- The temporary RPI source is restricted to the selected console's IP, serves HTTP Range/ETag, and expires after 12 hours. Keep AMEGA open while RPI downloads.
- GoldHEN built-in RPI, FTP and BinLoader remain separate adapter work; no such service is assumed from HEN detection.
- Installation must always be a separate user-approved action.

## Engineering documents

- `docs/ps4-platform-engineering-audit-2026-11.md` — firmware, exploit/HEN layers, OpenOrbis APIs, storage and installation research.
- `docs/architecture.md` — system boundaries and lifecycle.
- `docs/protocol-v1.md` — discovery, authorization, Range download and resume contract.
- `docs/security-model.md` — access control and release security requirements.
- `docs/ps4-hen-compatibility.md` — firmware and installer compatibility.
- `docs/open-source-research.md` — reference projects and licensing boundaries.
- `docs/roadmap.md` — implementation stages and hardware acceptance criteria.
- `docs/rpi-installation-adapter.md` — Standalone RPI setup, API behavior and safety limits.

## Release gate

Do not call AMEGA release-ready until both apps have been installed and tested together on a real PS4: discover, select, accept, transfer a large PKG, interrupt/resume, verify SHA-256, then separately validate any future installation adapter.
