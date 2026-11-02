# AMEGA LAN protocol — version 1

Status: bidirectional discovery plus explicit PC Send / PS4 Accept handshake. Windows authorization and Range-capable file serving are implemented; PS4 offer retrieval and accept are implemented. PS4 file download, staging, resume and receiver-side digest verification are still not implemented, so end-to-end transfer is not complete.

## 1. Transport

- Discovery: UDP IPv4 per-interface directed broadcast plus bounded unicast subnet probes. PC-to-PS4 announcements use UDP 38470; PS4-to-PC replies use UDP 38472.
- Control and data: HTTP on the trusted home LAN; Windows HTTP service uses TCP 38471.
- Plain HTTP is not secure on an untrusted network. Do not expose the service to the public Internet or a guest Wi-Fi network.
- Messages are UTF-8 JSON unless an endpoint explicitly serves binary file data.
- Maximum discovery datagram: 1,200 bytes.

## 2. Bidirectional discovery

- Windows broadcasts sender announcements on UDP 38470 using each active adapter's actual subnet broadcast address; it also performs a bounded unicast sweep every 15 seconds.
- PS4 listens on UDP 38470 and replies to the observed PC address on UDP 38472.
- A reply includes the PS4 receiver UUID, a 32-character per-launch nonce, display name, receiver version, and the PC nonce echoed in reply_to_nonce.
- Windows accepts a reply only if its UUID and nonce are valid and reply_to_nonce matches the current PC process nonce.
- Windows stores the receiver's latest nonce and IP for a short-lived accept request.

Discovery is unauthenticated and grants no access. It contains no local file path, package contents, token, or hardware serial. Client isolation, VLAN separation, and host firewalls may still block peer traffic.

## 3. Offer metadata

When a PKG is selected and its whole-file SHA-256 is complete, Windows exposes:
- random transfer_id;
- sender_id;
- display filename (never a local absolute path);
- exact file_size;
- file_kind;
- whole-file SHA-256;
- creation time and protocol major version.

GET /api/v1/offer returns 409 until a verified offer is ready. GET /api/v1/health exposes offer_ready and receiver_authorized.

## 4. Windows-side accept gate and binary file route

The Windows sender currently implements the server side of this stage. An outgoing offer is created only after the PC user selects a discovered PS4 and presses Send PKG. The offer is bound to that receiver UUID; changing the target revokes the previous offer and token.

### POST /api/v1/accept

Request JSON:

```json
{
  "transfer_id": "current offer UUID",
  "receiver_id": "discovered PS4 UUID",
  "instance_nonce": "32 hexadecimal characters from latest PS4 beacon",
  "reply_to_nonce": "current PC process nonce"
}
```

Validation:
- transfer_id must match the current offer;
- receiver_id and instance_nonce must match a receiver discovered within the last 15 seconds;
- reply_to_nonce must match the current Windows process nonce;
- only one receiver may hold an active token for the current offer.

Successful response includes accepted=true, transfer_id, a cryptographically random 256-bit access_token, and an expiry of 1,800 seconds. A new offer, stop, or expiry revokes the token.

**PS4 client behavior now implemented:** when the user selects a PC and presses Cross, the receiver requests GET /api/v1/offer and displays the filename, size and SHA-256. A second Cross on the offer screen sends POST /api/v1/accept using the receiver UUID, receiver nonce, and the selected PC nonce. The PS4 stores the returned access token in memory and shows an Accepted state. Circle returns to the previous screen. The PS4 does not yet issue GET /api/v1/file, so the file is not downloaded by this handshake alone.

### GET or HEAD /api/v1/file

The route is unavailable until POST /api/v1/accept succeeds. Requests must include:
- header X-AMEGA-Token with the short-lived token;
- source IP matching the PS4 receiver that was authorized.

The route streams the selected file from disk; it does not load the whole PKG into RAM. It supports HTTP byte ranges through the ASP.NET Core file result:
- a valid range returns 206 Partial Content and Content-Range;
- an unsatisfiable range returns 416;
- a complete GET returns 200;
- ETag is set to the whole-file SHA-256.

The receiver must still validate every Content-Range response and independently hash the full received file before marking it complete.

## 5. Remaining transfer behavior

Not yet implemented end-to-end:
- PS4 range-download client using X-AMEGA-Token and validated Content-Range;
- safe staging directory and free-space reservation;
- persistent resume metadata and retry behavior;
- receiver-side SHA-256 verification and final acknowledgement;
- transfer status, speed, pause, cancel, and resume endpoints.

A transfer becomes COMPLETE only after the receiver confirms exact byte length and matching whole-file SHA-256. Installation is a separate action and must never start automatically.

## 6. Security limitations

The current accept gate checks receiver ID, fresh discovery nonce, PC nonce echo, source IP, token entropy, token lifetime, and single-receiver scope. This is a useful local-LAN access gate, not production authentication:
- discovery and HTTP control/data are still plaintext;
- a device able to monitor or spoof traffic on the same LAN may be able to interfere;
- the shared discovery nonce is not a secret.

Before a public/release build, add a user-visible pairing confirmation/code with rate limits and authenticated encrypted transport (TLS with correctly validated identity or an authenticated secure session). Do not claim the current prototype is secure on an untrusted network.

## 7. Resume and integrity rules

The receiver should persist transfer ID, expected size, SHA-256, safe destination, and verified byte offset. A resume request must validate the returned range and actual partial-file length. The final complete file must be hashed again; a mismatched digest results in FAILED_INTEGRITY and must not be offered for installation.

## 8. Compatibility policy

Minor additions may be optional. A breaking change requires a new major protocol version. Both apps must show a clear incompatibility message rather than attempting an unsafe downgrade.
