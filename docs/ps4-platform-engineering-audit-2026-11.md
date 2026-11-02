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



## 9. Research addendum — later 2026 findings and corrections

### Firmware status: do not collapse exploit, HEN and GoldHEN into one table

Newly checked sources show that the older simplified 13.52 / 14.00 statements require finer distinctions:

- The public upstream GoldHEN release page for v2.4b18 still lists stable/pre-release compatibility only through 11.00. SiSTRo's own v2.4b18.12 beta announcement adds 13.02, 13.04 and 13.50.
- A community repack/fork named `EEJTek/GoldHEN_v2.4b18.12` lists 13.52, and `ps4aio` specifically selects GoldHEN 2.4b18.11 for 13.52. These are build/repack-specific claims, not proof that the upstream v2.4b18.12 release is universally interchangeable on 13.52.
- The current Scene-Collective `ps4-hen` repository has commits titled "Add 13.52 support" (2026-06-25) and "Add 14.00 support" (2026-09-19), with `MAX_FW 1400` in its version header. However, its README still describes PS4HEN v2.2.0 BETA as supporting 5.05–12.02. The maximum version macro and a source commit do not by themselves establish a stable end-user release or verified operation on all hardware.
- ConsoleMods currently describes 14.00 as exploitable through an unstable BD-JB route, with an open-source HEN payload offering fewer features and no GoldHEN support. HenJB's browser-hosted status page currently marks 14.00 unavailable. These community pages are not perfectly synchronized; AMEGA therefore treats 14.00 as experimental/unverified, not a supported installation target.
- On 13.02, SiSTRo's v2.4b18.12 beta announcement explicitly lists support. Community PS4HEN paths also exist. For this user-facing build, AMEGA must identify what service is actually reachable and must not depend on GoldHEN-only FTP/BinLoader/RPI features to perform basic discovery.

**Corrected AMEGA policy:** firmware information is diagnostic only. Probe the actual receiver and installer APIs. The UI must say "HEN detected / service detected" only when a real health request or known receiver handshake succeeds. Never label a console simply "GoldHEN-compatible" from its IP address or firmware number.

### Official Sony public-source material

Sony maintains an official PS4 Open Source Software page that publishes WebKit source snapshots for multiple historical system software versions, extending through the 13.00 family in the current public index. This is useful for understanding the publicly released browser component and license obligations. It is **not** a public release of Sony's complete PS4 SDK, full system source, private game-installation APIs, or a guarantee that a reverse-engineered homebrew declaration is supported.

### Firmware self-identification for AMEGA

OpenOrbis' public `libkernel.h` declares `sceKernelGetSystemSwVersion(OrbisKernelSwVersion*)`. This gives AMEGA a candidate native firmware-identification route without scraping Settings UI or asking the user to type a version. Before enabling it:
- verify the exact `OrbisKernelSwVersion` layout from the SDK header version used by the build;
- confirm the symbol is resolved on the target firmware at runtime;
- compare its result against the console's System Information screen in a hardware test;
- if unavailable, show "firmware not detected" rather than guessing.

The current 0.2.2 receiver only displays the local IPv4 address; this native firmware readout is researched but not yet implemented.

### PKG header digest and installer manifest

PS4 PKG format references document a 0x1000-byte package header and a 32-byte `pkg_digest` field at offset `0xFE0`. This is distinct from SHA-256 over the complete file. The newer PKG Sender implementation uses the header digest in the GoldHEN/DPI manifest and reports BGFT error `0x80990033` when the wrong manifest/metadata shape or content type is used.

Consequences for AMEGA:
- Keep **whole-file SHA-256** for corruption detection and resumable-transfer verification.
- Parse and validate the PKG header separately for package type/content ID/size and, if an installer protocol requires it, the exact package-header digest.
- Do not substitute a whole-file SHA-256 for the PKG header digest in a BGFT/DPI manifest.
- Validate both before offering installation; never trust the filename extension alone.

### New PS4 sender implementation observations

The maintained `Loopayeh/pkg-sender` v1.2.9 source and release notes provide a useful current engineering reference:
- PS4 mode detection tries RPI's application-level HTTP API at TCP 12800 first, then GoldHEN Payload Server's HTTP status route at 9090, then tests the alternate payload ports 9021 and 9020.
- Its GoldHEN path builds the DPI/BGFT manifest, derives the PKG header digest, prepares one callback listener, injects exactly once after a successful payload send, and avoids a second injection because that could queue a duplicate install.
- It uses multi-range HTTP pieces, buffers, and a queue with resume/pause/cancel.
- The project reports its own measured numbers (~75 MB/s RPI and ~50 MB/s GoldHEN on one gigabit setup). Those are author-reported measurements on their setup, not expected speeds for all PS4 models, disks, routers or firmware.
- Its root license is MIT. The repository can be used as a source of ideas and, if ever copying code, requires license notice preservation and a file-by-file dependency review. AMEGA currently uses it as a research reference only.

For AMEGA this strengthens the recommended path:
1. RPI API adapter (TCP 12800, API-level health check) as the preferred optional install mode when RPI is actually running.
2. GoldHEN Payload Server/BGFT adapter only as an explicitly selected advanced route. GoldHEN's own warning that BinLoader is experimental remains decisive; no auto-injection on discovery.
3. Native AMEGA transfer as the independent core function.
4. FTP /data/pkg as optional manual staging, not a requirement.
5. No direct AppInstUtil/BGFT native installer unless independently proven on device.

### Discovery and LAN caveats

- A UDP beacon should be treated as a hint, not authentication or proof that a device is ready to install.
- The current PKG Sender uses UDP 12801 for PS4 receiver beacons and 12802 for PC catalog beacons; our AMEGA uses its own versioned ports 38470/38472. Do not mix protocol magic or assume services from ports alone.
- For networks with broadcast suppression, a directed broadcast per real IPv4 subnet and a bounded TCP fallback can find listening RPI/receiver services. A large-scope sweep must be concurrency-limited and visibly report skipped/truncated ranges.
- A direct PC-to-PS4 Ethernet cable without a DHCP server requires static addresses on both endpoints; automatic 169.254/16 link-local addressing is unreliable for these setup instructions. This is a fallback setup, not the default user experience.

### Official-source and scene-reference index added

- Sony PS4 open-source software (WebKit snapshots): https://www.playstation.com/en-us/oss/ps4/webkit/
- PS4HEN current source and commit history: https://github.com/Scene-Collective/ps4-hen
- SiSTRo GoldHEN upstream releases: https://github.com/GoldHEN/GoldHEN/releases
- SiSTRo beta release notes: https://ko-fi.com/s/85ea22bd2f
- Community 13.52 GoldHEN package index: https://github.com/eddieygaming/ps4aio
- Community 13.52 / 13.50 exploit validation: https://github.com/caziques/PS4
- Firmware chain status: https://henjb.com/
- Current LAN PKG sender research: https://github.com/Loopayeh/pkg-sender
- Original RPI API reference: https://github.com/flatz/ps4_remote_pkg_installer
- PS4 PKG header layout: https://www.psdevwiki.com/ps4/PKG_files



## 10. Complete firmware-range picture and late-2026 correction

Exploit capability and HEN availability must be recorded separately. The current ConsoleMods exploit chart (last edited 2026-10-31) groups the public landscape as follows:

| Firmware family | Public status summary |
|---|---|
| Below 4.50 | Historical exploits; limited homebrew |
| 4.50–5.05/5.07 | Older stable homebrew path |
| 5.50–6.51 | Exploit path exists but chart describes it as unstable |
| 6.70–6.72 | Mature/optimized path |
| 7.00–7.55 and 8.00–8.52 | Several WebKit / kernel-exploit families |
| 9.00 | Multiple established routes |
| 9.03–9.51 and 9.60 | Public browser / kernel paths |
| 10.00–11.00 | Public paths; 11.00 is the last PPPwn firmware |
| 11.02 | Separate kernel-exploit family |
| 11.50–12.02 | Newer WebKit / BD-JB paths; payload support differs |
| 12.50–13.00 | Exploit availability exists, but kernel/HEN compatibility is more limited and varies by exact point release |
| 13.02 / 13.04 | Public browser chain; SiSTRo GoldHEN v2.4b18.12 beta explicitly adds both |
| 13.50 | Public browser chain; same SiSTRo beta explicitly adds 13.50 |
| 13.52 | Independent hardware report documents GoldHEN v2.4b18.11 on 2026-09-21 |
| 14.00 | BD-JB is described as unstable; no GoldHEN support is established in the current getting-started guide |
| Above 14.00 | No public userland + kernel path established by the chart reviewed |

GoldHEN version distinctions:
- Upstream GoldHEN v2.4b18 is a prerelease whose own README lists 5.05, 6.71/6.72, 9.00, 9.60, 10.00/10.01, 10.50, 10.70/10.71 and 11.00.
- SiSTRo's separate v2.4b18.12 beta notice explicitly adds 13.02, 13.04 and 13.50.
- The caziques/PS4 community test repository documents GoldHEN v2.4b18.11 for 13.52 and labels its test as hardware-verified. This is a distinct build and must not be silently substituted for the 13.02/13.04/13.50 beta.
- Scene-Collective ps4-hen source has development commits for 13.52 and 14.00, but its README's stated release range is narrower. A source commit or MAX_FW constant is not equivalent to a stable end-user payload.

**AMEGA policy:** no static firmware-to-feature promise. Read the firmware only as diagnostics where the API is verified; detect HEN/installer services with real application-level probes; keep native discovery and future transfer independent of those optional services.

### Runtime firmware API research

The current OpenOrbis public `libkernel.h` declares `sceKernelGetSystemSwVersion` and the matching `OrbisKernelSwVersion` structure. However, the OpenOrbis Docker image used by the AMEGA CI build did not expose that type consistently, so the call has deliberately been left out of the current receiver build. It remains a candidate for a later compatibility adapter after a reproducible SDK/toolchain version is pinned and checked. The current app displays the detected local IPv4 address instead.

### Controller update in AMEGA 0.2.2

The receiver now initializes SDL GameController as well as SDL Joystick, prefers standardized Cross/Circle/D-pad events, retains raw Joystick fallback, and reacts to controller add/remove events. The UI displays connected/not-found status. CI compilation has passed for this controller change; USB and Bluetooth DS4 behavior still requires physical-console validation.

### Current source references

- ConsoleMods exploit chart, last edited 2026-10-31: https://consolemods.org/wiki/PS4:Exploit_Chart
- ConsoleMods getting started: https://consolemods.org/wiki/Ps4_getting_started
- GoldHEN upstream README / features and warning: https://github.com/GoldHEN/GoldHEN
- GoldHEN upstream v2.4b18 release: https://github.com/GoldHEN/GoldHEN/releases/tag/2.4b18
- SiSTRo v2.4b18.12 beta note: https://ko-fi.com/s/85ea22bd2f
- 13.52 community hardware test: https://github.com/caziques/PS4
- OpenOrbis libkernel header: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/libkernel.h
- OpenOrbis kernel types: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/kernel.h



## 11. New 14.00 BD-J research (2026-10-30) — experimental, not a compatibility promise

PS4 Dev Wiki records a new item dated 2026-10-30 titled BD-JB-14.00, with a compiled ISO linked from the page. The record is explicitly preliminary:
- the suggested vulnerable Java method/check is still marked as requiring verification against the actual PS4 BD-J ISO;
- a kernel-stage vulnerability is described as undocumented, with an estimated success rate around 10%;
- the linked community ISO repository itself labels its code as stolen and warns that the Hx stage may write unknown kernel objects even on a failed run;
- that repository warns that a reported success must be independently verified and describes cold reboot after a failed attempt.

This is a scene-development signal, not a stable supported firmware release. ConsoleMods' currently published exploit chart and getting-started guide still have inconsistent/older summaries for 14.00; the getting-started guide says the BD-J route is unstable and GoldHEN is not available there. AMEGA must therefore label this as **experimental / unverified**, not "GoldHEN supported".

**Engineering decision:** AMEGA will not ship, embed, invoke or depend on this ISO, its classes, leaked code, or any experimental kernel payload. It is unnecessary for the user's current HEN setup and unrelated to the core PC↔PS4 transfer protocol. The only legitimate long-term implication is that a future receiver may run under additional HEN variants; it still has to use the same capability-based discovery and transfer layers. We do not need to exploit firmware to discover a console that already runs AMEGA.

References:
- PS4 Dev Wiki vulnerability record, BD-JB-14.00 added 2026-10-30: https://www.psdevwiki.com/ps4/Vulnerabilities
- Public repository described by that record (research only; it explicitly claims the code is stolen and carries safety warnings): https://github.com/OptiTronOffical/PS4-14.00-BD-JB
- ConsoleMods current exploit chart: https://consolemods.org/wiki/PS4:Exploit_Chart
- ConsoleMods getting-started firmware notes: https://consolemods.org/wiki/Ps4_getting_started

## 12. Reproducible toolchain and controller ABI note

OpenOrbis v0.5.4 is the latest public release shown in its release list as of this audit. The release notes specifically change OrbisPadData placement of acceleration and angularVelocity, update kernel.h, add sceSystemServiceAddLocalProcess args, and fix several struct/prototype details. This reinforces the need to pin a toolchain version before consuming raw native controller structures.

The PS4 CI previously used a mutable Docker tag openorbisofficial/toolchain:latest. On 2026-11-02, the successful build pulled image digest:

sha256:f752d09f9d3227c729ee069f29a05450bd8fa4ef5e785c0929ab8479f7dc4e64

AMEGA's PS4 workflow now pins that exact image digest. It keeps builds reproducible and prevents an upstream image update from silently changing headers/libraries between runs. The build container's public headers/stubs must still be verified against the specific symbol being used; the attempted system-version call was removed from the shipping receiver because its type did not exist in that pinned build image.

Current controller decision:
- keep SDL GameController + raw SDL Joystick as the application input route;
- do not replace it with direct scePad structure access until we pin the v0.5.4 headers/library stubs, inspect exact OrbisPadData ABI, and run a USB/Bluetooth DS4 hardware test.

Reference: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases
