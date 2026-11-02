#pragma once

#include <atomic>
#include <cstdint>

enum AmegaDownloadState : int {
    AMEGA_DOWNLOAD_IDLE = 0,
    AMEGA_DOWNLOAD_RUNNING = 1,
    AMEGA_DOWNLOAD_VERIFYING = 2,
    AMEGA_DOWNLOAD_PAUSED = 3,
    AMEGA_DOWNLOAD_COMPLETE = 4,
    AMEGA_DOWNLOAD_FAILED = 5
};

struct AmegaDownloadRequest {
    char ip[16];
    int port;
    char transferId[48];
    char accessToken[80];
    uint64_t fileSize;
    char sha256[80];
};

struct AmegaDownloadRuntime {
    std::atomic<int> state{AMEGA_DOWNLOAD_IDLE};
    std::atomic<uint64_t> bytes{0};
    std::atomic<int> errorCode{0};
    std::atomic<bool> cancel{false};
    char finalPath[512] = {};
};

// Downloads to /data/AMEGA/Download using a transfer-ID-based temporary file.
// Existing partial bytes are resumed only when the saved metadata matches this
// offer. The access token is never written to persistent storage.
bool AmegaReceivePackage(const AmegaDownloadRequest& request,
    AmegaDownloadRuntime& runtime);
