# AMEGA development roadmap

A stage is complete only when its acceptance criteria pass. A source file or successful compile alone is not proof of real-console operation.

## Stage 0 — foundation
- [x] Create repository and document architecture.
- [x] Define first protocol and security requirements.
- [x] Separate transfer from installation.
- [x] Define versioned discovery, nonce and receiver identity.

## Stage 1 — Windows sender
- [x] Native Windows UI in Russian.
- [x] PKG file picker and background SHA-256.
- [x] LAN discovery beacon and nonce-validated reply listener.
- [x] Per-interface directed broadcast and bounded subnet unicast fallback.
- [x] Discovery remains available during background SHA-256 hashing.
- [x] Explicit Send PKG action; offer published only after choosing a discovered PS4.
- [x] Bind each outgoing offer to the selected receiver UUID; switching the target revokes the offer/token.
- [x] Receiver accept validation for fresh UUID/nonce, source IP and current PC nonce.
- [x] Cryptographically random 256-bit receiver-bound access token with expiry.
- [x] Streamed HTTP file endpoint with 206/416 Range handling, Content-Range and ETag.
- [x] Transfer ID derived from package SHA-256 so the same file keeps its resume key across PC restarts.
- [x] Local integration test for mid-transfer interruption and Range resume.
- [ ] Add negative tests for accept denial, expiry, wrong source IP, changed file/ETag and malformed Range requests.
- [ ] Multi-file transfer queue and parallel/serialized jobs.
- [ ] Authenticated encrypted transport and pairing code.

Acceptance: Windows can offer a selected PKG, authorize only the selected PS4 and serve byte ranges without loading the entire PKG into RAM. Local integration test passes; real-console network test remains open.

## Stage 2 — PS4 receiver
- [x] Native PS4 homebrew project and reproducible OpenOrbis build workflow (toolchain image pinned by digest).
- [x] Russian-first UI with discovered sender list.
- [x] Listen for Windows discovery probes and send an immediate nonce-bound unicast reply.
- [x] SDL GameController input, raw Joystick fallback and hot-plug handling.
- [x] Display local IPv4 address and attempt firmware detection (firmware API still requires real-hardware validation).
- [x] Request the selected PC offer and validate protocol, sender ID, transfer UUID, PKG type, size and SHA-256.
- [x] Explicit Accept action; send receiver UUID and per-device nonce to Windows.
- [x] Store short-lived access token in memory only.
- [x] Stream PKG data using HTTP Range and validate status, Content-Range and Content-Length.
- [x] Save partial file and resume metadata under /data/AMEGA/Download.
- [x] Resume from actual partial-file length after connection interruption or application restart when the same offer is accepted again.
- [x] Check available disk space before receiving and keep peer-supplied filenames out of filesystem paths.
- [x] Verify whole-file SHA-256 on the PS4 before final rename.
- [x] Show download progress, pause on Circle and resume on Cross.
- [x] Keep the PC's original PKG untouched.
- [ ] Install and exercise the package on a real PS4 Pro with the user's exact HEN/GoldHEN environment.
- [ ] Validate firmware readout, filesystem permissions, free-space reporting and controller behavior on real hardware.

Current partial acceptance: the PS4 receiver implements the code path from offer through verified staging. CI builds the PKG; a local mock-server test interrupts a 4,194,427-byte file after 600,000 bytes, resumes at that exact offset and verifies the final digest. Real PS4 hardware acceptance remains open.

## Stage 3 — installation adapter

- [x] Add a Standalone RPI API client for TCP 12800 with an application-level `POST /api/is_exists` health check.
- [x] Add explicit Windows confirmation and `POST /api/install` request for one selected, signature-checked PKG.
- [x] Host a temporary source URL restricted to the selected PS4 IPv4 address; support HTTP Range and ETag.
- [x] Count unique file-byte intervals actually read by the HTTP response stream so retries and overlapping ranges do not inflate the progress.
- [x] Handle malformed RPI error responses that include unquoted hexadecimal error codes; keep the source open after an ambiguous timeout because the task may already have been accepted.
- [x] Warn before closing AMEGA or revoking the source while RPI may still be downloading.
- [ ] Poll RPI task progress with backoff and distinguish RECEIVED / INSTALLING / INSTALLED / FAILED; RPI may not answer while its single-threaded download handler is busy.
- [ ] Detect GoldHEN built-in Remote Package Install independently from Standalone RPI and BinLoader.
- [ ] Add GoldHEN-specific adapter only after firmware/service-specific hardware validation.
- [ ] Test the API, HTTP Range transfer, and installation on a real PS4 Pro; verify completion on the console.

Current limitation: AMEGA can confirm that RPI accepted an install task (when it returns a task ID) and can show unique HTTP byte coverage served from the PC. It does not yet assert that the PS4 has completed installation. A fully read source file is not the same as an installed title.

Acceptance: installation is separately confirmed by the user; AMEGA reports INSTALLED only after an authoritative console task result or an explicit console-side check.

## Stage 4 — release hardening
- [ ] Pairing code shown on both devices with retry limits.
- [ ] Authenticated encrypted transport and certificate/key validation.
- [ ] Negative security, malformed-input and token-expiry tests.
- [ ] Multi-gigabyte, low-disk-space, app-restart and repeated interruption tests.
- [ ] Windows signing/release packaging strategy.
- [ ] Real PS4 Pro HEN/GoldHEN test report and release notes.

## Release rule

Do not publish AMEGA as release-ready until the Windows and PS4 artifacts have been installed and tested together on real hardware: discover, send, accept, transfer a large PKG, interrupt/resume, verify SHA-256, and separately validate installation.
