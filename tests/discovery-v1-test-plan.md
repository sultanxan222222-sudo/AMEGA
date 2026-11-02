# AMEGA discovery v1 test plan

## Wire contract

- Windows sender probes: UDP 38470.
- PS4 receiver replies: UDP 38472.
- The PS4 reply echoes the active Windows `instance_nonce` in `reply_to_nonce`.
- Windows accepts only a UUID receiver ID, a 32-character hexadecimal receiver nonce, a non-empty display name (1–48 chars), a version (1–32 chars), and an exact nonce match.
- A stale reply from an earlier Windows process is ignored even when its other fields are valid.

## Required network cases

1. Same Wi-Fi subnet: directed broadcast discovers the console.
2. Access point suppresses broadcast but permits peer traffic: bounded unicast sweep discovers the console.
3. Multiple adapters (Wi-Fi + Ethernet + VPN/virtual): probes are sent from each active IPv4 adapter using that adapter's actual mask and directed broadcast.
4. Non-/24 subnet: broadcast and sweep use the real mask; no /24 assumption.
5. PC restarts: the previous process nonce is rejected; a fresh reply is accepted.
6. PS4 restarts: its installation ID remains stable, its per-launch nonce changes, and the PC refreshes last-seen state.
7. Client isolation / guest Wi-Fi: no device is shown; the UI must not claim that scanning can bypass isolation.
8. Malformed/oversized JSON, wrong protocol major/type, invalid UUID, non-hex nonce, wrong echoed nonce, or overlong display fields are ignored.

## PS4 startup checks

- SDL accelerated renderer is tried first; SDL software renderer is the fallback.
- The window is created with SDL_WINDOW_SHOWN.
- Startup failures are written to `/data/amega-startup.log` and return an error instead of entering an infinite sleep loop.
- The application uses SDL_RenderPresent only; it does not mix the renderer with SDL_UpdateWindowSurface.

CI validates compilation and package creation. Network cases and visible PS4 rendering still require real hardware on the same LAN.

## 5. Asymmetric PC-search / PS4-standby behavior

1. Start AMEGA on PS4 without starting the PC app. The PS4 discovery screen must show "ОЖИДАНИЕ ПОДКЛЮЧЕНИЯ С ПК"; it must not transmit periodic receiver announcements.
2. Start Search on Windows. The PC sends probes from UDP 38472 to UDP 38470; the PS4 receives each probe and returns a nonce-matched unicast response.
3. Verify that Windows lists the PS4 name, IPv4 address, and firmware version, and that the PS4 lists the PC name, IPv4 address, and HTTP port.
4. Stop Windows discovery. The PS4 remains in standby and the PC entry expires from the PS4 list; no background PS4 broadcast is emitted.
5. Restart Windows discovery and confirm both lists repopulate without restarting the PS4 app.

The automated OpenOrbis build validates compilation only; the above packet-flow and UI checks still require a real PS4 and Windows host on the same LAN.

## 6. PS4 local IPv4 initialization and reporting

1. Launch AMEGA after the PS4 has connected to Ethernet or Wi-Fi and received its network configuration.
2. Confirm the application loads `libSceNet` and `libSceNetCtl`, calls `sceNetInit`, creates the socket pool, then calls `sceNetCtlInit` before querying IP or creating a socket.
3. Compare the AMEGA IP display with Settings → Network → View Connection Status on the same console.
4. Repeat on Wi-Fi and Ethernet when available; the displayed address must not be loopback, 0.0.0.0, link-local 169.254/16, or multicast.
5. Temporarily disconnect the network and confirm AMEGA shows a clear no-IP state rather than a stale or unrelated interface address.

The `sceNetCtlGetInfo` path and module initialization still require on-console validation; CI can only confirm that the code links and packages.
