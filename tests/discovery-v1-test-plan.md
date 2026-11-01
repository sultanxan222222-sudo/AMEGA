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
