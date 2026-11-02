#include "../src/ps4/receiver/downloader.h"
#include "../src/ps4/receiver/sha256.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

static std::string digestFile(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return {};
    AmegaSha256 hash;
    uint8_t buffer[64 * 1024];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) { close(fd); return {}; }
        if (count == 0) break;
        if (!hash.update(buffer, static_cast<size_t>(count))) {
            close(fd);
            return {};
        }
    }
    close(fd);

    std::array<uint8_t, 32> digest{};
    if (!hash.finish(digest.data())) return {};
    static const char* digits = "0123456789abcdef";
    std::string result;
    for (uint8_t byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: test_downloader <port> <size> <sha256>\n");
        return 2;
    }

    AmegaDownloadRequest request{};
    std::snprintf(request.ip, sizeof(request.ip), "127.0.0.1");
    request.port = std::atoi(argv[1]);
    std::snprintf(request.transferId, sizeof(request.transferId),
        "123e4567-e89b-42d3-a456-426614174000");
    std::snprintf(request.accessToken, sizeof(request.accessToken),
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    request.fileSize = std::strtoull(argv[2], nullptr, 10);
    std::snprintf(request.sha256, sizeof(request.sha256), "%s", argv[3]);

    AmegaDownloadRuntime runtime;
    const bool firstResult = AmegaReceivePackage(request, runtime);
    const int firstState = runtime.state.load();
    const uint64_t savedOffset = runtime.bytes.load();

    if (firstResult || firstState != AMEGA_DOWNLOAD_PAUSED ||
        savedOffset == 0 || savedOffset >= request.fileSize) {
        std::fprintf(stderr,
            "first pass must pause with partial bytes: result=%d state=%d bytes=%llu\n",
            firstResult ? 1 : 0, firstState,
            static_cast<unsigned long long>(savedOffset));
        return 3;
    }

    runtime.cancel.store(false);
    const bool secondResult = AmegaReceivePackage(request, runtime);
    if (!secondResult || runtime.state.load() != AMEGA_DOWNLOAD_COMPLETE) {
        std::fprintf(stderr, "resume failed: state=%d error=%d bytes=%llu\n",
            runtime.state.load(), runtime.errorCode.load(),
            static_cast<unsigned long long>(runtime.bytes.load()));
        return 4;
    }

    if (runtime.finalPath[0] == 0 ||
        digestFile(runtime.finalPath) != request.sha256) {
        std::fprintf(stderr, "final staged file digest did not match\n");
        return 5;
    }

    std::puts("AMEGA resumable Range download and receiver SHA-256 test passed");
    return 0;
}
