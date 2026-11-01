# AMEGA architecture

## 1. Product boundary

AMEGA consists of two independently installed programs that implement one versioned protocol.

| Component | Responsibility | Must not do |
|---|---|---|
| Windows sender | Select a file, advertise availability, authorize a receiver, serve byte ranges, keep transfer state | Install or modify anything on the PS4 |
| PS4 receiver | Discover senders, show the user an offer, download to a safe staging location, verify, expose install actions | Accept unsolicited transfers or silently install |
| Shared protocol | Discovery, pairing, offer metadata, transfer state, integrity data, errors | Depend on cloud services |

The network protocol is transport-oriented. The UI and the PS4 installation adapter remain separate so a change in GoldHEN behavior cannot break ordinary file transfer.

## 2. Network topology

The Windows application is the temporary HTTP(S) file host. It announces its presence on the local subnet with a small UDP beacon. The PS4 application listens for those beacons and displays available PCs.

The beacon contains only discovery metadata: protocol version, sender instance ID, listening port, and a short-lived nonce. It must not contain a file path, package contents, or authentication secret.

After a user chooses a PC and approves a pairing code, the PS4 receiver contacts the PC over HTTP. File bytes are fetched by the PS4, which makes resume support and receiver-side verification straightforward.

Discovery is best-effort: broadcast may be blocked by guest Wi-Fi isolation, VLANs, or router settings. A manual IP/port option is a fallback, not the primary workflow.

## 3. Transfer lifecycle

`IDLE → DISCOVERED → PAIRED → OFFERED → ACCEPTED → TRANSFERRING → VERIFYING → COMPLETE`

Terminal or recoverable states:
- `DECLINED` — user rejected the offer.
- `CANCELLED` — user stopped the transfer.
- `PAUSED` — user paused or the connection was lost.
- `FAILED` — a non-recoverable error occurred.
- `NEEDS_REPAIR` — partial data exists but does not match the expected metadata.

The sender must not report a transfer as complete merely because all bytes were sent. Completion requires receiver-side size and SHA-256 verification plus a final acknowledgement.

## 4. File staging

The receiver writes to an application-controlled staging directory, never directly over an existing package. Partial downloads use a temporary filename and persisted transfer metadata. After successful verification, the receiver atomically renames the temporary file to its final received name where the filesystem permits.

A repeated transfer ID must not overwrite a completed package. Filenames are display metadata only; they are never trusted as paths. The receiver generates the destination path and rejects traversal characters and invalid names.

## 5. Installation boundary

Receiving a PKG and installing it are distinct user actions. The installation adapter is capability-driven:
- detect whether a supported GoldHEN remote package installation mechanism is present;
- report the detected capability and its limitations;
- request installation only after an explicit user action;
- show the result returned by the actual installation mechanism.

If the capability cannot be verified, AMEGA may still store and verify the PKG, but must disable the install action and explain why. AMEGA must not emulate undocumented endpoints or report success without a positive result.

## 6. First vertical slice

Implement and test in this order:
1. Shared protocol types and validation.
2. Windows discovery beacon and a minimal HTTP file host.
3. PS4 discovery screen and explicit accept/decline.
4. Resumable ranged transfer.
5. SHA-256 verification and persisted state.
6. UI polish, queueing, history, and cancellation.
7. GoldHEN capability detection and installation adapter.

This keeps the riskiest platform-specific work out of the core transfer protocol.
