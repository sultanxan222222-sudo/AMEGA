# AMEGA LAN protocol — version 1

**Status:** native file transfer, receiver-side resume and SHA-256 verification are implemented in the development branch and pass local integration tests. A real PS4 hardware run is still required.

## 1. Transport

- Discovery: the PC is the only active searcher. It sends UDP IPv4 per-interface directed broadcasts plus bounded unicast probes to PS4 port 38470 from its fixed UDP 38472 listener socket. The PS4 stays in standby, listens on 38470, and responds only to a received PC probe; it does not broadcast periodic discovery announcements. PS4 replies are sent to the PC's fixed UDP 38472 listener and, for compatibility, to the observed source port when different.
- Control and data: HTTP on the trusted home LAN; Windows sender listens on TCP 38471.
- Messages are UTF-8 JSON except the binary file endpoint.
- Maximum discovery datagram: 1,200 bytes.
- Current HTTP is plaintext. Do not expose the sender to the public Internet, guest Wi-Fi, or an untrusted LAN.

## 2. Discovery

Windows is the only active discoverer: it sends sender announcements every three seconds to UDP 38470 from its already-bound UDP 38472 socket, and listens for PS4 replies on that same socket. The PS4 is a standby responder: it listens on UDP 38470, records each PC that probes it, and sends a unicast receiver announcement only in response to that probe. The response goes to the fixed 38472 listener and, for compatibility, to the beacon's observed source port when different. Each receiver announcement includes receiver UUID, per-launch nonce, display name, version, optional firmware string, and the specific PC nonce echoed in `reply_to_nonce`.

Windows accepts a reply only when its UUID and nonce are valid and `reply_to_nonce` matches the current PC process nonce. PS4 stores each discovered PC's own nonce and IP in that sender's record; it does not use a global "last PC" nonce when accepting an offer.

Discovery is unauthenticated and grants no file access.

## 3. Offer and explicit consent

Windows publishes an offer only after the PC user selects a discovered PS4 and presses **Send PKG**.

The offer contains:
- `transfer_id`: deterministic UUID derived from the first 128 bits of the normalized whole-file SHA-256 (with UUID version/variant bits set); the same file therefore keeps the same resume key across PC application restarts;
- `sender_id`: current PC process UUID;
- `file_name`: display name only, never an absolute path;
- `file_size`: exact file length;
- `file_kind`: `pkg`;
- `sha256`: whole-file SHA-256;
- `created_utc` and protocol major version.

`GET /api/v1/offer` returns 409 until the PC user has prepared an offer for a selected receiver.

The PS4 user selects a PC and presses Cross to fetch the offer. The receiver validates the protocol, message type, sender ID, transfer UUID, PKG kind, positive length, and 64-hex SHA-256 before displaying the package card.

### POST /api/v1/accept

The PS4 sends:
```json
{
  "transfer_id": "content-derived UUID",
  "receiver_id": "PS4 receiver UUID",
  "instance_nonce": "32 hexadecimal characters from the current PS4 process",
  "reply_to_nonce": "32 hexadecimal characters from the selected PC process"
}
```

Windows checks:
- transfer ID matches the current offer;
- selected receiver UUID and nonce match a recent discovery entry;
- reply-to nonce matches the current PC process nonce;
- source IP matches the discovered PS4;
- offer is still bound to that PS4.

A successful response contains `accepted: true`, the transfer ID, a random 256-bit `access_token`, and `expires_in_seconds: 1800`. Only one receiver may hold an active token for the current offer. Changing the offer or stopping the sender revokes it.

The PS4 keeps the token in memory only. It is not written to the resume file or log.

## 4. Binary download route

### GET /api/v1/file

The PS4 downloads only after explicit Accept. Each request includes:
- `X-AMEGA-Token`: current receiver-bound access token;
- `Range: bytes=<offset>-`: actual number of bytes already stored in the partial file;
- `If-Range: "<whole-file-sha256>"`: prevents resuming against a changed representation;
- `Accept-Encoding: identity`.

Windows requires the correct token and source IP, then streams the selected file from disk.

The receiver accepts only `206 Partial Content`. It validates:
- `Content-Range` starts at the requested offset;
- total length equals the offer's expected file size;
- end offset is not before start;
- `Content-Length` exactly matches the returned byte interval.

A `200`, `416`, mismatched range, missing header, or expired token is not treated as a successful partial response.

## 5. Staging, resume and verification

The PS4 receiver writes only into its own fixed directory:
- temporary data: `/data/AMEGA/Download/<transfer_id>.part`;
- resume metadata: `/data/AMEGA/Download/<transfer_id>.resume`;
- final verified package: `/data/AMEGA/Download/<transfer_id>.pkg`.

The network-supplied filename is display metadata only. It is never used as a filesystem path.

Before receiving, the PS4:
- validates the transfer UUID, access token and expected hash format;
- creates the AMEGA staging directory;
- checks available disk space, reserving an additional 16 MiB;
- compares stored resume metadata with the transfer ID, expected length and SHA-256;
- discards a partial file if its metadata does not match the current offer.

The receiver streams in bounded memory, updates its actual partial-file length, and syncs data periodically. If the connection is interrupted or the user presses Circle, the partial file and metadata remain. Pressing Cross again resumes from the actual partial-file length; after an app restart, the user must fetch and accept the same offer again to receive a fresh token.

After the expected byte length has been received, the PS4 independently calculates whole-file SHA-256. Only an exact digest match is renamed to the final `.pkg`. A digest mismatch deletes the partial file and resume metadata and leaves no completed package. The sender's original file is never deleted.

## 6. Transfer states and installation

The UI distinguishes offer, accepted, downloading, paused/failed, and verified states. A file is not marked complete until receiver-side SHA-256 matches.

**Installation is not implemented in the current prototype.** A verified PKG remains in the AMEGA Download folder. Any future RPI, GoldHEN or native installer integration must be a separate action with separate consent and a confirmed installer task result.

## 7. Security limitations

The current prototype implements receiver UUID/nonce validation, selected-target binding, source-IP checks, a random 256-bit token, 30-minute expiry, Range validation and receiver-side digest verification.

This is still not production security because control/data are plaintext HTTP and local discovery is unauthenticated. Before a release on untrusted networks, add explicit pairing confirmation and authenticated encrypted transport.

## 8. Automated tests and hardware acceptance

CI includes:
- SHA-256 known-answer tests for empty input, `abc`, and one million `a` bytes;
- a local Range integration test that interrupts a 4,194,427-byte test transfer after 600,000 bytes, resumes from that exact offset, and verifies the resulting digest;
- OpenOrbis PS4 compilation and PKG packaging.

These tests do not prove a real PS4 will mount the package, reach Windows through every router, or write to its internal storage. Hardware acceptance remains open.
