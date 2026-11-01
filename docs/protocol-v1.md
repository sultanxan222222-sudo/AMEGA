# AMEGA LAN protocol — version 1

Status: design baseline. Implementations must validate every field and reject unknown major versions.

## 1. Transport

- Discovery: UDP IPv4 broadcast on the configured AMEGA discovery port.
- Control and data: HTTP over the local network in the first development milestone.
- Production hardening: authenticated HTTPS or an authenticated encrypted session established during pairing. Plain HTTP must not be treated as secure on untrusted networks.
- All messages are UTF-8 JSON unless the endpoint explicitly serves binary file data.
- Maximum discovery datagram: 1,200 bytes.

The discovery port and HTTP port are configuration values, not hard-coded assumptions. The initial defaults will be recorded in the implementation configuration and may be changed to avoid conflicts.

## 2. Discovery beacon

The sender periodically broadcasts a JSON object matching `protocol/schemas/discovery-v1.schema.json`.

Required fields:
- `protocol`: `"amega"`
- `major`: `1`
- `type`: `"sender_announcement"`
- `sender_id`: stable random installation ID, not a username or hardware serial
- `instance_nonce`: fresh random value on each app launch
- `http_port`: active local HTTP port
- `display_name`: user-editable device label

Beacon data is unauthenticated discovery data. It must never grant access to a file.

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
