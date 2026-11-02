# AMEGA security model

## Threat assumptions

The current development target is a trusted private home LAN. Discovery and the current HTTP control/data routes are plaintext. Wi-Fi networks can contain untrusted devices; a beacon is public information, not proof of identity.

## Implemented Windows-side controls

- Discovery replies are validated against the current PC process nonce, a receiver UUID, and a 32-character receiver nonce.
- The Windows accept endpoint only accepts the current transfer ID and a receiver UUID/nonce observed within the previous 15 seconds.
- A successful accept creates a random 256-bit token scoped to one transfer and one receiver IP.
- The token expires after 30 minutes, and changing/removing the offer or stopping the sender revokes it.
- The binary file endpoint requires the token and requires the source IP to match the authorized receiver.
- File serving is streamed from disk and supports byte-range responses; the full PKG is not loaded into RAM.

These controls reduce accidental access on a home LAN. They are not a substitute for authenticated encryption or a pairing code against an active LAN attacker.

## Required before release

1. **Explicit consent on both ends:** no file starts transferring until the PS4 user accepts the offer. Installation always requires a separate confirmation.
2. **Pairing confirmation:** implement a short-lived code displayed on both screens, compare it, and rate-limit retries. The current prototype does not yet implement that user-facing code.
3. **Authenticated transport:** replace plaintext HTTP with TLS whose identity is validated correctly, or another authenticated encrypted session.
4. **Token handling:** never log, display in ordinary UI, or place the token in a URL. Revoke it on cancel, app exit, expiry, or offer change.
5. **Path safety:** never use a peer-supplied path as a filesystem path. Normalize names, reject traversal, and create destinations under an app-owned directory.
6. **Integrity:** verify exact byte length and whole-file SHA-256 on the receiver before marking a package complete or enabling installation.
7. **Resource limits:** cap metadata sizes, concurrent transfers, request rates, and disk reservations. Check free space before accepting.
8. **Safe cleanup:** preserve the sender's original PKG. Never delete a completed package automatically.
9. **Logging:** redact tokens, pairing codes, local absolute paths, and sensitive network details.
10. **Failure behavior:** fail closed when authentication, protocol version, digest, or platform capability checks fail.

## Explicit non-goals

AMEGA is not a remote-control tool, does not bypass PS4 licensing or platform security, and does not install packages without the user's consent. It does not require a cloud relay.

## Release gate

A public/release build must not be labeled secure until user-visible pairing, authenticated encrypted transport, path validation, disk-space checks, and negative security tests have been implemented and reviewed.
