# PS4 HEN / GoldHEN compatibility

**Research snapshot:** 2026-11-02. This document tracks public reports and upstream project documentation; it does not imply that AMEGA has been hardware-tested on every listed combination.

## Compatibility layers

A PS4 exploit, kernel patch, HEN payload, GoldHEN services, a separate RPI homebrew, and an AMEGA build are different layers. AMEGA must query the feature that it needs instead of assuming a working port based on firmware alone.

## Firmware snapshot

| Firmware | Public evidence reviewed | AMEGA policy |
|---|---|---|
| 5.05, 6.71/6.72, 9.00, 9.60, 10.00/10.01, 10.50, 10.70/10.71, 11.00 | Listed by the public GoldHEN v2.4b18 GitHub pre-release | Probe each service at runtime |
| 12.00, 12.02 | Scene-Collective PS4HEN v2.2.0 BETA README lists support through 12.02, with Remote Package Install | Treat as open-source HEN, not GoldHEN; test the actual service |
| 12.50, 12.52 | Some exploit entry points are documented, but current HenJB matrix reports no shipped kernel patch | Do not infer a running HEN from an entry-point exploit |
| 13.02, 13.04, 13.50 | SiSTRo's own GoldHEN v2.4b18.12 beta adds these three firmwares | GoldHEN beta; record exact build |
| 13.52 | SiSTRo's separate GoldHEN v2.4b18.11 beta specifically adds 13.52; community hardware test results exist | GoldHEN beta; record exact build |
| 14.00 | BD-JB / open-source HEN activity is experimental and scene sources conflict; GoldHEN is not a reliable assumption | Core AMEGA networking must not depend on GoldHEN |
| Later than 14.00 | No supported HEN path established by sources reviewed for this snapshot | No compatibility claim |

Firmware exploit availability does not guarantee a stable HEN payload. In particular, GoldHEN 2.4b18.11 and 2.4b18.12 are separate beta builds for different 13.xx firmware groups. GoldHEN support also does not guarantee that optional services are enabled.

## GoldHEN capabilities

The GoldHEN project lists Remote Package Install, FTP on TCP 2121, BinLoader on TCP 9090, Klog on TCP 3232, plugins and internal package installation from `/data/pkg`.

AMEGA rules:
- Detect optional service liveness before exposing an adapter.
- Keep core discovery and future native transfer independent from FTP, BinLoader and Klog.
- Never use BinLoader for ordinary file transfer; GoldHEN labels it experimental and warns that arbitrary payloads may crash the console.
- Treat Klog as opt-in diagnostics only.
- Package Installer can scan USB and, when configured, internal `/data/pkg`; copy to that directory only after a successful transfer and explicit user action.
- Check internal free space before any staging copy. A package copy plus install activity can require more space than the PKG file size.

## Two remote-install mechanisms that must not be conflated

1. **GoldHEN's advertised Remote Package Install feature.** GoldHEN lists this capability, but the project-level feature list does not establish the exact API, port, or runtime service health for every version. Probe and test the real adapter.
2. **Standalone Remote Package Installer homebrew (RPI).** The independent app documents an API on TCP 12800, including `POST /api/is_exists`, `POST /api/install`, task progress and control endpoints. It must remain in the foreground for this API implementation; when suspended, it may stop answering.

A TCP connection to port 12800 is not a health check. A current PC sender documents an application-level liveness request because the RPI event loop can stop responding while the TCP port still appears open. During active package downloads, the RPI API may also pause its progress responses; a sender should not falsely report a failed transfer just because a progress poll times out.

Any RPI route requires correct HTTP Range handling: `206 Partial Content`, `Content-Range`, and `416` for an impossible range. A generic whole-file HTTP endpoint can break PKG metadata reads.

## Transfer and installation must remain separate

- Native AMEGA transfer should run on the local network without requiring a third-party installer.
- Receive into an AMEGA staging directory, persist byte offset and transfer ID, resume only from validated offsets, and hash the completed file on the PS4.
- Mark a file VERIFIED only after receiver-side SHA-256 matches the PC manifest.
- Require separate user confirmation to install.
- Report RECEIVED, VERIFIED, INSTALL_QUEUED, INSTALLING, INSTALLED and FAILED as distinct states.
- Never delete the source package automatically.

## OpenOrbis interfaces

OpenOrbis headers provide networking APIs, `sceNetCtl` network information, `scePad` controller APIs, `sceVideoOut` output APIs, system-module calls and AppInstUtil declarations. The public toolchain warns that header/stub coverage may be incomplete or need updates. AMEGA may use these APIs only behind tested adapters.

- Current receiver uses POSIX sockets and `getifaddrs`; future network diagnostics may compare them with `sceNetCtlGetInfo` (IP, netmask, route, SSID/RSSI).
- Current controller uses SDL joystick events; `scePadRead` may serve as a native fallback after hardware testing.
- Keep SDL as the working renderer; direct VideoOut/GPU code requires a complete buffer/flip lifecycle, not just a function call.
- Do not directly call AppInstUtil as the default installer. Reverse-engineering research documents a proof of concept that returned success but produced a package whose title later failed due to filesystem block mapping. The API declaration itself is not proof of safe installation.

## Hardware validation record required

Record exact console model, firmware, HEN name/version/build, enabled services, RPI app state, Ethernet/Wi-Fi, router isolation settings, test file size, launch/exit, controller input, discovery, transfer/resume, digest and installation result. A CI compile is not a hardware pass.

## New receiver diagnostics in AMEGA 0.2.2

The PS4 receiver now:
- displays its own detected local IPv4 address;
- attempts to read the system software version through the documented current OpenOrbis sceKernelGetSystemSwVersion ABI;
- includes that version as an optional firmware_version field in its UDP announcement;
- lets the Windows UI show the reported firmware beside the discovered console.

The bundled OpenOrbis build container has an older no-argument placeholder declaration for this import. The source uses a local ABI struct and a typed function-pointer bridge based on current upstream OpenOrbis headers. CI compilation passes, but the call is **not hardware-validated yet**. If it returns an error or malformed value, the receiver shows 'не определена'; AMEGA never guesses firmware from model or jailbreak method.

This is diagnostic information only. The actual installer mode still must be detected using an application-level API or explicit user selection.

## Primary firmware references

- SiSTRo GoldHEN v2.4b18.11 (13.52 beta): https://ko-fi.com/s/ccbe9f7bf0
- SiSTRo GoldHEN v2.4b18.12 (13.02 / 13.04 / 13.50 beta): https://ko-fi.com/s/85ea22bd2f
- GoldHEN public GitHub v2.4b18 feature list / supported firmware: https://github.com/GoldHEN/GoldHEN/releases
- Community hardware test details for 13.50 and 13.52: https://github.com/caziques/PS4
- Current PS4 firmware entry-point status: https://consolemods.org/wiki/Ps4_getting_started
- Firmware chain status: https://henjb.com/