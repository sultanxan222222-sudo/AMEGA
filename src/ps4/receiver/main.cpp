#include <SDL2/SDL.h>
#include <orbis/libkernel.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font_ru.h"
#include "transfer.h"

static const int SENDER_DISCOVERY_PORT = 38470;
static const int RECEIVER_DISCOVERY_PORT = 38472;
static const int HTTP_PORT = 38471;
static const int MAX_SENDERS = 8;

struct SenderEntry {
    char id[48];
    char name[64];
    char ip[INET_ADDRSTRLEN];
    int httpPort;
    Uint32 lastSeen;
    bool used;
};

static SenderEntry g_senders[MAX_SENDERS];
static int g_selected = -1;
static int g_socket = -1;
static char g_receiverId[40] = {};
static char g_nonce[40] = {};
static char g_lastSenderNonce[160] = {};
static char g_status[128] = "ПОИСК КОМПЬЮТЕРА В СЕТИ";
static char g_localIp[48] = "IP-UNKNOWN";
static Uint32 g_lastBeacon = 0;
static Uint32 g_nextSocketRetry = 0;
static SDL_Joystick* g_controller = nullptr;
static amega_transfer::Offer g_offer;
static Uint32 g_lastOfferPoll = 0;
static bool g_acceptRequested = false;
static bool g_downloading = false;
static uint64_t g_downloadBytes = 0;
static char g_savedFile[220] = {};
static char g_completedTransferId[80] = {};

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

static bool getStringField(const char* json, const char* key, char* out, size_t cap)
{
    char needle[80];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) ++p;
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
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

static void sendAnnouncement(const sockaddr_in* unicastTarget = nullptr)
{
    if (g_socket < 0 || !g_lastSenderNonce[0]) return;

    char payload[640];
    snprintf(payload, sizeof(payload),
        "{\"protocol\":\"amega\",\"major\":1,"
        "\"type\":\"receiver_announcement\","
        "\"receiver_id\":\"%s\",\"instance_nonce\":\"%s\","
        "\"display_name\":\"PS4 PRO\",\"receiver_version\":\"0.2.2\","
        "\"reply_to_nonce\":\"%s\"}",
        g_receiverId, g_nonce, g_lastSenderNonce);

    if (unicastTarget) {
        sockaddr_in target = *unicastTarget;
        target.sin_port = htons(RECEIVER_DISCOVERY_PORT);
        sendto(g_socket, payload, strlen(payload), 0,
            reinterpret_cast<sockaddr*>(&target), sizeof(target));
        return;
    }

    struct ifaddrs* interfaces = nullptr;
    bool sent = false;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs* item = interfaces; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
                !(item->ifa_flags & IFF_UP) || !(item->ifa_flags & IFF_BROADCAST) ||
                (item->ifa_flags & IFF_LOOPBACK) || !item->ifa_broadaddr)
                continue;
            sockaddr_in target = *reinterpret_cast<sockaddr_in*>(item->ifa_broadaddr);
            target.sin_port = htons(RECEIVER_DISCOVERY_PORT);
            if (sendto(g_socket, payload, strlen(payload), 0,
                reinterpret_cast<sockaddr*>(&target), sizeof(target)) >= 0)
                sent = true;
        }
        freeifaddrs(interfaces);
    }
    if (!sent) {
        sockaddr_in target = {};
        target.sin_family = AF_INET;
        target.sin_port = htons(RECEIVER_DISCOVERY_PORT);
        target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(g_socket, payload, strlen(payload), 0,
            reinterpret_cast<sockaddr*>(&target), sizeof(target));
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
    entry.httpPort = port;
    entry.lastSeen = SDL_GetTicks();
    entry.used = true;
    snprintf(g_lastSenderNonce, sizeof(g_lastSenderNonce), "%s", nonce);

    sockaddr_in reply = from;
    reply.sin_port = htons(RECEIVER_DISCOVERY_PORT);
    sendAnnouncement(&reply);

    if (g_selected < 0) g_selected = slot;
    snprintf(g_status, sizeof(g_status), "НАЙДЕН КОМПЬЮТЕР: %s", name);
}

static void openDiscoverySocket();

static void pollOffer()
{
    if (g_selected < 0 || !g_senders[g_selected].used) return;
    const Uint32 now = SDL_GetTicks();
    if (now - g_lastOfferPoll < 3000) return;
    g_lastOfferPoll = now;

    amega_transfer::Offer next;
    std::string error;
    if (amega_transfer::fetchOffer(g_senders[g_selected].ip,
        g_senders[g_selected].httpPort, next, error)) {
        if (next.ready && next.transferId == g_completedTransferId) {
            g_offer.ready = false;
            snprintf(g_status, sizeof(g_status), "ЭТА ПЕРЕДАЧА УЖЕ ЗАВЕРШЕНА");
        } else if (next.ready) {
            if (next.transferId != g_offer.transferId) g_downloadBytes = 0;
            g_offer = next;
            snprintf(g_status, sizeof(g_status), "ФАЙЛ ГОТОВ: НАЖМИТЕ X ДЛЯ ПРИЁМА");
        } else if (!g_downloading) {
            g_offer.ready = false;
            snprintf(g_status, sizeof(g_status), "ПК НАЙДЕН. ОЖИДАЮ КОМАНДУ ОТПРАВИТЬ");
        }
    } else if (!g_offer.ready && !g_downloading) {
        snprintf(g_status, sizeof(g_status), "ПК НАЙДЕН, HTTP: ОЖИДАНИЕ");
    }
}

static void pollNetwork()
{
    const Uint32 now = SDL_GetTicks();
    if (g_socket < 0 && now >= g_nextSocketRetry) {
        openDiscoverySocket();
        g_nextSocketRetry = now + 3000;
    }
    if (g_socket >= 0 && now - g_lastBeacon >= 3000) {
        sendAnnouncement(nullptr);
        g_lastBeacon = now;
    }

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
            if (g_selected == i) {
                g_selected = -1;
                g_offer.ready = false;
                g_lastOfferPoll = 0;
            }
        }
    }
    if (g_selected < 0) {
        for (int i = 0; i < MAX_SENDERS; ++i)
            if (g_senders[i].used) { g_selected = i; break; }
    }
    pollOffer();
}

static void writeStartupLog(const char* message);

static void refreshLocalIp()
{
    snprintf(g_localIp, sizeof(g_localIp), "IP-UNKNOWN");
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        char message[160];
        snprintf(message, sizeof(message), "getifaddrs failed errno=%d (%s)", errno, strerror(errno));
        writeStartupLog(message);
        return;
    }

    bool found = false;
    for (struct ifaddrs* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK))
            continue;

        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        const unsigned long host = ntohl(address->sin_addr.s_addr);
        if ((host & 0xffff0000UL) == 0xa9fe0000UL) continue; // Ignore link-local 169.254/16.
        if (inet_ntop(AF_INET, &address->sin_addr, g_localIp, sizeof(g_localIp))) {
            found = true;
            break;
        }
    }
    freeifaddrs(interfaces);

    if (found) {
        char message[160];
        snprintf(message, sizeof(message), "PS4 IPv4 address: %s", g_localIp);
        writeStartupLog(message);
    } else {
        writeStartupLog("No active non-loopback IPv4 address found");
    }
}

static void setNetworkError(const char* stage, int errorCode)
{
    char message[220];
    snprintf(message, sizeof(message), "UDP %s failed errno=%d (%s)",
        stage, errorCode, strerror(errorCode));
    writeStartupLog(message);
    snprintf(g_status, sizeof(g_status), "ОШИБКА %s: %d", stage, errorCode);
}

static void openDiscoverySocket()
{
    refreshLocalIp();

    g_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket < 0) {
        const int errorCode = errno;
        setNetworkError("SOCKET", errorCode);
        return;
    }

    int yes = 1;
    if (setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0) {
        const int errorCode = errno;
        setNetworkError("BROADCAST", errorCode);
        close(g_socket);
        g_socket = -1;
        return;
    }
    if (setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        const int errorCode = errno;
        setNetworkError("REUSEADDR", errorCode);
        close(g_socket);
        g_socket = -1;
        return;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(SENDER_DISCOVERY_PORT);
    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        const int errorCode = errno;
        setNetworkError("BIND-38470", errorCode);
        close(g_socket);
        g_socket = -1;
        return;
    }

    const int flags = fcntl(g_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(g_socket, F_SETFL, flags | O_NONBLOCK) != 0) {
        const int errorCode = errno;
        setNetworkError("NONBLOCK", errorCode);
        close(g_socket);
        g_socket = -1;
        return;
    }

    snprintf(g_status, sizeof(g_status), "UDP 38470 ГОТОВ");
    writeStartupLog("UDP discovery socket bound to 0.0.0.0:38470; nonblocking mode ready");
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

    fill(renderer, 60, 185, 1160, 2, PANEL2);
    drawText(renderer, 64, 218, "КОМПЬЮТЕРЫ В СЕТИ", GOLD, 2);

    int count = 0;
    for (int i = 0; i < MAX_SENDERS; ++i) if (g_senders[i].used) ++count;

    if (count == 0) {
        fill(renderer, 60, 275, 1160, 150, PANEL);
        drawText(renderer, 96, 320, "ИЩУ КОМПЬЮТЕР AMEGA...", TEXT, 2);
        drawText(renderer, 96, 370, "ПК И PS4 ДОЛЖНЫ БЫТЬ В ОДНОЙ WI-FI СЕТИ", MUTED, 1);
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

    fill(renderer, 60, 455, 1160, 2, PANEL2);
    if (g_offer.ready) {
        drawText(renderer, 64, 474, "ПРЕДЛОЖЕНИЕ ОТ ПК", GOLD, 1);
        char fileLine[180];
        snprintf(fileLine, sizeof(fileLine), "%s  |  %llu БАЙТ",
            g_offer.fileName.c_str(), (unsigned long long)g_offer.fileSize);
        drawText(renderer, 64, 496, fileLine, TEXT, 1);
    } else {
        drawText(renderer, 64, 482, "НА ПК ВЫБЕРИ ЭТУ PS4 И НАЖМИ «ОТПРАВИТЬ»", MUTED, 1);
    }

    fill(renderer, 60, 545, 1160, 2, PANEL2);
    drawText(renderer, 64, 565, g_status, g_downloading ? GOLD : GREEN, 1);
    if (g_downloading && g_offer.fileSize > 0) {
        int barWidth = (int)(1080ULL * g_downloadBytes / g_offer.fileSize);
        fill(renderer, 64, 592, 1080, 12, PANEL2);
        fill(renderer, 64, 592, barWidth, 12, GREEN);
    }
    char ipLine[100];
    snprintf(ipLine, sizeof(ipLine), "IP PS4: %s   |   ПАПКА: /data/AMEGA/Received", g_localIp);
    drawText(renderer, 64, 620, ipLine, MUTED, 1);
    if (g_savedFile[0]) drawText(renderer, 64, 646, g_savedFile, TEXT, 1);
    drawText(renderer, 64, 680, "X - ПРИНЯТЬ ФАЙЛ     O - СБРОСИТЬ ВЫБОР ПК", TEXT, 1);
    SDL_RenderPresent(renderer);
    if (surfaceRenderer)
        SDL_UpdateWindowSurface(window);
}

static void moveSelection(int direction)
{
    if (g_selected < 0) return;

    for (int n = 1; n <= MAX_SENDERS; ++n) {
        const int candidate = (g_selected + direction * n + MAX_SENDERS * 2) % MAX_SENDERS;
        if (g_senders[candidate].used) {
            if (g_selected != candidate) {
                g_offer.ready = false;
                g_lastOfferPoll = 0;
            }
            g_selected = candidate;
            return;
        }
    }
}

static void handleController(SDL_Event& event)
{
    // SDL reports a DualShock D-pad as a hat on most controller mappings.
    // Keep the button fallback for mappings that expose it as buttons.
    if (event.type == SDL_JOYHATMOTION && event.jhat.hat == 0) {
        if (event.jhat.value & SDL_HAT_UP)
            moveSelection(-1);
        else if (event.jhat.value & SDL_HAT_DOWN)
            moveSelection(1);
        return;
    }

    if (event.type != SDL_JOYBUTTONDOWN) return;

    switch (event.jbutton.button) {
        case 13: // D-pad up on button-based mappings
            moveSelection(-1);
            break;
        case 14: // D-pad down on button-based mappings
            moveSelection(1);
            break;
        case 0: // Cross accepts the offered package.
            if (g_selected >= 0 && g_senders[g_selected].used && g_offer.ready && !g_downloading) {
                g_acceptRequested = true;
                snprintf(g_status, sizeof(g_status), "ПРИНИМАЮ ФАЙЛ...");
            } else if (g_selected >= 0 && g_senders[g_selected].used) {
                snprintf(g_status, sizeof(g_status), "СНАЧАЛА НАЖМИ «ОТПРАВИТЬ» НА ПК");
            }
            break;
        case 1: // Circle clears the selected PC.
            g_selected = -1;
            g_offer.ready = false;
            g_completedTransferId[0] = 0;
            g_lastOfferPoll = 0;
            snprintf(g_status, sizeof(g_status), "ВЫБОР СБРОШЕН");
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
    writeStartupLog("AMEGA 0.3.0 transfer receiver startup");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
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
    if (SDL_NumJoysticks() > 0) g_controller = SDL_JoystickOpen(0);

    loadOrCreateReceiverId();
    openDiscoverySocket();
    if (g_socket < 0)
        writeStartupLog("Discovery socket unavailable; UI remains active");

    writeStartupLog("UI initialized");
    bool running = true;
    while (running) {
        pollNetwork();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
            handleController(event);
        }
        if (g_acceptRequested && g_selected >= 0 && g_senders[g_selected].used && g_offer.ready) {
            g_acceptRequested = false;
            g_downloading = true;
            g_downloadBytes = 0;
            g_savedFile[0] = 0;
            drawUi(renderer, window, surfaceRenderer);
            std::string savedPath, error;
            bool ok = amega_transfer::downloadFile(g_senders[g_selected].ip,
                g_senders[g_selected].httpPort, g_offer,
                [&](uint64_t bytes) {
                    g_downloadBytes = bytes;
                    drawUi(renderer, window, surfaceRenderer);
                }, savedPath, error);
            g_downloading = false;
            if (ok) {
                snprintf(g_status, sizeof(g_status), "ГОТОВО: SHA-256 ПРОВЕРЕН");
                snprintf(g_savedFile, sizeof(g_savedFile), "СОХРАНЕНО: %s", savedPath.c_str());
                snprintf(g_completedTransferId, sizeof(g_completedTransferId), "%s", g_offer.transferId.c_str());
                g_offer.ready = false;
                writeStartupLog("Package received and SHA-256 verified");
            } else {
                snprintf(g_status, sizeof(g_status), "%s", error.c_str());
                writeStartupLog(error.c_str());
            }
        }
        drawUi(renderer, window, surfaceRenderer);
        SDL_Delay(33);
    }

    if (g_socket >= 0) close(g_socket);
    if (g_controller) SDL_JoystickClose(g_controller);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    writeStartupLog("AMEGA normal exit");
    return 0;
}
