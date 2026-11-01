# AMEGA security model

## Threat assumptions

The first target is a private home LAN, but Wi-Fi networks can contain untrusted devices. A discovery beacon is visible to nearby network participants and must be treated as public information.

## Required controls

1. **Explicit consent:** no file starts transferring until the PS4 user accepts the offer. Installation always requires a separate confirmation.
2. **Pairing:** use a cryptographically random, short-lived one-time code and compare it on both screens. Rate-limit attempts and expire codes quickly.
3. **Session authorization:** issue a high-entropy random token after pairing. Scope it to one sender instance and an explicit transfer; expire it and revoke it on cancel or app exit.
4. **Transport protection:** do not claim that plain HTTP is secure. Before production release, protect control messages and file data with authenticated encryption or a correctly validated TLS session.
5. **Path safety:** never use a peer-supplied path as a filesystem path. Normalize display names, reject traversal, and create destinations under an app-owned directory.
6. **Integrity:** verify exact byte length and SHA-256 before marking a package complete or enabling installation.
7. **Resource limits:** cap metadata sizes, concurrent transfers, request rates, and disk reservations. Check free space before accepting.
8. **Safe cleanup:** preserve the sender's original PKG. Delete partial files only under clear state rules; never delete a completed package automatically.
9. **Logging:** redact tokens, pairing codes, local absolute paths, and sensitive network details.
10. **Failure behavior:** fail closed when authentication, protocol version, digest, or platform capability checks fail.

## Explicit non-goals

AMEGA is not a remote-control tool, does not bypass PS4 licensing or security, and does not install packages without the user's consent. It does not require a cloud relay.

## Release gate

A public/release build must not be labeled secure until authenticated transport, pairing rate limits, path validation, disk-space checks, and negative security tests have been implemented and reviewed.
