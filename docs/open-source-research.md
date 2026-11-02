# Open-source research and implementation choices

This document records useful existing work and the legal/technical boundaries for AMEGA. These projects are references and possible compatibility targets; AMEGA does not copy their source code into this repository.

## Windows-side reference: PS4 PKG Installer

Repository: https://github.com/KonixDev/ps4-pkg-installer  
License: MIT.

Useful findings:
- A PS4 Remote Package Installer (RPI) homebrew can accept a package URL from a PC-side sender.
- The documented RPI API listens on TCP port 12800 and exposes `/api/is_exists` and `/api/install`; the latter accepts a direct package URL.
- The RPI application must be running in the foreground for that API workflow.
- The PC must serve correct HTTP byte ranges. The PS4 package parser asks for data at arbitrary offsets; a server that ignores `Range` and returns the whole file can cause package parsing to fail.

AMEGA consequence: HTTP Range support is a release requirement, not an optional performance feature. RPI compatibility will be an optional adapter because having HEN installed does not prove that the separate RPI homebrew is installed or currently running.

## PS4 homebrew reference: ezRemote Client / Server

Repositories:
- https://github.com/cy33hc/ps4-ezremote-client
- https://github.com/cy33hc/ps4-ezremote-server

The client supports multiple remote file protocols and a PS4-side background helper. Its documentation says the helper is launched through GoldHEN BIN Loader and handles background downloads/installs. The client repository carries GPL-2.0. AMEGA will not copy or derive code from it unless the project explicitly adopts a compatible licensing strategy and preserves all required notices.

AMEGA consequence: this is evidence that a PS4-side network helper is feasible, but the BIN Loader requirement is an additional capability, not a synonym for HEN itself.

## PS4 SDK reference: OpenOrbis

Repository: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain

OpenOrbis provides an open homebrew toolchain with PS4 headers, library stubs, build tools, and samples. Its repository carries GPL-3.0. It will be treated as a separately installed build toolchain; AMEGA will not vendor it into the application package.

## OOSDK Remote Package Installer port

Repository: https://github.com/Backporter/ps4_remote_pkg_installer-OOSDK

The repository describes itself as an OOSDK port of flatz's Remote Package Installer and credits the original authors. No LICENSE file was present at the time of review. AMEGA will not copy or incorporate its source unless licensing permission is established. It may be used as a technical reference while designing an independently implemented adapter.

## Integration strategy

AMEGA will keep three layers separate:

1. **Native AMEGA transfer:** custom PC and PS4 applications use the versioned AMEGA protocol, explicit Accept/Decline, resumable transfer, and SHA-256 verification.
2. **Optional existing-RPI adapter:** when a compatible Remote Package Installer is present and responsive, a PC-side compatibility path may send a URL to its documented API. It must clearly state that the separate RPI app must remain in the foreground.
3. **GoldHEN-specific installation adapter:** capabilities must be detected and tested. HEN presence alone is not proof of a particular API, payload, or port.

No installation adapter may report success based only on opening a TCP connection or sending a request. It must use the platform's actual task/result state.


## LAN discovery reference: PKG Sender / LoopDPI

Repository: https://github.com/Loopayeh/pkg-sender  
License: MIT.

The public project demonstrates practical discovery lessons:
- enumerate active IPv4 adapters and use their actual subnet masks instead of assuming every network is /24;
- send discovery beacons first, then use a bounded subnet sweep when broadcasts are not heard;
- merge and de-duplicate device responses;
- distinguish an open TCP port from an application that actually answers its protocol.

AMEGA adopts these architectural lessons in an independent protocol implementation: per-adapter directed broadcasts, a bounded unicast UDP sweep, and a nonce-correlated PS4 reply on a separate port. AMEGA does not copy LoopDPI source code or use its receiver protocol. Its PS4/PS5 service ports and APIs are a different compatibility target.

The original flatz Remote Package Installer documentation confirms that RPI is a separate service on TCP 12800 and that installation is an HTTP POST carrying a package URL. AMEGA discovery does not infer that RPI is installed merely because a PS4 responds to AMEGA UDP; RPI/GoldHEN installation capability remains a separate integration stage.


## Updated remote-install research (2026-11-02)

The current `KonixDev/ps4-pkg-installer` README contains important implementation details beyond the initial API overview:

- It verifies PS4 packages by their package signature before queueing them, rather than trusting the filename extension.
- Its RPI readiness check calls the actual `POST /api/is_exists` endpoint. A TCP handshake alone can falsely report ready if the RPI's single-threaded event loop has become stuck.
- It tracks installation by `task_id` and polls `/api/get_task_progress`, `/api/pause_task`, `/api/resume_task`, and `/api/stop_task`.
- The RPI can temporarily stop answering progress polls while it is doing a long download; the PC-side server's observed HTTP Range requests can provide an independent transfer-progress signal. Silence must not automatically be interpreted as a failed download.
- It documents specific RPI failure codes, including insufficient free space and an existing install task. AMEGA should preserve real error codes instead of converting every failure to a generic message.

The same research explains why HTTP Range handling is a protocol requirement: the PS4 package reader seeks metadata at non-zero offsets. The server must return the requested byte interval with `206 Partial Content`, a correct `Content-Range`, and `416 Range Not Satisfiable` for an invalid interval. A generic web server that returns the whole file with `200 OK` can make a valid package appear corrupted.

## Additional installation approaches

- **DirectPackageInstaller** is a separate homebrew approach that uses GoldHEN BinLoader rather than the standalone RPI application. ConsoleMods notes the distinction; GoldHEN itself marks BinLoader experimental and warns of possible crashes. AMEGA will not make this the default path.
- **BGFT / AppInstUtil** native-install research remains exploratory. Headers and PoCs do not prove a safe retail-userland installation lifecycle. The flatz write-up records a case where a direct AppInstUtil call returned success but the installed title later failed due to filesystem block mapping. AMEGA must rely on a verified task/result state and a real hardware test before enabling any such adapter.
- **OrbisShelf** is a newer project worth monitoring for its BGFT/AppInstUtil workflow, but its public description/build state is not a substitute for an AMEGA end-to-end test. It remains a research reference only.

## Engineering decision

For AMEGA, the least fragile path is:
1. Build and test our own discovery + authenticated resumable transfer independently of installer services.
2. Add an RPI adapter that checks the actual API and implements correct Range.
3. Add GoldHEN / FTP / Package Installer as explicitly detected options.
4. Treat BinLoader and direct native AppInstUtil as optional research-only paths until proven stable.
