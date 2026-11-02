#include "downloader.h"
#include "sha256.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
constexpr const char* kDownloadDirectory = "/data/AMEGA/Download";
constexpr uint64_t kFreeSpaceReserve = 16ull * 1024ull * 1024ull;
constexpr size_t kNetworkBufferSize = 256u * 1024u;
constexpr size_t kHeaderBufferSize = 8192u;

enum class RangeResult {
    Complete,
    Paused,
    Failed
};

static bool isHexString(const char* value, size_t expectedLength)
{
    if (!value || std::strlen(value) != expectedLength) return false;
    for (size_t i = 0; i < expectedLength; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) return false;
    return true;
}

static bool isTransferId(const char* value)
{
    if (!value || std::strlen(value) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

static bool ensureDirectory(const char* path)
{
    if (mkdir(path, 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat info = {};
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool writeAll(int fd, const void* data, size_t length)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < length) {
        ssize_t count = write(fd, bytes + written, length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += static_cast<size_t>(count);
    }
    return true;
}

static bool socketSendAll(int sock, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(sock, data + sent, length - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

static bool writeMetadata(const char* metadataPath, const char* transferId,
    uint64_t fileSize, const char* sha256)
{
    char temporaryPath[600] = {};
    if (std::snprintf(temporaryPath, sizeof(temporaryPath), "%s.tmp", metadataPath) >=
        static_cast<int>(sizeof(temporaryPath)))
        return false;

    char metadata[256] = {};
    int length = std::snprintf(metadata, sizeof(metadata),
        "AMEGA-RESUME-1\n%s\n%llu\n%s\n",
        transferId, static_cast<unsigned long long>(fileSize), sha256);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(metadata))
        return false;

    int fd = open(temporaryPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = writeAll(fd, metadata, static_cast<size_t>(length));
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) {
        unlink(temporaryPath);
        return false;
    }
    if (rename(temporaryPath, metadataPath) != 0) {
        unlink(temporaryPath);
        return false;
    }
    return true;
}

static bool metadataMatches(const char* metadataPath, const char* transferId,
    uint64_t fileSize, const char* sha256)
{
    int fd = open(metadataPath, O_RDONLY);
    if (fd < 0) return false;
    char current[256] = {};
    ssize_t count = read(fd, current, sizeof(current) - 1);
    close(fd);
    if (count <= 0) return false;

    char expected[256] = {};
    int length = std::snprintf(expected, sizeof(expected),
        "AMEGA-RESUME-1\n%s\n%llu\n%s\n",
        transferId, static_cast<unsigned long long>(fileSize), sha256);
    return length > 0 && static_cast<size_t>(length) < sizeof(expected) &&
        static_cast<size_t>(count) == static_cast<size_t>(length) &&
        std::memcmp(current, expected, static_cast<size_t>(length)) == 0;
}

static bool getFileSize(const char* path, uint64_t& size)
{
    struct stat info = {};
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return false;
    size = static_cast<uint64_t>(info.st_size);
    return true;
}

static bool hasEnoughSpace(uint64_t remaining)
{
    if (remaining == 0) return true;
    struct statvfs info = {};
    if (statvfs(kDownloadDirectory, &info) != 0) return false;
    const uint64_t unit = info.f_frsize ? info.f_frsize : info.f_bsize;
    const uint64_t available = static_cast<uint64_t>(info.f_bavail) * unit;
    if (available < kFreeSpaceReserve) return false;
    return remaining <= available - kFreeSpaceReserve;
}

static bool headerValue(const char* header, const char* name, char* output, size_t capacity)
{
    if (!header || !name || !output || capacity == 0) return false;
    output[0] = 0;
    const size_t nameLength = std::strlen(name);
    const char* line = std::strstr(header, "\r\n");
    if (!line) return false;
    line += 2; // Skip the HTTP status line.

    while (*line) {
        const char* end = std::strstr(line, "\r\n");
        const bool hasTerminator = end != nullptr;
        if (!end) end = line + std::strlen(line);
        if (end == line) break;
        if (static_cast<size_t>(end - line) > nameLength &&
            strncasecmp(line, name, nameLength) == 0 &&
            line[nameLength] == ':') {
            const char* value = line + nameLength + 1;
            while (value < end && (*value == ' ' || *value == '\t')) ++value;
            size_t length = static_cast<size_t>(end - value);
            while (length > 0 && (value[length - 1] == ' ' || value[length - 1] == '\t'))
                --length;
            if (length + 1 > capacity) return false;
            std::memcpy(output, value, length);
            output[length] = 0;
            return true;
        }
        if (!hasTerminator) break;
        line = end + 2;
    }
    return false;
}

static bool parseContentRange(const char* value, uint64_t& start,
    uint64_t& end, uint64_t& total)
{
    if (!value || std::strncmp(value, "bytes ", 6) != 0) return false;
    unsigned long long parsedStart = 0, parsedEnd = 0, parsedTotal = 0;
    char trailing = 0;
    if (std::sscanf(value + 6, "%llu-%llu/%llu%c",
        &parsedStart, &parsedEnd, &parsedTotal, &trailing) != 3)
        return false;
    if (parsedEnd < parsedStart || parsedTotal == 0 || parsedEnd >= parsedTotal)
        return false;
    start = static_cast<uint64_t>(parsedStart);
    end = static_cast<uint64_t>(parsedEnd);
    total = static_cast<uint64_t>(parsedTotal);
    return true;
}

static void setFailure(AmegaDownloadRuntime& runtime, int error)
{
    runtime.errorCode.store(error, std::memory_order_relaxed);
    runtime.state.store(AMEGA_DOWNLOAD_FAILED, std::memory_order_release);
}

static bool hashFile(const char* path, char hexOutput[65])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    AmegaSha256 hash;
    uint8_t buffer[1024u * 1024u];
    bool ok = true;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0) break;
        if (!hash.update(buffer, static_cast<size_t>(count))) {
            ok = false;
            break;
        }
    }
    close(fd);
    if (!ok) return false;

    uint8_t digest[32] = {};
    if (!hash.finish(digest)) return false;
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        hexOutput[i * 2] = digits[digest[i] >> 4];
        hexOutput[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hexOutput[64] = 0;
    return true;
}

static RangeResult receiveOneRange(const AmegaDownloadRequest& request,
    uint64_t offset, uint64_t expectedSize, int partFd,
    AmegaDownloadRuntime& runtime, uint64_t& bytesReceived)
{
    bytesReceived = 0;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        runtime.errorCode.store(errno, std::memory_order_relaxed);
        return RangeResult::Paused;
    }

    struct timeval timeout = {};
    timeout.tv_sec = 5;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(static_cast<uint16_t>(request.port));
    if (inet_pton(AF_INET, request.ip, &remote.sin_addr) != 1 ||
        connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
        runtime.errorCode.store(errno, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Paused;
    }

    char requestText[1024] = {};
    const int requestLength = std::snprintf(requestText, sizeof(requestText),
        "GET /api/v1/file HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "Accept: application/octet-stream\r\n"
        "Accept-Encoding: identity\r\n"
        "User-Agent: AMEGA-PS4/0.2.2\r\n"
        "Range: bytes=%llu-\r\n"
        "If-Range: \"%s\"\r\n"
        "X-AMEGA-Token: %s\r\n\r\n",
        request.ip, request.port,
        static_cast<unsigned long long>(offset), request.sha256, request.accessToken);
    if (requestLength <= 0 || static_cast<size_t>(requestLength) >= sizeof(requestText) ||
        !socketSendAll(sock, requestText, static_cast<size_t>(requestLength))) {
        runtime.errorCode.store(errno, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Paused;
    }

    char header[kHeaderBufferSize] = {};
    size_t receivedHeaderBytes = 0;
    size_t headerLength = 0;
    size_t initialBodyLength = 0;
    char* separator = nullptr;
    while (receivedHeaderBytes + 1 < sizeof(header)) {
        if (runtime.cancel.load(std::memory_order_relaxed)) {
            close(sock);
            return RangeResult::Paused;
        }
        ssize_t count = recv(sock, header + receivedHeaderBytes,
            sizeof(header) - receivedHeaderBytes - 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            runtime.errorCode.store(count == 0 ? ECONNRESET : errno, std::memory_order_relaxed);
            close(sock);
            return RangeResult::Paused;
        }
        receivedHeaderBytes += static_cast<size_t>(count);
        header[receivedHeaderBytes] = 0;
        separator = std::strstr(header, "\r\n\r\n");
        if (separator) {
            headerLength = static_cast<size_t>(separator - header) + 4;
            initialBodyLength = receivedHeaderBytes - headerLength;
            break;
        }
    }

    if (!separator || headerLength == 0) {
        runtime.errorCode.store(1001, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }

    *separator = 0;
    int status = 0;
    if (std::sscanf(header, "HTTP/%*s %d", &status) != 1) {
        runtime.errorCode.store(1002, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }
    if (status == 401 || status == 403 || status == 404 || status == 409 || status == 410) {
        runtime.errorCode.store(status, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }
    if (status != 206) {
        runtime.errorCode.store(status, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }

    char contentLengthText[64] = {};
    char contentRangeText[160] = {};
    if (!headerValue(header, "Content-Length", contentLengthText, sizeof(contentLengthText)) ||
        !headerValue(header, "Content-Range", contentRangeText, sizeof(contentRangeText))) {
        runtime.errorCode.store(1003, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }

    char* lengthEnd = nullptr;
    unsigned long long parsedLength = std::strtoull(contentLengthText, &lengthEnd, 10);
    if (lengthEnd == contentLengthText || *lengthEnd != 0) {
        runtime.errorCode.store(1004, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }

    uint64_t rangeStart = 0, rangeEnd = 0, rangeTotal = 0;
    if (!parseContentRange(contentRangeText, rangeStart, rangeEnd, rangeTotal) ||
        rangeStart != offset || rangeTotal != expectedSize ||
        rangeEnd < rangeStart ||
        static_cast<uint64_t>(parsedLength) != rangeEnd - rangeStart + 1 ||
        initialBodyLength > static_cast<uint64_t>(parsedLength)) {
        runtime.errorCode.store(1005, std::memory_order_relaxed);
        close(sock);
        return RangeResult::Failed;
    }

    uint64_t remaining = static_cast<uint64_t>(parsedLength);
    uint8_t buffer[kNetworkBufferSize];
    uint64_t sinceSync = 0;

    auto appendBytes = [&](const void* data, size_t length) -> bool {
        if (length == 0) return true;
        if (runtime.cancel.load(std::memory_order_relaxed)) return false;
        if (!writeAll(partFd, data, length)) {
            runtime.errorCode.store(errno, std::memory_order_relaxed);
            return false;
        }
        bytesReceived += length;
        remaining -= length;
        sinceSync += length;
        runtime.bytes.store(offset + bytesReceived, std::memory_order_relaxed);
        if (sinceSync >= 16ull * 1024ull * 1024ull) {
            if (fsync(partFd) != 0) {
                runtime.errorCode.store(errno, std::memory_order_relaxed);
                return false;
            }
            sinceSync = 0;
        }
        return true;
    };

    const char* firstBody = separator + 4;
    if (initialBodyLength > 0 &&
        !appendBytes(firstBody, initialBodyLength)) {
        fsync(partFd);
        close(sock);
        return runtime.cancel.load(std::memory_order_relaxed)
            ? RangeResult::Paused : RangeResult::Paused;
    }

    while (remaining > 0) {
        if (runtime.cancel.load(std::memory_order_relaxed)) {
            fsync(partFd);
            close(sock);
            return RangeResult::Paused;
        }
        const size_t wanted = remaining < sizeof(buffer)
            ? static_cast<size_t>(remaining) : sizeof(buffer);
        ssize_t count = recv(sock, buffer, wanted, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            runtime.errorCode.store(count == 0 ? ECONNRESET : errno, std::memory_order_relaxed);
            fsync(partFd);
            close(sock);
            return RangeResult::Paused;
        }
        if (!appendBytes(buffer, static_cast<size_t>(count))) {
            fsync(partFd);
            close(sock);
            return RangeResult::Paused;
        }
    }

    fsync(partFd);
    close(sock);
    return RangeResult::Complete;
}

} // namespace

bool AmegaReceivePackage(const AmegaDownloadRequest& request,
    AmegaDownloadRuntime& runtime)
{
    runtime.errorCode.store(0, std::memory_order_relaxed);
    runtime.bytes.store(0, std::memory_order_relaxed);
    runtime.finalPath[0] = 0;
    runtime.state.store(AMEGA_DOWNLOAD_RUNNING, std::memory_order_release);

    if (!isTransferId(request.transferId) ||
        !isHexString(request.accessToken, 64) ||
        !isHexString(request.sha256, 64) ||
        request.fileSize == 0 || request.port < 1024 || request.port > 65535) {
        setFailure(runtime, 1);
        return false;
    }

    char partPath[600] = {};
    char metadataPath[600] = {};
    char finalPath[600] = {};
    if (std::snprintf(partPath, sizeof(partPath), "%s/%s.part",
            kDownloadDirectory, request.transferId) >= static_cast<int>(sizeof(partPath)) ||
        std::snprintf(metadataPath, sizeof(metadataPath), "%s/%s.resume",
            kDownloadDirectory, request.transferId) >= static_cast<int>(sizeof(metadataPath)) ||
        std::snprintf(finalPath, sizeof(finalPath), "%s/%s.pkg",
            kDownloadDirectory, request.transferId) >= static_cast<int>(sizeof(finalPath))) {
        setFailure(runtime, 2);
        return false;
    }

    if (!ensureDirectory("/data/AMEGA") || !ensureDirectory(kDownloadDirectory)) {
        setFailure(runtime, errno ? errno : 2);
        return false;
    }

    // If a previous attempt completed the rename but the process exited
    // before showing success, verify that final file again.
    uint64_t existingFinalSize = 0;
    if (getFileSize(finalPath, existingFinalSize)) {
        if (existingFinalSize == request.fileSize) {
            runtime.state.store(AMEGA_DOWNLOAD_VERIFYING, std::memory_order_release);
            char existingDigest[65] = {};
            if (hashFile(finalPath, existingDigest) &&
                strcasecmp(existingDigest, request.sha256) == 0) {
                std::snprintf(runtime.finalPath, sizeof(runtime.finalPath), "%s", finalPath);
                runtime.bytes.store(request.fileSize, std::memory_order_relaxed);
                runtime.state.store(AMEGA_DOWNLOAD_COMPLETE, std::memory_order_release);
                return true;
            }
        }
        unlink(finalPath);
    }

    if (!metadataMatches(metadataPath, request.transferId, request.fileSize, request.sha256)) {
        unlink(partPath);
        unlink(metadataPath);
        if (!writeMetadata(metadataPath, request.transferId, request.fileSize, request.sha256)) {
            setFailure(runtime, errno ? errno : 3);
            return false;
        }
    }

    uint64_t offset = 0;
    if (getFileSize(partPath, offset)) {
        if (offset > request.fileSize) {
            unlink(partPath);
            offset = 0;
        }
    } else {
        offset = 0;
    }

    runtime.bytes.store(offset, std::memory_order_relaxed);
    if (!hasEnoughSpace(request.fileSize - offset)) {
        setFailure(runtime, 4);
        return false;
    }

    int partFd = open(partPath, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (partFd < 0) {
        setFailure(runtime, errno ? errno : 5);
        return false;
    }

    while (offset < request.fileSize) {
        if (runtime.cancel.load(std::memory_order_relaxed)) {
            fsync(partFd);
            close(partFd);
            runtime.state.store(AMEGA_DOWNLOAD_PAUSED, std::memory_order_release);
            return false;
        }

        uint64_t received = 0;
        RangeResult result = receiveOneRange(request, offset, request.fileSize,
            partFd, runtime, received);
        offset += received;

        if (result == RangeResult::Failed) {
            fsync(partFd);
            close(partFd);
            setFailure(runtime, runtime.errorCode.load(std::memory_order_relaxed));
            return false;
        }
        if (result == RangeResult::Paused) {
            fsync(partFd);
            close(partFd);
            runtime.state.store(AMEGA_DOWNLOAD_PAUSED, std::memory_order_release);
            return false;
        }
        if (received == 0) {
            fsync(partFd);
            close(partFd);
            setFailure(runtime, 1006);
            return false;
        }
    }

    if (fsync(partFd) != 0) {
        const int error = errno;
        close(partFd);
        setFailure(runtime, error ? error : 6);
        return false;
    }
    close(partFd);

    runtime.state.store(AMEGA_DOWNLOAD_VERIFYING, std::memory_order_release);
    char actualDigest[65] = {};
    if (!hashFile(partPath, actualDigest)) {
        setFailure(runtime, errno ? errno : 7);
        return false;
    }
    if (strcasecmp(actualDigest, request.sha256) != 0) {
        unlink(partPath);
        unlink(metadataPath);
        setFailure(runtime, 8);
        return false;
    }

    if (rename(partPath, finalPath) != 0) {
        setFailure(runtime, errno ? errno : 9);
        return false;
    }
    unlink(metadataPath);

    int directoryFd = open(kDownloadDirectory, O_RDONLY);
    if (directoryFd >= 0) {
        fsync(directoryFd);
        close(directoryFd);
    }

    std::snprintf(runtime.finalPath, sizeof(runtime.finalPath), "%s", finalPath);
    runtime.bytes.store(request.fileSize, std::memory_order_relaxed);
    runtime.state.store(AMEGA_DOWNLOAD_COMPLETE, std::memory_order_release);
    return true;
}
