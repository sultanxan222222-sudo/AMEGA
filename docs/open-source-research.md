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
