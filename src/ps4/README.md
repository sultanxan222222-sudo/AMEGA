# AMEGA for PS4

Native PS4 homebrew receiver prototype for a compatible HEN environment.

## Current receiver prototype

- Starts a PS4 SDL2 interface with a Russian-first UI.
- Supports DualShock 4 navigation (D-pad, Cross, Circle).
- Broadcasts an AMEGA `receiver_announcement` every three seconds on UDP 38470.
- Listens for Windows `sender_announcement` beacons on UDP 38470.
- Shows discovered PCs, their local IP address, and announced HTTP port.
- Persists a receiver installation ID at `/data/amega_receiver_id.txt`.
- Expires devices that have not announced themselves for 12 seconds.

The Cross button selects a discovered PC. It does not claim to receive a package yet. Pairing, explicit transfer acceptance, resumable download, SHA-256 verification, and installation are later implementation stages.

## Build

Use the OpenOrbis PS4 Toolchain with SDL2 support. The GitHub Actions workflow uses the OpenOrbis toolchain container.

Locally, with the toolchain installed:

```bash
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis
make clean all
```

Expected output: `AMEGA-PS4.pkg`.

A successful build only proves that the package was produced. It still must be installed and tested on a real PS4 Pro running the target HEN/GoldHEN version. This prototype is for discovery only; it is not yet a game receiver.
