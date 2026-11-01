# AMEGA for PS4

Planned implementation: PS4 homebrew receiver for a compatible HEN environment.

Responsibilities:
- listen for AMEGA sender announcements;
- display discovered PCs and pairing state;
- show file offer metadata and require explicit acceptance;
- check free space and receive into a staging directory;
- support resume and SHA-256 verification;
- expose a separate installation action only when a supported HEN adapter is detected.

The receiver must remain useful for safe file reception even when package-install capability is unavailable. Firmware and GoldHEN compatibility must be measured on real hardware.
