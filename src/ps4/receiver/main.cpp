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
static char g_localIp[64] = "НЕ ОПРЕДЕЛЁН";
static char g_firmwareVersion[48] = "НЕ ОПРЕДЕЛЕНА";
static Uint32 g_lastBeacon = 0;
static SDL_Joystick* g_controller = nullptr;
static SDL_GameController* g_gameController = nullptr;
static Uint32 g_lastNetworkRefresh = 0;

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

static void refreshLocalIp()
{
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        snprintf(g_localIp, sizeof(g_localIp), "ОШИБКА СЕТИ");
        return;
    }

    bool found = false;
    for (struct ifaddrs* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            !(item->ifa_flags & IFF_UP) || (item->ifa_flags & IFF_LOOPBACK))
            continue;

        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        char ip[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip))) {
            snprintf(g_localIp, sizeof(g_localIp), "%s", ip);
            found = true;
            break;
        }
    }
    freeifaddrs(interfaces);
    if (!found) snprintf(g_localIp, sizeof(g_localIp), "НЕТ IPv4 СЕТИ");
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

static void sendAnnouncement(const sockaddr_in* unicastTarget = nullptr)
{
    if (g_socket < 0 || !g_lastSenderNonce[0]) return;

    char payload[640];
    snprintf(payload, sizeof(payload),
        "{\"protocol\":\"amega\",\"major\":1,"
        "\"type\":\"receiver_announcement\","
        "\"receiver_id\":\"%s\",\"instance_nonce\":\"%s\","
        "\"display_name\":\"PS4 PRO\",\"receiver_version\":\"0.2.2\","
        "\"firmware_version\":\"%s\",\"reply_to_nonce\":\"%s\"}",
        g_receiverId, g_nonce, g_firmwareVersion, g_lastSenderNonce);

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

static void pollNetwork()
{
    const Uint32 now = SDL_GetTicks();
    if (now - g_lastBeacon >= 3000) {
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
            if (g_selected == i) g_selected = -1;
        }
    }
    if (g_selected < 0) {
        for (int i = 0; i < MAX_SENDERS; ++i)
            if (g_senders[i].used) { g_selected = i; break; }
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

    fill(renderer, 60, 520, 1160, 2, PANEL2);
    drawText(renderer, 64, 555, g_status, GREEN, 1);
    drawText(renderer, 64, 575, "IP PS4:", TEXT, 1);
    drawText(renderer, 160, 575, g_localIp, MUTED, 1);
    drawText(renderer, 64, 600, "ПРОШИВКА PS4:", TEXT, 1);
    drawText(renderer, 240, 600, g_firmwareVersion, MUTED, 1);
    drawText(renderer, 64, 625, controllerConnected() ? "ДЖОЙСТИК: ПОДКЛЮЧЁН" : "ДЖОЙСТИК: НЕ НАЙДЕН", controllerConnected() ? GREEN : MUTED, 1);
    drawText(renderer, 64, 650, "X - ВЫБРАТЬ ПК     O - СБРОСИТЬ ВЫБОР", TEXT, 1);
    drawText(renderer, 64, 685, "ПЕРЕДАЧА ФАЙЛОВ БУДЕТ ВКЛЮЧЕНА В СЛЕДУЮЩЕМ ЭТАПЕ", MUTED, 1);
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
            g_selected = candidate;
            return;
        }
    }
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
            case SDL_CONTROLLER_BUTTON_A: // Cross on the standard PlayStation mapping.
                if (g_selected >= 0 && g_senders[g_selected].used)
                    snprintf(g_status, sizeof(g_status), "ВЫБРАН ПК: %s", g_senders[g_selected].name);
                break;
            case SDL_CONTROLLER_BUTTON_B: // Circle.
                g_selected = -1;
                snprintf(g_status, sizeof(g_status), "ВЫБОР СБРОШЕН");
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
            if (g_selected >= 0 && g_senders[g_selected].used)
                snprintf(g_status, sizeof(g_status), "ВЫБРАН ПК: %s", g_senders[g_selected].name);
            break;
        case 1:
            g_selected = -1;
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
    writeStartupLog("AMEGA 0.2.2 startup");

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
    openDiscoverySocket();
    if (g_socket < 0)
        writeStartupLog("Discovery socket unavailable; UI remains active");

    writeStartupLog("UI initialized");
    bool running = true;
    while (running) {
        pollNetwork();
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

    if (g_socket >= 0) close(g_socket);
    if (g_gameController) SDL_GameControllerClose(g_gameController);
    if (g_controller) SDL_JoystickClose(g_controller);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    writeStartupLog("AMEGA normal exit");
    return 0;
}
