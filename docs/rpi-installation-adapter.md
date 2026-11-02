# AMEGA Standalone RPI adapter

Status: development implementation in the Windows branch. Not validated on physical PS4 hardware yet.

## What this adapter is

AMEGA has two different PS4-side paths:

1. **Native AMEGA receiver:** the AMEGA homebrew receives the PKG into `/data/AMEGA/Download`, supports resume, and verifies SHA-256.
2. **Standalone RPI installation:** a separate Remote Package Installer homebrew on the PS4 receives an install command from AMEGA for Windows and downloads the selected PKG from the Windows HTTP server.

The optional RPI path does not replace or disable native AMEGA receiving. It is not the GoldHEN built-in Remote Package Install service, FTP, BinLoader or AppInstUtil.

## PS4 prerequisites

- A compatible HEN environment.
- Standalone Remote Package Installer homebrew installed separately (commonly title ID `FLTZ00003`).
- The RPI application open and in the PS4 foreground while AMEGA checks and submits the task. Its API is documented on TCP 12800. If the application is suspended, the API may stop answering.
- PC and PS4 on the same trusted private LAN. Do not use guest Wi-Fi or expose these HTTP services to the Internet.

## Windows flow

1. Start AMEGA LAN discovery and select the intended PS4.
2. Press **Проверить RPI**. AMEGA sends `POST /api/is_exists` with `{"title_id":"CUSA00000"}`. This is an application-level API probe; a returned `status: fail` still proves that the endpoint answered. A TCP connect alone is not considered proof of readiness.
3. Select a PKG and press **Установить через RPI**.
4. AMEGA asks for separate confirmation, checks the PKG magic `7F 43 4E 54`, and recomputes the whole-file SHA-256 to detect a file change since selection.
5. The Windows server exposes a temporary URL:
   `http://<PC-private-IPv4>:38471/api/v1/rpi-file`
6. The endpoint is restricted to the selected console IPv4, serves only that selected PKG, supports HTTP Range and ETag, and expires after 12 hours or explicit revocation.
7. AMEGA submits `POST /api/install` with `type: direct` and the one package URL in `packages`.
8. When RPI returns a task ID, AMEGA displays it. The UI separately tracks unique source byte coverage using the actual HTTP stream reads; overlapping/retried byte ranges are counted once.

## Timeouts and ambiguous acceptance

RPI may read PKG metadata synchronously before returning from `/api/install`. The adapter allows up to 150 seconds for the request.

If the connection times out or the response is `status: success` without a task ID, AMEGA treats acceptance as ambiguous and deliberately leaves the HTTP source open. The request may have reached the console even though the PC did not receive a complete reply. The UI tells the user to check the PS4 before submitting the same PKG again.

Do not close AMEGA or revoke the temporary source while RPI may still be downloading. The UI asks for confirmation before stopping it.

## Progress semantics

The RPI API may stop answering progress polls while it is servicing a large file download. The current AMEGA branch therefore reports only the unique byte intervals read by the Windows HTTP response stream:

- 0% means no PKG byte interval has yet been read.
- Intermediate percentages describe unique source-file coverage, not installation work.
- 100% means every file byte has been read by the HTTP response stream at least once.
- 100% does **not** mean the title is fully installed. RPI may still be verifying, unpacking or installing on the PS4.

When RPI returns a task ID, AMEGA now polls `POST /api/get_task_progress` at a conservative interval with a short timeout. It reads the console's `transferred_total`, `length_total`, `preparing_percent`, `local_copy_percent`, and error result. If the single-threaded RPI API is busy serving the PKG and does not answer, AMEGA falls back to unique HTTP source coverage and retries later. It reports completion only when RPI reports the full byte count and `local_copy_percent` reaches 100%. This is an API-reported task completion state, not a physical-console validation; the user should still check the PS4 screen.

## Error handling

RPI versions may emit responses containing unquoted hexadecimal integers such as `0x80990085`, which makes the complete response invalid JSON. AMEGA extracts the fixed status and known error-code fields without treating the body as valid JSON.

Known messages include:
- `0x80990085`: insufficient PS4 free space.
- `0x80990088`: an install task already exists for the content.
- `0x8099000E`: content is already installed.
- `0x80020016`: invalid parameter or incompatible PKG.

Unknown codes are shown rather than silently converted into a generic success.

## Security boundary

The temporary RPI URL is plaintext HTTP and uses the selected PS4 source IP as its access restriction. It is not cryptographic authentication and can be spoofed by an active attacker on an untrusted LAN. Keep it on a private trusted network only. Pairing and authenticated encrypted transport remain release requirements.

The Windows source PKG is never deleted automatically. The RPI adapter does not silently install: the user must confirm each request.
