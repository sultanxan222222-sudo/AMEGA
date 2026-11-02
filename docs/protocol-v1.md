# AMEGA LAN protocol — version 1

Status: design baseline. Implementations must validate every field and reject unknown major versions.

## 1. Transport

- Discovery: UDP IPv4 per-interface directed broadcast plus bounded unicast subnet probes. PC-to-PS4 announcements use UDP 38470; PS4-to-PC replies use UDP 38472. The distinct ports avoid competing wildcard binds. On Windows, the AMEGA executable must be permitted to receive inbound UDP 38472 on a trusted Private profile; the future local HTTP host uses TCP 38471.
- Control and data: HTTP over the local network in the first development milestone.
- Production hardening: authenticated HTTPS or an authenticated encrypted session established during pairing. Plain HTTP must not be treated as secure on untrusted networks.
- All messages are UTF-8 JSON unless the endpoint explicitly serves binary file data.
- Maximum discovery datagram: 1,200 bytes.

Initial development defaults: UDP 38470 and HTTP 38471. These are configurable implementation defaults, not protocol constants.

## 2. Bidirectional discovery

The PS4 receiver retries UDP discovery socket initialization every three seconds after a startup failure and exposes its current IPv4 address independently from socket status. The Windows build includes a narrowly scoped firewall helper for UDP 38472 and TCP 38471; it must not enable these rules on Public profiles.

Discovery is a challenge/reply exchange:

- Windows sends `sender_announcement` beacons to PS4 UDP 38470, using each active adapter's actual IPv4 subnet broadcast address. It also performs a bounded unicast sweep of each subnet every 15 seconds as a fallback when an access point drops broadcasts.
- PS4 listens on UDP 38470 and immediately replies to the sender's observed source IP at Windows UDP 38472. The reply matches `protocol/schemas/receiver-discovery-v1.schema.json` and echoes the active PC `instance_nonce` in `reply_to_nonce`.
- Windows accepts only a response with the current process nonce, a valid receiver UUID, a 32-character hexadecimal instance nonce, and bounded display fields. This rejects stale replies from a previous PC process.

The Windows app listens for receiver announcements; the PS4 app listens for sender announcements. Each beacon includes a random installation ID, a fresh per-launch nonce, protocol major version, and display name. The sender announcement also includes the active HTTP port.

Discovery is unauthenticated and grants no access. It contains no file path, package contents, authentication secret, or hardware serial. Networks with client isolation, guest Wi-Fi, VLAN separation, or host firewalls may block peer traffic. A bounded subnet sweep can bypass broadcast suppression, but it cannot bypass client isolation or firewall rules.

## 3. Pairing and offer

The receiver selects a sender and starts a pairing request. The sender displays a short-lived one-time code; the receiver asks the user to confirm the same code. A successful pairing creates a random session token with limited lifetime and scope.

After pairing, the sender exposes an offer containing:
- random `transfer_id`;
- safe display filename;
- exact byte size;
- package kind (`pkg` or `test_file`);
- SHA-256 digest;
- creation time and protocol version.

The offer is metadata only. No payload moves until the receiver explicitly accepts it.

## 4. Transfer endpoints

The concrete route names are reserved for the implementation and must remain under a versioned prefix such as `/api/v1/`.

Required behavior:
- Fetch offer metadata for a transfer.
- Accept or decline a transfer.
- Fetch file bytes with HTTP Range support.
- Query transfer status and resume offset.
- Acknowledge final receiver-side verification.

Range semantics must follow HTTP byte ranges. A resumed request must validate the returned `Content-Range`; a server must not silently return the wrong bytes. Partial responses use HTTP 206. Invalid or unsatisfiable ranges must return the correct error status.

## 5. Resume rules

The receiver persists transfer ID, expected file size, expected SHA-256, safe destination, and verified byte count. After reconnecting, it asks the sender for transfer status and continues from the last safely persisted offset.

The receiver must not trust a locally stored offset by itself. It validates the partial file length and re-hashes the final complete file. If metadata differs, the partial file is quarantined or discarded only after user-safe cleanup rules are applied.

## 6. Integrity and completion

SHA-256 is computed from the complete received file. The sender's digest is compared with the receiver's digest. A transfer becomes `COMPLETE` only after equality and a final acknowledgement.

A mismatch results in `FAILED_INTEGRITY`; the file must not be offered for installation.

## 7. Error model

Errors use stable machine-readable codes, a human-readable message, and a retryable flag. Do not expose local absolute paths, secrets, or stack traces to the network peer.

## 8. Compatibility policy

Minor additions may be optional. A breaking change requires a new major protocol version. Both apps show a clear incompatibility message rather than attempting an unsafe downgrade.
