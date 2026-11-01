# AMEGA development roadmap

A stage is complete only when its acceptance criteria pass. A source file or successful compile alone is not proof of end-to-end functionality.

## Stage 0 — foundation
- [x] Create repository and document architecture.
- [x] Define first protocol and security requirements.
- [x] Separate transfer from installation.
- [ ] Add shared protocol implementation and automated tests.

## Stage 1 — Windows sender
- [ ] Native Windows UI in Russian.
- [ ] PKG file picker and metadata preview.
- [ ] LAN discovery beacon.
- [ ] Pairing code and receiver authorization.
- [ ] HTTP byte-range server and transfer status.
- [ ] Queue, cancel, pause, and resume.
- [ ] Build artifact produced by CI.

Acceptance: a second computer on the same LAN can discover the sender, pair, download a test file, resume it, and verify SHA-256.

## Stage 2 — PS4 receiver
- [ ] PS4 homebrew project and reproducible build instructions.
- [ ] Russian UI with visible sender list.
- [ ] Explicit Accept / Decline actions.
- [ ] Safe staging storage and free-space check.
- [ ] Resumable receive and SHA-256 verification.
- [ ] Build and installable PKG validated on real hardware.

Acceptance: PS4 discovers Windows sender, accepts a test file, resumes after disconnect, and verifies the exact expected digest.

## Stage 3 — installation adapter
- [ ] Detect supported GoldHEN/HEN installation capabilities.
- [ ] Add adapter for a documented mechanism.
- [ ] Show exact installation state and useful errors.
- [ ] Verify behavior with a known safe test package.

Acceptance: installation is initiated only after explicit confirmation and success is reported only when the console mechanism confirms it.

## Stage 4 — release hardening
- [ ] Authenticated encrypted transport.
- [ ] Negative security and malformed-input tests.
- [ ] Large-file and low-disk-space tests.
- [ ] Wi-Fi loss, app restart, and duplicate-transfer tests.
- [ ] Windows signing/release packaging strategy.
- [ ] Real PS4 Pro HEN test report and release notes.

## Release rule

Do not publish a release as ready until the Windows and PS4 artifacts have both been built, installed, and tested in the same end-to-end scenario.
