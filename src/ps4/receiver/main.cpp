#include <SDL2/SDL.h>
#include <orbis/libkernel.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Sysmodule.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "font_ru.h"
#include "downloader.h"

// OpenOrbis toolchain images in the wild do not all ship the newest
// OrbisKernelSwVersion typedef. Keep the documented 0x28 ABI local so
// this receiver can still build against older toolchain snapshots.
#ifndef AMEGA_ORBIS_KERNEL_SW_VERSION_DEFINED
#define AMEGA_ORBIS_KERNEL_SW_VERSION_DEFINED
typedef struct {
    size_t Size;
    char VersionString[0x1C];
    uint32_t Version;
} OrbisKernelSwVersion;
#endif

static const int SENDER_DISCOVERY_PORT = 38470;
static const int RECEIVER_DISCOVERY_PORT = 38472;
static const int HTTP_PORT = 38471;
static const int MAX_SENDERS = 8;

struct SenderEntry {
    char id[48];
    char name[64];
    char ip[INET_ADDRSTRLEN];
    char instanceNonce[40];
    int httpPort;
    Uint32 lastSeen;
    bool used;
};

static SenderEntry g_senders[MAX_SENDERS];
static int g_selected = -1;
static int g_socket = -1;
static int g_netPoolId = -1;
static bool g_netStackReady = false;
static bool g_netCtlReady = false;
static char g_receiverId[40] = {};
static char g_nonce[40] = {};
static char g_status[128] = "ОЖИДАНИЕ ПОДКЛЮЧЕНИЯ С ПК";
static char g_localIp[64] = "НЕ ОПРЕДЕЛЁН";
static char g_firmwareVersion[48] = "НЕ ОПРЕДЕЛЕНА";
static SDL_Joystick* g_controller = nullptr;
static SDL_GameController* g_gameController = nullptr;
static Uint32 g_lastNetworkRefresh = 0;

enum AmegaScreen {
    SCREEN_DISCOVERY = 0,
    SCREEN_OFFER = 1,
    SCREEN_ACCEPTED = 2,
    SCREEN_DOWNLOADING = 3,
    SCREEN_VERIFIED = 4
};
static AmegaScreen g_screen = SCREEN_DISCOVERY;
static char g_offerTransferId[48] = {};
static char g_offerFileName[224] = {};
static char g_offerSha256[80] = {};
static uint64_t g_offerFileSize = 0;
static char g_accessToken[80] = {};
static AmegaDownloadRuntime g_downloadRuntime;
static SDL_Thread* g_downloadThread = nullptr;

static void writeStartupLog(const char* message);
static bool controllerConnected();

static bool randomBytes(unsigned char* out, size_t size)
{
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t read = fread(out, 1, size, f);
    fclose(f);
    return read == size;
}

static void makeUuid(char out[40])
{
    unsigned char b[16] = {};
    if (!randomBytes(b, sizeof(b))) {
        srand((unsigned int)time(nullptr));
        for (int i = 0; i < 16; ++i) b[i] = (unsigned char)(rand() & 0xff);
    }
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 40,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
        b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}

static void makeNonce(char out[40])
{
    unsigned char b[16] = {};
    if (!randomBytes(b, sizeof(b))) {
        snprintf(out, 40, "%032llx", (unsigned long long)time(nullptr));
        return;
    }
    for (int i = 0; i < 16; ++i) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static void loadOrCreateReceiverId()
{
    const char* path = "/data/amega_receiver_id.txt";
    FILE* f = fopen(path, "rb");
    if (f) {
        if (fgets(g_receiverId, sizeof(g_receiverId), f)) {
            g_receiverId[strcspn(g_receiverId, "\r\n")] = 0;
        }
        fclose(f);
    }
    if (strlen(g_receiverId) < 36) {
        makeUuid(g_receiverId);
        f = fopen(path, "wb");
        if (f) {
            fputs(g_receiverId, f);
            fclose(f);
        }
    }
    makeNonce(g_nonce);
}

static int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool appendUtf8(char* out, size_t cap, size_t& used, uint32_t cp)
{
    unsigned char bytes[4];
    size_t count = 0;
    if (cp <= 0x7f) {
        bytes[0] = (unsigned char)cp; count = 1;
    } else if (cp <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3f)); count = 2;
    } else if (cp <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3f)); count = 3;
    } else if (cp <= 0x10ffff) {
        bytes[0] = (unsigned char)(0xf0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3f)); count = 4;
    } else return false;

    if (used + count + 1 > cap) return false;
    memcpy(out + used, bytes, count);
    used += count;
    return true;
}

static bool parseHex4(const char*& p, uint32_t& value)
{
    value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hexDigitValue(*p++);
        if (digit < 0) return false;
        value = (value << 4) | (uint32_t)digit;
    }
    return true;
}

static bool getStringField(const char* json, const char* key, char* out, size_t cap)
{
    if (!json || !key || !out || cap == 0) return false;
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p || !(p = strchr(p, ':'))) return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != '"') return false;

    size_t used = 0;
    while (*p) {
        if (*p == '"') {
            out[used] = 0;
            return true;
        }
        if ((unsigned char)*p < 0x20) return false;

        if (*p != '\\') {
            if (used + 2 > cap) return false;
            out[used++] = *p++;
            continue;
        }

        ++p;
        if (!*p) return false;
        char esc = *p++;
        uint32_t cp = 0;
        switch (esc) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = ' '; break;
            case 'f': cp = ' '; break;
            case 'n': cp = ' '; break;
            case 'r': cp = ' '; break;
            case 't': cp = ' '; break;
            case 'u':
                if (!parseHex4(p, cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (p[0] != '\\' || p[1] != 'u') return false;
                    p += 2;
                    uint32_t low = 0;
                    if (!parseHex4(p, low) || low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    return false;
                }
                break;
            default:
                return false;
        }
        if (!appendUtf8(out, cap, used, cp)) return false;
    }
    return false;
}

static int getIntField(const char* json, const char* key)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p || !(p = strchr(p, ':'))) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    return atoi(p);
}

static int64_t getLongField(const char* json, const char* key)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p || !(p = strchr(p, ':'))) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') return -1;
    char* end = nullptr;
    long long value = strtoll(p, &end, 10);
    return end == p || value < 0 ? -1 : (int64_t)value;
}

static bool sendAll(int sock, const char* data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(sock, data + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool httpJsonRequest(const char* ip, int port, const char* method,
    const char* path, const char* body, const char* token,
    int* statusOut, char* bodyOut, size_t bodyCap)
{
    if (!ip || !method || !path || !statusOut || !bodyOut || bodyCap == 0) return false;
    bodyOut[0] = 0;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval timeout = {};
    timeout.tv_sec = 4;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in remote = {};
    remote.sin_family = AF_INET;
    remote.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &remote.sin_addr) != 1 ||
        connect(sock, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0) {
        close(sock);
        return false;
    }

    const size_t bodyLength = body ? strlen(body) : 0;
    if (bodyLength > 1024) {
        close(sock);
        return false;
    }

    char request[2048];
    int requestLength = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"
        "Accept: application/json\r\nUser-Agent: AMEGA-PS4/0.2.2\r\n",
        method, path, ip, port);
    if (requestLength < 0 || (size_t)requestLength >= sizeof(request)) {
        close(sock);
        return false;
    }

    size_t used = (size_t)requestLength;
    if (body) {
        int extra = snprintf(request + used, sizeof(request) - used,
            "Content-Type: application/json\r\nContent-Length: %zu\r\n", bodyLength);
        if (extra < 0 || (size_t)extra >= sizeof(request) - used) {
            close(sock);
            return false;
        }
        used += (size_t)extra;
    }
    if (token) {
        int extra = snprintf(request + used, sizeof(request) - used,
            "X-AMEGA-Token: %s\r\n", token);
        if (extra < 0 || (size_t)extra >= sizeof(request) - used) {
            close(sock);
            return false;
        }
        used += (size_t)extra;
    }
    int tail = snprintf(request + used, sizeof(request) - used, "\r\n");
    if (tail < 0 || (size_t)tail >= sizeof(request) - used) {
        close(sock);
        return false;
    }
    requestLength = (int)(used + (size_t)tail);

    bool sent = sendAll(sock, request, (size_t)requestLength);
    if (sent && bodyLength > 0) sent = sendAll(sock, body, bodyLength);
    if (!sent) {
        close(sock);
        return false;
    }

    char raw[8192] = {};
    size_t total = 0;
    while (total + 1 < sizeof(raw)) {
        ssize_t n = recv(sock, raw + total, sizeof(raw) - total - 1, 0);
        if (n == 0) break;
        if (n < 0) {
            close(sock);
            return false;
        }
        total += (size_t)n;
    }
    close(sock);
    raw[total] = 0;

    char* headerEnd = strstr(raw, "\r\n\r\n");
    if (!headerEnd) return false;
    int status = 0;
    if (sscanf(raw, "HTTP/%*s %d", &status) != 1) return false;
    *statusOut = status;

    const char* responseBody = headerEnd + 4;
    size_t responseLength = total - (size_t)(responseBody - raw);
    if (responseLength + 1 > bodyCap) return false;

    // These small JSON endpoints must be length-delimited. Reject a
    // truncated body or an unexpected chunked response before parsing JSON.
    char contentLengthText[64] = {};
    bool hasContentLength = false;
    const char* line = std::strstr(raw, "\r\n");
    if (line) line += 2;
    while (line && line < headerEnd) {
        const char* end = std::strstr(line, "\r\n");
        if (!end || end > headerEnd) end = headerEnd;
        if (static_cast<size_t>(end - line) >= 15 &&
            strncasecmp(line, "Content-Length:", 15) == 0) {
            const char* value = line + 15;
            while (value < end && (*value == ' ' || *value == '\t')) ++value;
            size_t length = static_cast<size_t>(end - value);
            if (length == 0 || length >= sizeof(contentLengthText)) return false;
            std::memcpy(contentLengthText, value, length);
            contentLengthText[length] = 0;
            hasContentLength = true;
        }
        if (static_cast<size_t>(end - line) >= 18 &&
            strncasecmp(line, "Transfer-Encoding:", 18) == 0)
            return false;
        if (end == headerEnd) break;
        line = end + 2;
    }

    if (!hasContentLength) return false;
    char* lengthEnd = nullptr;
    unsigned long long expectedLength = std::strtoull(contentLengthText, &lengthEnd, 10);
    if (lengthEnd == contentLengthText || *lengthEnd != 0 ||
        expectedLength != responseLength)
        return false;

    memcpy(bodyOut, responseBody, responseLength);
    bodyOut[responseLength] = 0;
    return true;
}

static bool isValidTransferId(const char* value)
{
    if (!value || std::strlen(value) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool fetchSelectedOffer()
{
    if (g_selected < 0 || g_selected >= MAX_SENDERS || !g_senders[g_selected].used)
        return false;

    char response[4096] = {};
    int status = 0;
    SenderEntry& pc = g_senders[g_selected];
    if (!httpJsonRequest(pc.ip, pc.httpPort, "GET", "/api/v1/offer",
        nullptr, nullptr, &status, response, sizeof(response))) {
        snprintf(g_status, sizeof(g_status), "НЕТ ОТВЕТА ОТ ПК");
        return false;
    }
    if (status != 200) {
        snprintf(g_status, sizeof(g_status), status == 409
            ? "НА ПК ЕЩЁ НЕ НАЖАЛИ ОТПРАВИТЬ"
            : "ПК ОТКЛОНИЛ ЗАПРОС ПРЕДЛОЖЕНИЯ");
        return false;
    }

    char transferId[48] = {}, fileName[224] = {}, sha256[80] = {};
    char senderId[48] = {}, fileKind[32] = {};
    char protocol[24] = {}, offerType[40] = {};
    int64_t fileSize = getLongField(response, "file_size");
    if (!getStringField(response, "protocol", protocol, sizeof(protocol)) ||
        !getStringField(response, "type", offerType, sizeof(offerType)) ||
        std::strcmp(protocol, "amega") != 0 ||
        std::strcmp(offerType, "transfer_offer") != 0 ||
        getIntField(response, "major") != 1 ||
        !getStringField(response, "sender_id", senderId, sizeof(senderId)) ||
        !getStringField(response, "transfer_id", transferId, sizeof(transferId)) ||
        !getStringField(response, "file_name", fileName, sizeof(fileName)) ||
        !getStringField(response, "file_kind", fileKind, sizeof(fileKind)) ||
        !getStringField(response, "sha256", sha256, sizeof(sha256)) ||
        strcmp(senderId, pc.id) != 0 ||
        !isValidTransferId(transferId) ||
        std::strcmp(fileKind, "pkg") != 0 ||
        fileName[0] == 0 ||
        fileSize <= 0 || std::strlen(sha256) != 64) {
        snprintf(g_status, sizeof(g_status), "ОШИБКА ДАННЫХ ПРЕДЛОЖЕНИЯ");
        return false;
    }
    for (size_t i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)sha256[i])) {
            snprintf(g_status, sizeof(g_status), "НЕВЕРНЫЙ SHA-256 PKG");
            return false;
        }

    const bool sameTransfer = strcmp(g_offerTransferId, transferId) == 0 &&
        strcmp(g_offerSha256, sha256) == 0 &&
        g_offerFileSize == static_cast<uint64_t>(fileSize);
    snprintf(g_offerTransferId, sizeof(g_offerTransferId), "%s", transferId);
    snprintf(g_offerFileName, sizeof(g_offerFileName), "%s", fileName);
    snprintf(g_offerSha256, sizeof(g_offerSha256), "%s", sha256);
    g_offerFileSize = (uint64_t)fileSize;
    if (!sameTransfer) {
        g_downloadRuntime.bytes.store(0, std::memory_order_relaxed);
        g_downloadRuntime.errorCode.store(0, std::memory_order_relaxed);
        g_downloadRuntime.state.store(AMEGA_DOWNLOAD_IDLE, std::memory_order_release);
        g_downloadRuntime.finalPath[0] = 0;
    }
    g_accessToken[0] = 0;
    g_screen = SCREEN_OFFER;
    snprintf(g_status, sizeof(g_status), "ПРЕДЛОЖЕНИЕ ПОЛУЧЕНО");
    writeStartupLog("Offer metadata fetched from selected PC");
    return true;
}

static bool acceptSelectedOffer()
{
    if (g_screen != SCREEN_OFFER || g_selected < 0 ||
        g_selected >= MAX_SENDERS || !g_senders[g_selected].used)
        return false;

    SenderEntry& pc = g_senders[g_selected];
    char requestBody[512];
    snprintf(requestBody, sizeof(requestBody),
        "{\"transfer_id\":\"%s\",\"receiver_id\":\"%s\","
        "\"instance_nonce\":\"%s\",\"reply_to_nonce\":\"%s\"}",
        g_offerTransferId, g_receiverId, g_nonce, pc.instanceNonce);

    char response[2048] = {};
    int status = 0;
    if (!httpJsonRequest(pc.ip, pc.httpPort, "POST", "/api/v1/accept",
        requestBody, nullptr, &status, response, sizeof(response))) {
        snprintf(g_status, sizeof(g_status), "НЕТ ОТВЕТА ПРИ ПОДТВЕРЖДЕНИИ");
        return false;
    }

    char token[80] = {};
    if (status != 200 || !strstr(response, "\"accepted\":true") ||
        !getStringField(response, "access_token", token, sizeof(token)) ||
        strlen(token) != 64) {
        snprintf(g_status, sizeof(g_status), "ПК НЕ ПРИНЯЛ ПОДТВЕРЖДЕНИЕ");
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)token[i])) {
            snprintf(g_status, sizeof(g_status), "НЕВЕРНЫЙ ТОКЕН ДОСТУПА");
            return false;
        }
    }

    snprintf(g_accessToken, sizeof(g_accessToken), "%s", token);
    g_screen = SCREEN_ACCEPTED;
    snprintf(g_status, sizeof(g_status), "ПРЕДЛОЖЕНИЕ ПРИНЯТО");
    writeStartupLog("Receiver accepted current PC offer");
    return true;
}

static int downloadWorker(void* userData)
{
    AmegaDownloadRequest* request = static_cast<AmegaDownloadRequest*>(userData);
    if (!request) {
        g_downloadRuntime.errorCode.store(1007, std::memory_order_relaxed);
        g_downloadRuntime.state.store(AMEGA_DOWNLOAD_FAILED, std::memory_order_release);
        return 1;
    }
    const bool success = AmegaReceivePackage(*request, g_downloadRuntime);
    delete request;
    return success ? 0 : 1;
}

static bool startPackageDownload()
{
    if (g_downloadThread || g_screen != SCREEN_ACCEPTED || g_selected < 0 ||
        g_selected >= MAX_SENDERS || !g_senders[g_selected].used ||
        !isValidTransferId(g_offerTransferId) || !isxdigit((unsigned char)g_accessToken[0]) ||
        std::strlen(g_accessToken) != 64) {
        snprintf(g_status, sizeof(g_status), "СНАЧАЛА ПОДТВЕРДИ ПРЕДЛОЖЕНИЕ");
        return false;
    }

    AmegaDownloadRequest request = {};
    snprintf(request.ip, sizeof(request.ip), "%s", g_senders[g_selected].ip);
    request.port = g_senders[g_selected].httpPort;
    snprintf(request.transferId, sizeof(request.transferId), "%s", g_offerTransferId);
    snprintf(request.accessToken, sizeof(request.accessToken), "%s", g_accessToken);
    request.fileSize = g_offerFileSize;
    snprintf(request.sha256, sizeof(request.sha256), "%s", g_offerSha256);

    g_downloadRuntime.cancel.store(false, std::memory_order_relaxed);
    g_downloadRuntime.errorCode.store(0, std::memory_order_relaxed);
    g_downloadRuntime.bytes.store(0, std::memory_order_relaxed);
    g_downloadRuntime.finalPath[0] = 0;
    g_downloadRuntime.state.store(AMEGA_DOWNLOAD_RUNNING, std::memory_order_release);

    AmegaDownloadRequest* copy = new (std::nothrow) AmegaDownloadRequest(request);
    if (!copy) {
        g_downloadRuntime.errorCode.store(1008, std::memory_order_relaxed);
        g_downloadRuntime.state.store(AMEGA_DOWNLOAD_FAILED, std::memory_order_release);
        snprintf(g_status, sizeof(g_status), "НЕДОСТАТОЧНО ПАМЯТИ ДЛЯ ЗАДАЧИ");
        return false;
    }

    g_downloadThread = SDL_CreateThread(downloadWorker, "AMEGA-PKG-Download", copy);
    if (!g_downloadThread) {
        delete copy;
        g_downloadRuntime.errorCode.store(1009, std::memory_order_relaxed);
        g_downloadRuntime.state.store(AMEGA_DOWNLOAD_FAILED, std::memory_order_release);
        snprintf(g_status, sizeof(g_status), "НЕ УДАЛОСЬ ЗАПУСТИТЬ ЗАГРУЗКУ");
        return false;
    }

    g_screen = SCREEN_DOWNLOADING;
    snprintf(g_status, sizeof(g_status), "ЗАГРУЗКА PKG НАЧАЛАСЬ");
    writeStartupLog("Started range-based PKG download to AMEGA staging directory");
    return true;
}

static void reapDownloadThread()
{
    if (!g_downloadThread) return;
    const int state = g_downloadRuntime.state.load(std::memory_order_acquire);
    if (state != AMEGA_DOWNLOAD_PAUSED &&
        state != AMEGA_DOWNLOAD_COMPLETE &&
        state != AMEGA_DOWNLOAD_FAILED)
        return;

    int threadResult = 0;
    SDL_WaitThread(g_downloadThread, &threadResult);
    g_downloadThread = nullptr;
    (void)threadResult;

    if (state == AMEGA_DOWNLOAD_COMPLETE) {
        g_accessToken[0] = 0;
        g_screen = SCREEN_VERIFIED;
        snprintf(g_status, sizeof(g_status), "ФАЙЛ ПОЛУЧЕН И SHA-256 СОВПАЛ");
        writeStartupLog("PKG download completed and receiver SHA-256 verified");
    } else if (state == AMEGA_DOWNLOAD_PAUSED) {
        g_screen = SCREEN_ACCEPTED;
        snprintf(g_status, sizeof(g_status), "ЗАГРУЗКА ПРИОСТАНОВЛЕНА. X - ПРОДОЛЖИТЬ");
        writeStartupLog("PKG download paused; verified partial bytes are retained for resume");
    } else {
        g_screen = SCREEN_ACCEPTED;
        const int error = g_downloadRuntime.errorCode.load(std::memory_order_relaxed);
        if (error == 4)
            snprintf(g_status, sizeof(g_status), "НЕДОСТАТОЧНО МЕСТА НА ДИСКЕ");
        else if (error == 401 || error == 403)
            snprintf(g_status, sizeof(g_status), "ДОСТУП ИСТЁК. ВЫБЕРИ ПК И ПОЛУЧИ ПРЕДЛОЖЕНИЕ СНОВА");
        else if (error == 8)
            snprintf(g_status, sizeof(g_status), "SHA-256 НЕ СОВПАЛ. ПОВТОРИ ЗАГРУЗКУ");
        else
            snprintf(g_status, sizeof(g_status), "СЕТЬ ПРЕРВАНА. X - ПРОДОЛЖИТЬ ЗАГРУЗКУ");
        char message[96];
        snprintf(message, sizeof(message), "PKG download stopped; error=%d", error);
        writeStartupLog(message);
    }
}

static bool isUsableLocalIPv4(const char* text)
{
    if (!text || !text[0]) return false;
    struct in_addr address = {};
    if (inet_pton(AF_INET, text, &address) != 1) return false;

    const uint32_t host = ntohl(address.s_addr);
    if (host == 0 || host == 0xffffffffu) return false;
    if ((host >> 24) == 127) return false;                  // loopback
    if ((host & 0xffff0000u) == 0xa9fe0000u) return false; // link-local
    if ((host >> 28) == 0xE) return false;                 // multicast
    return true;
}

static bool initializePs4Network()
{
    writeStartupLog("Initializing PS4 network modules");

    // OpenOrbis exposes Net and NetCtl as internal system modules.
    // Load them before calling their APIs or creating BSD sockets.
    const uint32_t netModule = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    const uint32_t ctlModule = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);
    char moduleLog[160];
    snprintf(moduleLog, sizeof(moduleLog),
        "Network module load results: Net=0x%08X NetCtl=0x%08X",
        netModule, ctlModule);
    writeStartupLog(moduleLog);

    int32_t result = sceNetInit();
    if (result < 0 && static_cast<uint32_t>(result) != 0x80410004u) {
        char message[128];
        snprintf(message, sizeof(message), "sceNetInit failed: 0x%08X", static_cast<uint32_t>(result));
        writeStartupLog(message);
        snprintf(g_localIp, sizeof(g_localIp), "СЕТЬ PS4 НЕ ЗАПУЩЕНА");
        return false;
    }

    // The PS4 network pool must exist before the first socket() call.
    g_netPoolId = sceNetPoolCreate("AMEGA", 256 * 1024, 0);
    if (g_netPoolId < 0) {
        char message[128];
        snprintf(message, sizeof(message), "sceNetPoolCreate failed: 0x%08X",
            static_cast<uint32_t>(g_netPoolId));
        writeStartupLog(message);
        snprintf(g_localIp, sizeof(g_localIp), "ОШИБКА СЕТЕВОГО ПУЛА");
        return false;
    }

    result = sceNetCtlInit();
    if (result == 0 || static_cast<uint32_t>(result) == 0x80412102u) {
        g_netCtlReady = true;
    } else {
        char message[128];
        snprintf(message, sizeof(message), "sceNetCtlInit failed: 0x%08X", static_cast<uint32_t>(result));
        writeStartupLog(message);
    }

    g_netStackReady = true;
    writeStartupLog("PS4 network stack initialized");
    return true;
}

static void refreshLocalIp()
{
    // Preferred source: the console's own network-control service.
    // Unlike arbitrary interface enumeration, this reports the configured
    // PS4 connection address for Ethernet or Wi-Fi.
    if (g_netCtlReady) {
        OrbisNetCtlInfo info = {};
        const int32_t result = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
        info.ip_address[sizeof(info.ip_address) - 1] = '\0';
        if (result == 0 && isUsableLocalIPv4(info.ip_address)) {
            snprintf(g_localIp, sizeof(g_localIp), "%s", info.ip_address);
            return;
        }

        char message[128];
        snprintf(message, sizeof(message),
            "sceNetCtlGetInfo(IP_ADDRESS) failed: rc=0x%08X ip=%s",
            static_cast<uint32_t>(result), info.ip_address);
        writeStartupLog(message);
    }

    // Compatibility fallback for unusual firmware/network-control states.
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs* item = interfaces; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
                !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK))
                continue;

            const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            char ip[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) &&
                isUsableLocalIPv4(ip)) {
                snprintf(g_localIp, sizeof(g_localIp), "%s", ip);
                freeifaddrs(interfaces);
                writeStartupLog("PS4 IPv4 address resolved by getifaddrs fallback");
                return;
            }
        }
        freeifaddrs(interfaces);
    } else {
        char message[96];
        snprintf(message, sizeof(message), "getifaddrs failed: errno=%d", errno);
        writeStartupLog(message);
    }

    snprintf(g_localIp, sizeof(g_localIp), "НЕТ IP — ПРОВЕРЬ СЕТЬ PS4");
}

static void refreshFirmwareVersion()
{
    OrbisKernelSwVersion version = {};
    version.Size = sizeof(version);
    // The bundled OpenOrbis container has the same import symbol but an
    // older no-argument placeholder declaration. Use the ABI recorded by
    // current upstream OpenOrbis headers without changing the imported NID.
    using GetSystemSwVersionFn = int32_t (*)(OrbisKernelSwVersion*);
    const auto getSystemSwVersion =
        reinterpret_cast<GetSystemSwVersionFn>(&sceKernelGetSystemSwVersion);
    const int32_t result = getSystemSwVersion(&version);
    if (result < 0) {
        snprintf(g_firmwareVersion, sizeof(g_firmwareVersion), "НЕ ОПРЕДЕЛЕНА");
        writeStartupLog("sceKernelGetSystemSwVersion unavailable");
        return;
    }

    char raw[sizeof(version.VersionString) + 1] = {};
    memcpy(raw, version.VersionString, sizeof(version.VersionString));
    raw[sizeof(version.VersionString)] = '\0';

    char* start = raw;
    while (*start && isspace((unsigned char)*start)) ++start;
    size_t length = strlen(start);
    while (length > 0 && isspace((unsigned char)start[length - 1]))
        start[--length] = '\0';

    // The version field should be plain digits and dots. Reject unexpected
    // bytes rather than placing untrusted system data directly into JSON.
    for (size_t i = 0; i < length; ++i) {
        if (!isdigit((unsigned char)start[i]) && start[i] != '.') {
            snprintf(g_firmwareVersion, sizeof(g_firmwareVersion), "НЕ ОПРЕДЕЛЕНА");
            writeStartupLog("Firmware version string had an unexpected format");
            return;
        }
    }

    if (length == 0) {
        snprintf(g_firmwareVersion, sizeof(g_firmwareVersion), "НЕ ОПРЕДЕЛЕНА");
        return;
    }

    snprintf(g_firmwareVersion, sizeof(g_firmwareVersion), "%s", start);
    char message[96];
    snprintf(message, sizeof(message), "Firmware detected: %s", g_firmwareVersion);
    writeStartupLog(message);
}

static void sendAnnouncement(const sockaddr_in& unicastTarget, const char* replyToNonce)
{
    if (g_socket < 0 || !replyToNonce || strlen(replyToNonce) != 32) return;

    char payload[640];
    snprintf(payload, sizeof(payload),
        "{\"protocol\":\"amega\",\"major\":1,"
        "\"type\":\"receiver_announcement\","
        "\"receiver_id\":\"%s\",\"instance_nonce\":\"%s\","
        "\"display_name\":\"PS4 PRO\",\"receiver_version\":\"0.2.2\","
        "\"firmware_version\":\"%s\",\"reply_to_nonce\":\"%s\"}",
        g_receiverId, g_nonce, g_firmwareVersion, replyToNonce);

    sockaddr_in target = unicastTarget;
    const uint16_t originalPort = target.sin_port;

    // PS4 is a responder, not an active discoverer: reply only to the PC
    // which sent a discovery probe. The current PC listens on fixed UDP 38472.
    target.sin_port = htons(RECEIVER_DISCOVERY_PORT);
    if (sendto(g_socket, payload, strlen(payload), 0,
        reinterpret_cast<sockaddr*>(&target), sizeof(target)) < 0) {
        char message[96];
        snprintf(message, sizeof(message), "Unicast discovery reply failed: errno=%d", errno);
        writeStartupLog(message);
    }

    // Compatibility for older senders that receive on the beacon source port.
    if (originalPort != htons(RECEIVER_DISCOVERY_PORT)) {
        target.sin_port = originalPort;
        if (sendto(g_socket, payload, strlen(payload), 0,
            reinterpret_cast<sockaddr*>(&target), sizeof(target)) < 0) {
            char message[96];
            snprintf(message, sizeof(message), "Compatibility discovery reply failed: errno=%d", errno);
            writeStartupLog(message);
        }
    }
}

static void updateSender(const char* json, const sockaddr_in& from)
{
    char protocol[24], type[40], id[48], name[64], nonce[160];
    if (!getStringField(json, "protocol", protocol, sizeof(protocol)) ||
        strcmp(protocol, "amega") != 0 ||
        getIntField(json, "major") != 1 ||
        !getStringField(json, "type", type, sizeof(type)) ||
        strcmp(type, "sender_announcement") != 0 ||
        !getStringField(json, "sender_id", id, sizeof(id)) ||
        !getStringField(json, "display_name", name, sizeof(name)) ||
        !getStringField(json, "instance_nonce", nonce, sizeof(nonce)) ||
        strlen(nonce) != 32 || strlen(name) == 0 || strlen(name) > 48)
        return;
    for (size_t i = 0; i < strlen(nonce); ++i)
        if (!isxdigit((unsigned char)nonce[i])) return;

    int port = getIntField(json, "http_port");
    if (port < 1024 || port > 65535) return;

    char ip[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) return;

    int slot = -1;
    for (int i = 0; i < MAX_SENDERS; ++i) {
        if (g_senders[i].used && strcmp(g_senders[i].id, id) == 0) {
            slot = i;
            break;
        }
        if (!g_senders[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return;

    SenderEntry& entry = g_senders[slot];
    snprintf(entry.id, sizeof(entry.id), "%s", id);
    snprintf(entry.name, sizeof(entry.name), "%s", name);
    snprintf(entry.ip, sizeof(entry.ip), "%s", ip);
    snprintf(entry.instanceNonce, sizeof(entry.instanceNonce), "%s", nonce);
    entry.httpPort = port;
    entry.lastSeen = SDL_GetTicks();
    entry.used = true;

    // Respond only to the PC probe; this PS4 does not broadcast discovery.
    sendAnnouncement(from, nonce);

    if (g_selected < 0) g_selected = slot;
    snprintf(g_status, sizeof(g_status), "ПК НАЙДЕН: %s", name);
}

static void pollNetwork()
{
    const Uint32 now = SDL_GetTicks();

    // Standby mode: listen for PC discovery probes and answer each one.
    if (g_socket >= 0) {
        for (;;) {
            char buffer[1201] = {};
            sockaddr_in from = {};
            socklen_t fromLen = sizeof(from);
            const int received = (int)recvfrom(g_socket, buffer, 1200, 0,
                reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (received <= 0) break;
            buffer[received] = 0;
            updateSender(buffer, from);
        }
    }

    for (int i = 0; i < MAX_SENDERS; ++i) {
        if (g_senders[i].used && now - g_senders[i].lastSeen > 12000) {
            g_senders[i].used = false;
            if (g_selected == i) g_selected = -1;
        }
    }
    if (g_selected < 0) {
        for (int i = 0; i < MAX_SENDERS; ++i)
            if (g_senders[i].used) { g_selected = i; break; }
    }

    // Do not leave a stale "PC found" message after the sender disappears.
    if (g_selected < 0) {
        snprintf(g_status, sizeof(g_status), "ОЖИДАНИЕ ПОДКЛЮЧЕНИЯ С ПК");
    } else if (g_screen == SCREEN_DISCOVERY) {
        snprintf(g_status, sizeof(g_status), "ПК НАЙДЕН: %s", g_senders[g_selected].name);
    }
}

static void openDiscoverySocket()
{
    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket < 0) {
        snprintf(g_status, sizeof(g_status), "ОШИБКА СЕТИ: %d", errno);
        return;
    }

    int yes = 1;
    if (setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
        snprintf(g_status, sizeof(g_status), "ОШИБКА BROADCAST: %d", errno);
        close(g_socket);
        g_socket = -1;
        return;
    }
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(SENDER_DISCOVERY_PORT);
    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        snprintf(g_status, sizeof(g_status), "ОШИБКА ПОРТА: %d", errno);
        close(g_socket);
        g_socket = -1;
        return;
    }

    const int flags = fcntl(g_socket, F_GETFL, 0);
    if (flags >= 0) fcntl(g_socket, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t nextCodepoint(const char*& p)
{
    const unsigned char c = (unsigned char)*p++;
    if (c < 0x80) return c;
    if ((c & 0xe0) == 0xc0) {
        const unsigned char c2 = (unsigned char)*p++;
        return ((c & 0x1f) << 6) | (c2 & 0x3f);
    }
    if ((c & 0xf0) == 0xe0) {
        const unsigned char c2 = (unsigned char)*p++;
        const unsigned char c3 = (unsigned char)*p++;
        return ((c & 0x0f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
    }
    return '?';
}

static uint32_t upperCodepoint(uint32_t cp)
{
    if (cp >= 0x430 && cp <= 0x44f) return cp - 0x20;
    if (cp == 0x451) return 0x401;
    if (cp >= 'a' && cp <= 'z') return cp - 32;
    return cp;
}

static const AmegaGlyph* findGlyph(uint32_t cp)
{
    for (size_t i = 0; i < sizeof(AMEGA_GLYPHS) / sizeof(AMEGA_GLYPHS[0]); ++i)
        if (AMEGA_GLYPHS[i].code == cp) return &AMEGA_GLYPHS[i];
    return nullptr;
}

static void drawText(SDL_Renderer* renderer, int x, int y, const char* text,
    SDL_Color color, int scale = 2)
{
    const char* p = text;
    int cursor = x;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    while (*p) {
        uint32_t cp = upperCodepoint(nextCodepoint(p));
        const AmegaGlyph* glyph = findGlyph(cp);
        if (glyph) {
            for (int row = 0; row < 14; ++row) {
                for (int col = 0; col < 10; ++col) {
                    if (glyph->rows[row] & (1u << (9 - col))) {
                        SDL_Rect pixel = { cursor + col * scale, y + row * scale, scale, scale };
                        SDL_RenderFillRect(renderer, &pixel);
                    }
                }
            }
        }
        cursor += 12 * scale;
    }
}

static void fill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color)
{
    SDL_Rect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderFillRect(renderer, &rect);
}

static const SDL_Color BG = { 18, 21, 27, 255 };
static const SDL_Color PANEL = { 34, 40, 50, 255 };
static const SDL_Color PANEL2 = { 43, 50, 62, 255 };
static const SDL_Color TEXT = { 235, 239, 245, 255 };
static const SDL_Color MUTED = { 143, 154, 170, 255 };
static const SDL_Color GOLD = { 245, 190, 55, 255 };
static const SDL_Color GREEN = { 99, 205, 145, 255 };

static void drawUi(SDL_Renderer* renderer, SDL_Window* window, bool surfaceRenderer)
{
    SDL_SetRenderDrawColor(renderer, BG.r, BG.g, BG.b, 255);
    SDL_RenderClear(renderer);

    fill(renderer, 0, 0, 1280, 8, GOLD);
    drawText(renderer, 62, 52, "AMEGA", GOLD, 4);
    drawText(renderer, 64, 126, "ПЕРЕДАЧА ИГР ПО WI-FI", TEXT, 2);

    if (g_screen == SCREEN_OFFER) {
        fill(renderer, 60, 195, 1160, 2, PANEL2);
        drawText(renderer, 64, 225, "ПРЕДЛОЖЕНИЕ ОТ КОМПЬЮТЕРА", GOLD, 2);
        fill(renderer, 60, 275, 1160, 245, PANEL);
        char shortName[88] = {};
        snprintf(shortName, sizeof(shortName), "%.76s", g_offerFileName);
        drawText(renderer, 92, 310, shortName, TEXT, 1);
        char sizeLine[80] = {};
        snprintf(sizeLine, sizeof(sizeLine), "РАЗМЕР: %.2f MB",
            static_cast<double>(g_offerFileSize) / (1024.0 * 1024.0));
        drawText(renderer, 92, 360, sizeLine, MUTED, 1);
        char hashLine[96] = {};
        snprintf(hashLine, sizeof(hashLine), "SHA-256: %.32s...", g_offerSha256);
        drawText(renderer, 92, 400, hashLine, MUTED, 1);
        drawText(renderer, 64, 555, g_status, GREEN, 1);
        drawText(renderer, 64, 625, "X - ПРИНЯТЬ     O - НАЗАД", TEXT, 1);
        drawText(renderer, 64, 685, "ПРИНЯТИЕ СОХРАНИТ ФАЙЛ НА PS4 ТОЛЬКО ПОСЛЕ ПРОВЕРКИ", MUTED, 1);
    } else if (g_screen == SCREEN_ACCEPTED) {
        fill(renderer, 60, 195, 1160, 2, PANEL2);
        drawText(renderer, 64, 225, "ПРЕДЛОЖЕНИЕ ПРИНЯТО", GOLD, 2);
        fill(renderer, 60, 275, 1160, 245, PANEL);
        drawText(renderer, 96, 320, "ФАЙЛ БУДЕТ СОХРАНЁН В AMEGA / DOWNLOAD", TEXT, 1);
        uint64_t received = g_downloadRuntime.bytes.load(std::memory_order_relaxed);
        char receivedLine[96] = {};
        snprintf(receivedLine, sizeof(receivedLine), "УЖЕ ПОЛУЧЕНО: %.2f MB",
            static_cast<double>(received) / (1024.0 * 1024.0));
        drawText(renderer, 96, 370, receivedLine, MUTED, 1);
        drawText(renderer, 96, 420, "ПОСЛЕ ЗАГРУЗКИ AMEGA СВЕРИТ SHA-256", TEXT, 1);
        drawText(renderer, 64, 555, g_status, GREEN, 1);
        drawText(renderer, 64, 625, "X - НАЧАТЬ / ПРОДОЛЖИТЬ     O - НАЗАД", TEXT, 1);
        drawText(renderer, 64, 685, "ЧАСТИЧНЫЙ ФАЙЛ СОХРАНЯЕТСЯ ДЛЯ ВОЗОБНОВЛЕНИЯ", MUTED, 1);
    } else if (g_screen == SCREEN_DOWNLOADING) {
        fill(renderer, 60, 195, 1160, 2, PANEL2);
        drawText(renderer, 64, 225, "ЗАГРУЗКА PKG НА PS4", GOLD, 2);
        fill(renderer, 60, 275, 1160, 245, PANEL);
        uint64_t total = g_offerFileSize;
        uint64_t received = g_downloadRuntime.bytes.load(std::memory_order_relaxed);
        if (received > total) received = total;
        int percent = total > 0 ? static_cast<int>((received * 100u) / total) : 0;
        char progressLine[96] = {};
        snprintf(progressLine, sizeof(progressLine), "ПОЛУЧЕНО: %d%%", percent);
        drawText(renderer, 96, 320, progressLine, TEXT, 2);
        char sizeLine[120] = {};
        snprintf(sizeLine, sizeof(sizeLine), "%.2f MB ИЗ %.2f MB",
            static_cast<double>(received) / (1024.0 * 1024.0),
            static_cast<double>(total) / (1024.0 * 1024.0));
        drawText(renderer, 96, 375, sizeLine, MUTED, 1);
        fill(renderer, 96, 425, 1000, 22, PANEL2);
        int width = static_cast<int>((static_cast<uint64_t>(1000) * percent) / 100u);
        if (width > 0) fill(renderer, 96, 425, width, 22, GREEN);
        const int state = g_downloadRuntime.state.load(std::memory_order_relaxed);
        drawText(renderer, 96, 475,
            state == AMEGA_DOWNLOAD_VERIFYING ? "ПРОВЕРКА SHA-256..." : "ЗАПИСЬ ВРЕМЕННОГО ФАЙЛА...",
            GOLD, 1);
        drawText(renderer, 64, 555, g_status, GREEN, 1);
        drawText(renderer, 64, 625, "O - ПРИОСТАНОВИТЬ (ЧАСТИЧНЫЙ ФАЙЛ ОСТАНЕТСЯ)", TEXT, 1);
        drawText(renderer, 64, 685, "НЕ ЗАКРЫВАЙ ПРИЛОЖЕНИЕ ДЛЯ ПОЛУЧЕНИЯ МАКСИМАЛЬНОЙ СКОРОСТИ", MUTED, 1);
    } else if (g_screen == SCREEN_VERIFIED) {
        fill(renderer, 60, 195, 1160, 2, PANEL2);
        drawText(renderer, 64, 225, "ФАЙЛ ПОЛУЧЕН И ПРОВЕРЕН", GOLD, 2);
        fill(renderer, 60, 275, 1160, 245, PANEL);
        drawText(renderer, 96, 320, "SHA-256 СОВПАЛ С ОРИГИНАЛОМ НА ПК", GREEN, 1);
        drawText(renderer, 96, 375, "ФАЙЛ СОХРАНЁН:", TEXT, 1);
        char pathLine[112] = {};
        snprintf(pathLine, sizeof(pathLine), "%.88s", g_downloadRuntime.finalPath);
        drawText(renderer, 96, 420, pathLine, MUTED, 1);
        drawText(renderer, 64, 555, "ОРИГИНАЛЬНЫЙ PKG НА ПК НЕ УДАЛЯЛСЯ", GREEN, 1);
        drawText(renderer, 64, 625, "O - ВЕРНУТЬСЯ К СПИСКУ КОМПЬЮТЕРОВ", TEXT, 1);
        drawText(renderer, 64, 685, "УСТАНОВКА ИГРЫ ОСТАЁТСЯ ОТДЕЛЬНЫМ ДЕЙСТВИЕМ", MUTED, 1);
    } else {
        fill(renderer, 60, 185, 1160, 2, PANEL2);
        drawText(renderer, 64, 218, "КОМПЬЮТЕРЫ, НАШЕДШИЕ PS4", GOLD, 2);

        int count = 0;
        for (int i = 0; i < MAX_SENDERS; ++i) if (g_senders[i].used) ++count;

        if (count == 0) {
            fill(renderer, 60, 275, 1160, 150, PANEL);
            drawText(renderer, 96, 320, "ОЖИДАНИЕ ПОДКЛЮЧЕНИЯ С ПК...", TEXT, 2);
            drawText(renderer, 96, 370, "НА ПК НАЖМИ ИСКАТЬ PS4 — КОНСОЛЬ ОТВЕТИТ", MUTED, 1);
        } else {
            int row = 0;
            for (int i = 0; i < MAX_SENDERS; ++i) {
                if (!g_senders[i].used) continue;
                const bool selected = (i == g_selected);
                fill(renderer, 60, 270 + row * 92, 1160, 78, selected ? PANEL2 : PANEL);
                if (selected) fill(renderer, 60, 270 + row * 92, 7, 78, GOLD);
                drawText(renderer, 96, 288 + row * 92, g_senders[i].name, TEXT, 2);
                char line[100];
                snprintf(line, sizeof(line), "%s  :  %d", g_senders[i].ip, g_senders[i].httpPort);
                drawText(renderer, 96, 326 + row * 92, line, MUTED, 1);
                ++row;
            }
        }

        fill(renderer, 60, 520, 1160, 2, PANEL2);
        drawText(renderer, 64, 555, g_status, GREEN, 1);
        drawText(renderer, 64, 575, "IP PS4:", TEXT, 1);
        drawText(renderer, 160, 575, g_localIp, MUTED, 1);
        drawText(renderer, 64, 600, "ПРОШИВКА PS4:", TEXT, 1);
        drawText(renderer, 240, 600, g_firmwareVersion, MUTED, 1);
        drawText(renderer, 64, 625, controllerConnected() ? "ДЖОЙСТИК: ПОДКЛЮЧЁН" : "ДЖОЙСТИК: НЕ НАЙДЕН", controllerConnected() ? GREEN : MUTED, 1);
        drawText(renderer, 64, 650, "X - ПРЕДЛОЖЕНИЕ     O - СБРОСИТЬ ВЫБОР", TEXT, 1);
        drawText(renderer, 64, 685, "ВЫБЕРИ ПК И НАЖМИ X, ЧТОБЫ ПОСМОТРЕТЬ PKG", MUTED, 1);
    }

    SDL_RenderPresent(renderer);
    if (surfaceRenderer)
        SDL_UpdateWindowSurface(window);
}

static void moveSelection(int direction)
{
    if (g_screen != SCREEN_DISCOVERY || g_selected < 0) return;

    for (int n = 1; n <= MAX_SENDERS; ++n) {
        const int candidate = (g_selected + direction * n + MAX_SENDERS * 2) % MAX_SENDERS;
        if (g_senders[candidate].used) {
            g_selected = candidate;
            return;
        }
    }
}

static void activateSelection()
{
    if (g_screen == SCREEN_DISCOVERY)
        fetchSelectedOffer();
    else if (g_screen == SCREEN_OFFER)
        acceptSelectedOffer();
    else if (g_screen == SCREEN_ACCEPTED)
        startPackageDownload();
}

static void returnToPreviousScreen()
{
    if (g_screen == SCREEN_DISCOVERY) {
        g_selected = -1;
        snprintf(g_status, sizeof(g_status), "ВЫБОР СБРОШЕН");
        return;
    }
    if (g_screen == SCREEN_DOWNLOADING) {
        g_downloadRuntime.cancel.store(true, std::memory_order_relaxed);
        snprintf(g_status, sizeof(g_status), "ОСТАНОВКА. ЧАСТИЧНЫЙ ФАЙЛ СОХРАНЯЕТСЯ");
        return;
    }

    g_screen = SCREEN_DISCOVERY;
    g_accessToken[0] = 0;
    snprintf(g_status, sizeof(g_status), "ВОЗВРАТ К СПИСКУ КОМПЬЮТЕРОВ");
}

static void openInputDevices()
{
    if (g_gameController && SDL_GameControllerGetAttached(g_gameController)) return;
    if (g_gameController) {
        SDL_GameControllerClose(g_gameController);
        g_gameController = nullptr;
    }
    if (g_controller) {
        SDL_JoystickClose(g_controller);
        g_controller = nullptr;
    }

    // Prefer SDL's standardized game-controller mapping (Cross=A, Circle=B,
    // D-pad buttons). Fall back to raw joystick events for unusual mappings.
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i)) continue;
        g_gameController = SDL_GameControllerOpen(i);
        if (g_gameController) {
            writeStartupLog("GameController input opened");
            return;
        }
    }
    for (int i = 0; i < count; ++i) {
        g_controller = SDL_JoystickOpen(i);
        if (g_controller) {
            writeStartupLog("Raw joystick input opened");
            return;
        }
    }
    writeStartupLog("No SDL controller detected; waiting for hot-plug");
}

static bool controllerConnected()
{
    if (g_gameController && SDL_GameControllerGetAttached(g_gameController)) return true;
    return g_controller && SDL_JoystickGetAttached(g_controller);
}

static void handleController(SDL_Event& event)
{
    if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_JOYDEVICEADDED) {
        openInputDevices();
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (g_gameController &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gameController)) == event.cdevice.which) {
            SDL_GameControllerClose(g_gameController);
            g_gameController = nullptr;
        }
        openInputDevices();
        return;
    }
    if (event.type == SDL_JOYDEVICEREMOVED) {
        if (g_controller && SDL_JoystickInstanceID(g_controller) == event.jdevice.which) {
            SDL_JoystickClose(g_controller);
            g_controller = nullptr;
        }
        openInputDevices();
        return;
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                moveSelection(-1);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                moveSelection(1);
                break;
            case SDL_CONTROLLER_BUTTON_A: // Cross.
                activateSelection();
                break;
            case SDL_CONTROLLER_BUTTON_B: // Circle.
                returnToPreviousScreen();
                break;
            default:
                break;
        }
        return;
    }

    // A mapped GameController also emits raw joystick events. Ignore those
    // to avoid processing the same physical press twice.
    if (g_gameController) return;

    // Raw SDL mappings: D-pad is usually a hat, sometimes buttons 13/14.
    if (event.type == SDL_JOYHATMOTION && event.jhat.hat == 0) {
        if (event.jhat.value & SDL_HAT_UP) moveSelection(-1);
        else if (event.jhat.value & SDL_HAT_DOWN) moveSelection(1);
        return;
    }
    if (event.type != SDL_JOYBUTTONDOWN) return;

    switch (event.jbutton.button) {
        case 13:
            moveSelection(-1);
            break;
        case 14:
            moveSelection(1);
            break;
        case 0:
            activateSelection();
            break;
        case 1:
            returnToPreviousScreen();
            break;
        default:
            break;
    }
}

static void writeStartupLog(const char* message)
{
    FILE* file = fopen("/data/amega-startup.log", "ab");
    if (!file) return;
    fprintf(file, "%lld %s\n", (long long)time(nullptr), message ? message : "");
    fclose(file);
}

static SDL_Renderer* createCompatibleRenderer(SDL_Window* window, bool& surfaceRenderer)
{
    surfaceRenderer = false;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer) return renderer;

    char message[256];
    snprintf(message, sizeof(message), "Accelerated renderer unavailable: %s", SDL_GetError());
    writeStartupLog(message);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (renderer) return renderer;

    snprintf(message, sizeof(message), "Window software renderer unavailable: %s", SDL_GetError());
    writeStartupLog(message);

    // Last-resort SDL path for backends that expose a window surface but no
    // window renderer. Presentation is explicitly updated only in this mode.
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    if (surface) {
        renderer = SDL_CreateSoftwareRenderer(surface);
        if (renderer) {
            surfaceRenderer = true;
            return renderer;
        }
    }

    snprintf(message, sizeof(message), "All SDL render paths failed: %s", SDL_GetError());
    writeStartupLog(message);
    return nullptr;
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    setvbuf(stdout, nullptr, _IONBF, 0);
    writeStartupLog("AMEGA 0.2.2 startup");

    // Initialize Net/NetCtl and create the required socket pool before any
    // network-interface query or BSD socket use.
    const bool networkReady = initializePs4Network();
    if (networkReady) refreshLocalIp();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        char message[256];
        snprintf(message, sizeof(message), "SDL_Init failed: %s", SDL_GetError());
        writeStartupLog(message);
        printf("AMEGA: %s\n", message);
        SDL_Quit();
        return 1;
    }

    bool fullscreenAttempt = true;
    SDL_Window* window = SDL_CreateWindow(
        "AMEGA PS4 Receiver",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window) {
        fullscreenAttempt = false;
        writeStartupLog("Fullscreen desktop window unavailable; trying 1280x720 window");
        window = SDL_CreateWindow(
            "AMEGA PS4 Receiver",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 720, SDL_WINDOW_SHOWN);
    }
    if (!window) {
        char message[256];
        snprintf(message, sizeof(message), "SDL_CreateWindow failed: %s", SDL_GetError());
        writeStartupLog(message);
        printf("AMEGA: %s\n", message);
        SDL_Quit();
        return 2;
    }

    bool surfaceRenderer = false;
    SDL_Renderer* renderer = createCompatibleRenderer(window, surfaceRenderer);
    if (!renderer && fullscreenAttempt) {
        SDL_DestroyWindow(window);
        writeStartupLog("Renderer unavailable in fullscreen; retrying windowed mode");
        window = SDL_CreateWindow(
            "AMEGA PS4 Receiver",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1280, 720, SDL_WINDOW_SHOWN);
        if (window)
            renderer = createCompatibleRenderer(window, surfaceRenderer);
    }
    if (!renderer) {
        char message[256];
        snprintf(message, sizeof(message), "SDL renderer creation failed: %s", SDL_GetError());
        writeStartupLog(message);
        printf("AMEGA: %s\n", message);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 3;
    }

    if (SDL_RenderSetLogicalSize(renderer, 1280, 720) != 0)
        writeStartupLog("SDL_RenderSetLogicalSize failed; using native renderer dimensions");

    SDL_RendererInfo rendererInfo = {};
    if (SDL_GetRendererInfo(renderer, &rendererInfo) == 0) {
        char message[256];
        snprintf(message, sizeof(message), "Renderer: %s%s",
            rendererInfo.name ? rendererInfo.name : "unknown",
            surfaceRenderer ? " (window surface fallback)" : "");
        writeStartupLog(message);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);
    openInputDevices();

    loadOrCreateReceiverId();
    refreshLocalIp();
    refreshFirmwareVersion();
    if (g_netStackReady) {
        openDiscoverySocket();
    } else {
        snprintf(g_status, sizeof(g_status), "СЕТЬ НЕ ИНИЦИАЛИЗИРОВАНА");
        writeStartupLog("Discovery socket skipped because PS4 network initialization failed");
    }
    if (g_socket < 0)
        writeStartupLog("Discovery socket unavailable; UI remains active");

    writeStartupLog("UI initialized");
    bool running = true;
    while (running) {
        pollNetwork();
        reapDownloadThread();
        const Uint32 networkNow = SDL_GetTicks();
        if (networkNow - g_lastNetworkRefresh >= 5000) {
            refreshLocalIp();
            g_lastNetworkRefresh = networkNow;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
            handleController(event);
        }
        drawUi(renderer, window, surfaceRenderer);
        SDL_Delay(33);
    }

    if (g_downloadThread) {
        g_downloadRuntime.cancel.store(true, std::memory_order_relaxed);
        SDL_WaitThread(g_downloadThread, nullptr);
        g_downloadThread = nullptr;
    }
    g_accessToken[0] = 0;
    if (g_socket >= 0) close(g_socket);
    if (g_gameController) SDL_GameControllerClose(g_gameController);
    if (g_controller) SDL_JoystickClose(g_controller);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    writeStartupLog("AMEGA normal exit");
    return 0;
}
