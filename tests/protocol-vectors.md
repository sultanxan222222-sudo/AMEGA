# Protocol test vectors

These vectors are implementation-independent checks for the first protocol implementation.

## Discovery beacon: valid shape

```json
{
  "protocol": "amega",
  "major": 1,
  "type": "sender_announcement",
  "sender_id": "0c4e2d2f-3f43-4bd7-b65e-48ee7e8bbcc5",
  "instance_nonce": "7d4a8b6c0f1e2d3c4b5a69788796a5b4",
  "http_port": 38471,
  "display_name": "AMega-PC"
}
```

Expected: accept as an unauthenticated discovery announcement only. Do not grant access.

## Transfer offer: valid shape

```json
{
  "protocol": "amega",
  "major": 1,
  "type": "transfer_offer",
  "transfer_id": "a1b2c3d4-e5f6-47a8-9b0c-1d2e3f4a5b6c",
  "sender_id": "0c4e2d2f-3f43-4bd7-b65e-48ee7e8bbcc5",
  "file_name": "sample.pkg",
  "file_size": 1048576,
  "file_kind": "pkg",
  "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "created_utc": "2026-10-31T12:00:00Z"
}
```

Expected: accept only after schema validation and session authorization. This vector does not represent a real downloadable file.
