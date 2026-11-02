# AMEGA — PS4 platform engineering audit

**Research cut-off:** 2026-11-02  
**Purpose:** platform compatibility, runtime capability detection, networking, storage, controller input, display, and package-install integration.  
**Status:** engineering design and source review; this document is not proof of operation on every firmware or console revision.

## 1. What “PS4 compatibility” actually means

AMEGA must not treat “jailbreak works” as equivalent to “GoldHEN is installed” or “every GoldHEN service is running”. These are separate layers:

1. Retail PS4 firmware and hardware.
2. A firmware-specific entry point / exploit.
3. Kernel and sandbox patching.
4. A HEN payload (GoldHEN or another compatible HEN).
5. Optional HEN services, plugins, and independent homebrew applications.
6. AMEGA's own userland process, network, filesystem, SDL, and controller support.

The app must detect the capabilities it needs at run time. It must never infer a particular port, installer, or privilege from the mere fact that the AMEGA PKG launched.

Public Sony developer SDK material is not a complete public reference set. OpenOrbis is the public homebrew toolchain used here; its own README notes that headers and library stubs can be incomplete or need updating as functions are discovered. Treat headers as a build/research reference, not a guarantee that every symbol or behavior is identical on every retail firmware.

## 2. Firmware and HEN matrix as of 2026-11-02

The following is a research snapshot, not a promise that every combination has been tested by AMEGA.

| Firmware family | Publicly documented HEN status found | AMEGA policy |
|---|---|---|
| 5.05, 6.71/6.72, 9.00, 9.60, 10.00/10.01, 10.50, 10.70/10.71, 11.00 | Listed by the public GoldHEN v2.4b18 pre-release | Build and test separately; query capabilities at run time |
| 13.02, 13.04, 13.50 | SiSTRo's GoldHEN v2.4b18.12 is explicitly described as a beta adding these firmware versions | Treat as beta target; collect actual GoldHEN build and service status |
| 13.52 | Browser-exploit / HEN reports and community builds exist, but the official SiSTRo beta page checked for this audit lists 13.02, 13.04 and 13.50 only | Do not claim official GoldHEN support without a verifiable build and hardware result |
| 14.00 | BD-JB / HEN material is described as unstable; the consulted current guide says GoldHEN is not supported there | AMEGA core transfer must not require GoldHEN; do not enable GoldHEN-specific adapters on assumption alone |
| Above 14.00 | No compatible path was established by the sources reviewed for this audit | No compatibility claim |

Important distinctions:
- A kernel exploit may exist without a stable HEN payload.
- A HEN payload may load while optional services are disabled.
- A homebrew package may compile successfully and still fail at runtime on a particular firmware.
- “PS4 Pro” is not a firmware identifier. Record the exact model and system software separately.

## 3. Native PS4 interfaces worth using

OpenOrbis exposes headers for the following system interfaces. AMEGA should use documented / reversible interfaces where possible and keep each dependency optional.

### Network state: sceNetCtl / sceNet

OpenOrbis headers expose:
- `sceNetCtlInit` and `sceNetCtlGetInfo`.
- Network information selectors for device, SSID, RSSI, IP address, netmask, default route and DNS.
- `sceNet` socket, bind, send, receive, socket-option, and interface-query calls.

AMEGA's current PS4 receiver uses POSIX-compatible sockets and `getifaddrs` for interface broadcast data. This is acceptable for its current UDP discovery prototype. A later diagnostics panel should query network connection state and IP/netmask through `sceNetCtl` (after verifying required module initialization and linking on the target firmware), then compare the result with the POSIX interface view. Do not alter the console's network configuration.

### Controller: scePad as a native fallback

OpenOrbis exposes `scePadInit`, `scePadOpen`, `scePadRead`, `scePadReadState`, and controller information calls. The current UI uses SDL joystick events. Keep SDL as the first path because it already renders and works on the user's console; add `scePad` only as a tested fallback if a particular SDL mapping misses D-pad or button events. Do not switch input stacks without an A/B hardware test.

### Display and graphics

The current AMEGA UI uses SDL2 renderer creation with accelerated/software fallback. The user has reported that the newer interface is visible on their PS4, so SDL is currently the proven path on that particular device. OpenOrbis exposes video-output APIs, but a direct `sceVideoOut` path needs correct buffer allocation, registration, flip submission and cleanup. It is not a safe one-line fallback. Do not replace the working SDL path with raw video-output calls without a complete sample-based implementation and hardware testing.

### System modules

OpenOrbis exposes `sceSysmoduleIsLoaded`, `sceSysmoduleLoadModule` and `sceSysmoduleUnloadModule`. These can support diagnostics for known modules, but AMEGA must not load or unload unknown modules merely to probe the console. Any module dependency must be named, versioned and isolated in a capability adapter.

### App installation interfaces

Headers include AppInstUtil declarations such as title-ID parsing, package installation and progress queries. Their presence in a header does not prove that an ordinary homebrew process has the necessary authorization, module lifecycle, filesystem conditions, or safe access to those operations. The flatz reverse-engineering write-up documents a proof of concept where a direct AppInstUtil call returned success but the installed title later failed because of filesystem block mapping / package placement issues. Therefore:
- do not make direct AppInstUtil calls the default installer;
- do not report installation success merely because a function returned zero;
- first use a documented, independently running installer or GoldHEN's own package workflow;
- only add a native BGFT/AppInstUtil adapter after module, privilege, storage, progress and real-hardware behavior are proven on each target.

## 4. Existing GoldHEN capabilities and how AMEGA should use them

The public GoldHEN project lists:
- Remote Package Install feature;
- FTP server on TCP 2121;
- BinLoader on TCP 9090;
- Klog on TCP 3232;
- plugin support;
- internal package installation support from `/data/pkg`.

Do not confuse three separate mechanisms:

### A. GoldHEN Package Installer / internal PKG folder

GoldHEN Debug Settings > Package Installer can scan USB storage and internal `/data/pkg` (depending on the configured package source). A simple compatibility mode can place a fully received and verified PKG in an AMEGA-owned staging folder, then — only if the user chooses — copy it into `/data/pkg` for the GoldHEN Package Installer.

The app must check free space first and must not delete the source file automatically. The staging copy and the package installation may require substantial additional free space; do not assume that “free space >= PKG size” is sufficient.

### B. FTP on port 2121

GoldHEN can expose an FTP server on port 2121 when the relevant setting is enabled. This is a useful fallback for moving files to internal storage, but it is not the primary AMEGA design:
- it may be disabled;
- its availability and implementation vary by HEN build;
- it does not itself prove a package was installed;
- writing large files to internal storage creates space and filesystem-fragmentation concerns.

Probe FTP only as an optional, user-visible capability. Never require it for basic discovery or AMEGA file reception.

### C. GoldHEN BinLoader on port 9090

GoldHEN's own repository labels BinLoader experimental and warns that untrusted payloads may crash the console or cause harm. AMEGA must not use BinLoader for ordinary file transfer, must not auto-send payloads, and must not depend on it for discovery or PKG installation.

### D. Klog on port 3232

Klog is a diagnostic stream, not a transfer protocol. AMEGA may later offer an opt-in diagnostics screen that reads logs only when the user explicitly enables it. It is not needed in normal operation.

### E. GoldHEN Remote Package Install vs. standalone Remote Package Installer

The GoldHEN README lists a Remote Package Install feature. Separately, the `KonixDev/ps4-pkg-installer` project documents an API for the independent PS4 Remote Package Installer homebrew on TCP port 12800, including `/api/is_exists` and `/api/install`. Do not assume that the standalone app API and GoldHEN's built-in feature are the same service.

For the standalone RPI adapter:
- verify the actual application/API with a real request, not just an open TCP port;
- require that the PS4 RPI app is running in the foreground, because its network service may stop when suspended;
- send a URL only after the user explicitly chooses Install;
- display the installer task/progress state rather than assuming success;
- implement HTTP Range correctly on the Windows host (`206 Partial Content`, `Content-Range`, `416` on unsatisfiable ranges).

The modern PS4 PKG Installer documentation explains that some PKG readers request metadata at arbitrary offsets. A generic whole-file HTTP server can return the wrong bytes while still returning HTTP 200; this may look like a firmware or corrupted-file problem. Range correctness is a hard release requirement.

## 5. Recommended AMEGA architecture

### Layer 1 — independent discovery

- Keep the versioned AMEGA UDP discovery protocol (current development defaults: PC announcements UDP 38470; PS4 replies to PC UDP 38472).
- Use directed broadcast per actual adapter subnet and a bounded unicast fallback.
- Correlate reply nonces, validate protocol version and device IDs, deduplicate by persistent receiver/sender ID.
- Discovery must work before a PKG is selected or hashed.
- Do not use a GoldHEN service port as the device-discovery protocol.

### Layer 2 — pair and authenticate

- Show the discovered PC/PS4, IP address, protocol version and reachability.
- Ask for an explicit one-time pairing confirmation on both sides.
- Use a short-lived session token and authenticated transport for control/data.
- Discovery is not authentication and must not reveal file paths or grant file access.
- Never expose an unrestricted file server to the local network.

### Layer 3 — native AMEGA file reception

- Keep normal PKG transfer independent of GoldHEN FTP, BinLoader or RPI.
- Windows hosts the chosen file; PS4 receives it into a dedicated staging directory.
- Use bounded streaming chunks and HTTP Range with strict `Content-Range` validation.
- Persist transfer ID, byte offset, total length and expected SHA-256 so a failed transfer can resume.
- Use receiver-side SHA-256 verification; only a matching file becomes `VERIFIED`.
- Keep a verified PKG until the user makes a separate deletion decision.

### Layer 4 — optional install adapters

Capability order:
1. GoldHEN built-in package workflow, when positively detected and documented.
2. Standalone RPI API adapter, when the actual RPI app is running and passes an API-level health check.
3. Optional FTP staging to `/data/pkg`, when enabled and enough storage is available.
4. Native BGFT/AppInstUtil adapter only after a complete independent research and hardware test.

The user must always approve installation. A received file, a verified file, a queued install, an active install, and a completed install are different states in the UI.

## 6. UI / large-file design

A PKG can be many gigabytes. Device discovery must not wait for a full-file SHA-256 pass. The Windows UI should:
- allow “Find PS4” before choosing a file;
- keep discovery active while hashing runs in the background;
- show hash percent and cancellation clearly;
- allow choosing another file without deadlocking the UI;
- offer a clear stop/cancel action with a visually distinct active style;
- update the offer metadata only after hashing completes;
- never make every button appear disabled during background work.

The receiver must show a persistent device list with last-seen state, IP address, and status. Controller focus must be visible and tested for both SDL and native `scePad` paths.

## 7. Validation matrix required before release

For each physical console test, record:
- model (CUH generation and Pro/Slim/base);
- exact system software version;
- HEN name and exact version / build;
- whether GoldHEN services are enabled;
- whether standalone RPI is installed and in foreground;
- Ethernet or Wi-Fi and router/client-isolation conditions;
- local IP, subnet and gateway (do not collect MAC or account identifiers);
- app launch/exit, controller navigation, discovery both directions;
- small test transfer, large PKG transfer, interruption/resume, SHA-256;
- free-space check and install result, if installation is tested;
- logs and exact error codes.

A successful CI build is not a hardware pass. Do not state universal compatibility without a test record for each claimed firmware range.

## 8. References reviewed

- GoldHEN project and current public release list: https://github.com/GoldHEN/GoldHEN
- SiSTRo GoldHEN v2.4b18.12 beta announcement: https://ko-fi.com/s/85ea22bd2f
- ConsoleMods PS4 firmware / exploit status: https://consolemods.org/wiki/Ps4_getting_started
- ConsoleMods BD-JB status: https://consolemods.org/wiki/Bd-jb
- ConsoleMods GoldHEN features and Package Installer: https://consolemods.org/wiki/PS4:GoldHEN
- ConsoleMods FTP notes: https://consolemods.org/wiki/PS4:Transferring_Files_with_FTP
- OpenOrbis toolchain and headers: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- OpenOrbis NetCtl header: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/NetCtl.h
- OpenOrbis Net header: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/Net.h
- OpenOrbis Pad header: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/Pad.h
- OpenOrbis AppInstUtil header: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/AppInstUtil.h
- KonixDev standalone PS4 PKG Installer and Range/API notes: https://github.com/KonixDev/ps4-pkg-installer
- flatz PKG installation research and limitations: https://flatz.github.io/

