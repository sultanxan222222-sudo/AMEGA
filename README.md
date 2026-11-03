# AMEGA

**AMEGA** is a two-part local-network transfer system for moving PS4 game package files (`.pkg`) from a Windows PC to a PS4 running a compatible HEN environment.

The product has two native applications:
- **AMEGA for Windows** — selects packages, discovers consoles, offers transfers, and reports progress.
- **AMEGA for PS4** — discovers Windows senders, asks the user to accept, receives and verifies packages, and exposes compatible installation actions.

AMEGA is intended to work over a trusted home Wi-Fi/LAN without a cloud account or internet connection.

## Project status — transfer implementation 0.3.0

The 0.3.0 source now implements the first complete transfer path:
- Windows selects a PKG, discovers PS4 receivers, and requires an explicit **Send** action for the selected console.
- The HTTP file endpoint is restricted to the IPv4 address of that selected console and supports HTTP byte ranges.
- PS4 displays the offer, requires the user to press **X**, downloads into /data/AMEGA/Received, keeps a .part file after interruptions, and resumes from the existing byte count.
- PS4 verifies the full file with SHA-256 before renaming it to its final filename. A failed digest is not presented as a completed package.

Builds: [Windows x64 build](https://github.com/sultanxan222222-sudo/AMEGA/actions/runs/41713040149) · [PS4 OpenOrbis build and transfer-helper tests](https://github.com/sultanxan222222-sudo/AMEGA/actions/runs/41713046772).

**Important:** successful CI builds prove that the Windows app and PS4 PKG compile; they do not prove a real transfer on the user's PS4 Pro. The build still needs a hardware test with the target GoldHEN/firmware combination before it can be called a finished release. The current transfer uses plain HTTP on a trusted home LAN and does not yet include encrypted pairing or automatic package installation.

## Design priorities

1. **User approval on both ends.** Nothing transfers or installs silently.
2. **Integrity first.** Every package is checked with SHA-256 before it is marked complete.
3. **Resume, do not restart.** Interrupted transfers should continue from verified byte ranges.
4. **HTTP Range correctness.** PS4 package-install workflows can request arbitrary byte ranges; a whole-file-only HTTP response is not sufficient.
5. **No destructive cleanup.** AMEGA never deletes the source package automatically.
6. **Separate transfer from installation.** Receiving a PKG and installing it are distinct actions.
7. **HEN compatibility is explicit.** HEN does not automatically mean a separate Remote Package Installer app is installed or running.
8. **Local-first.** Discovery and transfer stay on the local network.

## Repository map

- `docs/architecture.md` — system boundaries and transfer lifecycle.
- `docs/protocol-v1.md` — discovery and transfer protocol.
- `docs/security-model.md` — pairing, trust, and safe file handling.
- `docs/ps4-hen-compatibility.md` — HEN/GoldHEN integration constraints.
- `docs/open-source-research.md` — technical references and licensing boundaries.
- `docs/roadmap.md` — implementation stages and acceptance criteria.
- `docs/third-party-notices.md` — required notices.
- `protocol/schemas/` — machine-readable protocol schemas.
- `src/windows/` — Windows application.
- `src/ps4/` — PS4 homebrew application.

## Windows firewall setup

The Windows sender listens for PS4 discovery replies on UDP 38472 and hosts its local HTTP endpoint on TCP 38471. Windows can block inbound traffic even when PC-to-PS4 ping works. The Windows artifact includes `Allow-AMEGA-Firewall.ps1`, which creates program-scoped inbound rules for those two ports on **Private** network profiles only. Run it from the extracted folder containing `AMEGA.exe` and approve the administrator prompt. Use it only on a trusted home network, never on public or guest Wi-Fi.

## Current prototypes

**Windows:** selects a PKG, computes SHA-256, discovers PS4 receivers, exposes an explicit Send action, and serves the file through an HTTP endpoint with range processing. The endpoint only serves the armed transfer to the selected receiver's observed IPv4 address.

**PS4:** displays discovered PCs and the pending filename, accepts with Cross, saves data under /data/AMEGA/Received, resumes a .part file after a broken connection, and verifies SHA-256 before finalizing. Cross is an actual receive action, not just a selection action.

## Discovery hardening 0.2.0

The discovery design now follows proven LAN-discovery patterns from existing open-source senders: actual per-interface subnet masks, directed broadcast, bounded unicast fallback, and nonce correlation. See `docs/open-source-research.md` and `docs/protocol-v1.md`. CI success still does not prove a visible screen or real LAN discovery on the user's hardware. The 0.2.2 diagnostic build adds PS4 socket retry/IP diagnostics and a Windows firewall helper; real-hardware retesting is still required.

## Remaining release gates

1. Test a small harmless file and then a legally obtained PKG between the actual Windows PC and PS4 Pro on the same home LAN.
2. Test a deliberately interrupted transfer and confirm that pressing Cross resumes rather than restarts.
3. Confirm the final SHA-256 on the PS4 and inspect the saved file from a PS4 file manager.
4. Verify exact behavior on the target firmware/GoldHEN setup.
5. Add encrypted pairing, low-disk-space checks, explicit decline/cancel, and a documented installation adapter before calling AMEGA a hardened release.

## Compatibility

The initial target is Windows 10/11 x64 and a PS4 running a compatible HEN/GoldHEN setup. Exact firmware and GoldHEN capabilities must be detected and verified; AMEGA will not assume that every HEN version exposes the same services.

## Contributing

Read the architecture, protocol, security, and third-party notices before changing network behavior. Any protocol change must update its schema and test vectors together.
