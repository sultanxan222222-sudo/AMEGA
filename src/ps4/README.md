# AMEGA for PS4

Native PS4 homebrew receiver prototype for a compatible HEN environment.

## Current receiver prototype 0.2.0

- Starts a PS4 SDL2 interface with a Russian-first UI.
- Supports DualShock 4 navigation (D-pad, Cross, Circle).
- Listens for Windows `sender_announcement` probes on UDP 38470.
- Immediately unicasts a nonce-bound `receiver_announcement` reply to the PC on UDP 38472.
- Uses per-interface directed broadcast for periodic replies after the first PC probe.
- Shows discovered PCs, their local IP address, and announced HTTP port.
- Persists a receiver installation ID at `/data/amega_receiver_id.txt`.
- Expires devices that have not announced themselves for 12 seconds.
- Explicitly enables `SO_BROADCAST` and logs SDL startup failures to `/data/amega-startup.log`.
- Tries the accelerated SDL renderer first and the software renderer second; it no longer mixes the renderer API with `SDL_UpdateWindowSurface`.

The OpenOrbis CI workflow successfully generated `AMEGA-PS4.pkg` (6,619,136 bytes). The package has not been installed or tested on a real PS4 Pro yet.

The Cross button selects a discovered PC. It does not claim to receive a package yet. Pairing, explicit transfer acceptance, resumable download, SHA-256 verification, and installation are later implementation stages.

## Build

Use the OpenOrbis PS4 Toolchain with SDL2 support. The GitHub Actions workflow uses the OpenOrbis toolchain container and generates the icon and GP4 manifest on the runner.

Locally, with the toolchain installed:

```bash
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis
python3 tools/generate_icon.py src/ps4/sce_sys/icon0.png
python3 tools/create_gp4.py --template src/ps4/openorbis-template.gp4 --content-id IV0000-AMEG00001_00-AMEGARECEIVER000 --output src/ps4/pkg.gp4
cd src/ps4
make clean all
```

A successful build proves that the package was produced. It does not prove operation on a real PS4. This prototype is for discovery only; it is not yet a game receiver.
