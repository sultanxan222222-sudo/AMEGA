# AMEGA security model

## Threat assumptions

The current development target is a trusted private home LAN. Discovery and HTTP control/data routes are plaintext. Do not expose the sender to the public Internet, guest Wi-Fi, or a network containing untrusted clients.

## Implemented controls

- Discovery replies are validated against the current PC process nonce, receiver UUID and 32-character receiver nonce.
- Windows publishes an offer only after the user selects a discovered PS4 and presses Send PKG.
- Each offer is bound to one receiver UUID; changing the selected receiver revokes the offer/token.
- The PS4 checks the sender UUID from its own selected-PC record before displaying an offer.
- The Windows accept endpoint checks the current transfer ID, receiver UUID, nonce, fresh discovery record and source IP.
- A successful accept creates a random 256-bit token scoped to one transfer and one receiver IP.
- The token expires after 30 minutes; it is kept in PS4 memory only and is not persisted in resume metadata or logs.
- The binary endpoint requires the token and authorized source IP.
- The receiver requests HTTP Range intervals and validates the returned status, Content-Range, total size and Content-Length.
- The receiver writes only to a fixed AMEGA staging directory. The sender-provided filename is display-only and never becomes a path.
- Resume metadata binds a partial file to transfer ID, expected size and whole-file SHA-256. A mismatch discards the partial file.
- The receiver independently hashes the completed file. Only an exact digest match is renamed to the final PKG.
- The sender's original package is never deleted automatically.

These controls reduce accidental access and corruption on a trusted LAN. They are not a substitute for authenticated encryption or a pairing code against an active LAN attacker.

## Required before release

1. **Explicit consent on both ends:** no transfer starts until the PS4 user accepts. Installation remains a separate future action.
2. **Pairing confirmation:** implement a short-lived code displayed on both screens, compare it, and rate-limit retries. The current prototype does not yet implement this user-facing code.
3. **Authenticated transport:** replace plaintext HTTP with TLS whose identity is validated correctly, or another authenticated encrypted session.
4. **Token handling:** never log, display in ordinary UI, or place the token in a URL. Revoke on cancel, app exit, expiry or offer change.
5. **Path safety:** never use a peer-supplied path as a filesystem path. Keep destinations under the app-owned directory.
6. **Integrity:** require exact byte length and receiver-side whole-file SHA-256 before marking complete or enabling a future installer.
7. **Resource limits:** cap metadata sizes, concurrent transfers, request rates and disk reservations.
8. **Safe cleanup:** preserve the sender's original PKG. Never delete a verified received package automatically.
9. **Logging:** redact tokens, pairing codes, local absolute paths and sensitive network details.
10. **Failure behavior:** fail closed when authentication, protocol version, range, digest or platform capability checks fail.

## Explicit non-goals

AMEGA is not a remote-control tool, does not bypass PS4 licensing or platform security, and does not install packages without the user's consent. It does not require a cloud relay.

## Release gate

A public/release build must not be labeled secure until pairing, authenticated encrypted transport, rate limits and negative security tests have been implemented and reviewed. A CI test or successful PKG build is not a real-console hardware pass.
