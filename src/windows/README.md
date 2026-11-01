# AMEGA for Windows

Planned implementation: a native Windows desktop sender.

Responsibilities:
- select PKG/test files;
- show filename, size, and SHA-256;
- advertise the sender on the local network;
- pair with a receiver and authorize one transfer;
- serve verified byte ranges;
- show queue, speed, progress, pause/resume, and errors.

The UI must be Russian-first. The sender must never delete source packages.

Implementation decision: keep UI, discovery, transfer server, and state storage in separate modules. Do not put network or filesystem logic directly in button handlers.
