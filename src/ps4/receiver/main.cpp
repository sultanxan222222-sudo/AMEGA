#include <SDL2/SDL.h>
#include <orbis/libkernel.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "font_ru.h"

static const int DISCOVERY_PORT = 38470;
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
static char g_status[128] = "ПОИСК КОМПЬЮТЕРА В СЕТИ";
static Uint32 g_lastBeacon = 0;
static SDL_Joystick* g_controller = nullptr;

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

static void sendAnnouncement()
{
    if (g_socket < 0) return;
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"protocol\":\"amega\",\"major\":1,"
        "\"type\":\"receiver_announcement\","
        "\"receiver_id\":\"%s\",\"instance_nonce\":\"%s\","
        "\"display_name\":\"PS4 PRO\",\"receiver_version\":\"0.1.0\"}",
        g_receiverId, g_nonce);

    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(DISCOVERY_PORT);
    target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(g_socket, payload, strlen(payload), 0,
        reinterpret_cast<sockaddr*>(&target), sizeof(target));
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
        strlen(nonce) < 32)
        return;

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

    if (g_selected < 0) g_selected = slot;
    snprintf(g_status, sizeof(g_status), "НАЙДЕН КОМПЬЮТЕР: %s", name);
}

static void pollNetwork()
{
    const Uint32 now = SDL_GetTicks();
    if (now - g_lastBeacon >= 3000) {
        sendAnnouncement();
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
    setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(g_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(DISCOVERY_PORT);
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

static void drawUi(SDL_Renderer* renderer)
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
    drawText(renderer, 64, 620, "X - ВЫБРАТЬ ПК     O - СБРОСИТЬ ВЫБОР", TEXT, 1);
    drawText(renderer, 64, 660, "ПЕРЕДАЧА ФАЙЛОВ БУДЕТ ВКЛЮЧЕНА В СЛЕДУЮЩЕМ ЭТАПЕ", MUTED, 1);
    SDL_RenderPresent(renderer);
}

static void handleController(SDL_Event& event)
{
    if (event.type != SDL_JOYBUTTONDOWN) return;

    switch (event.jbutton.button) {
        case 13: // D-pad up
            if (g_selected >= 0) {
                for (int n = 1; n <= MAX_SENDERS; ++n) {
                    int candidate = (g_selected - n + MAX_SENDERS) % MAX_SENDERS;
                    if (g_senders[candidate].used) { g_selected = candidate; break; }
                }
            }
            break;
        case 14: // D-pad down
            if (g_selected >= 0) {
                for (int n = 1; n <= MAX_SENDERS; ++n) {
                    int candidate = (g_selected + n) % MAX_SENDERS;
                    if (g_senders[candidate].used) { g_selected = candidate; break; }
                }
            }
            break;
        case 0: // Cross: select a discovered PC; transfer remains a separate future step.
            if (g_selected >= 0 && g_senders[g_selected].used)
                snprintf(g_status, sizeof(g_status), "ВЫБРАН ПК: %s", g_senders[g_selected].name);
            break;
        case 1: // Circle: clear the selection.
            g_selected = -1;
            snprintf(g_status, sizeof(g_status), "ВЫБОР СБРОШЕН");
            break;
        default:
            break;
    }
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        printf("AMEGA: SDL_Init failed: %s\n", SDL_GetError());
        for (;;) sceKernelUsleep(1000000);
    }

    SDL_Window* window = SDL_CreateWindow(
        "AMEGA PS4 Receiver",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        1280, 720, 0);
    if (!window) {
        printf("AMEGA: SDL_CreateWindow failed: %s\n", SDL_GetError());
        for (;;) sceKernelUsleep(1000000);
    }

    SDL_Surface* surface = SDL_GetWindowSurface(window);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) {
        printf("AMEGA: renderer failed: %s\n", SDL_GetError());
        for (;;) sceKernelUsleep(1000000);
    }

    SDL_JoystickEventState(SDL_ENABLE);
    if (SDL_NumJoysticks() > 0) g_controller = SDL_JoystickOpen(0);
    loadOrCreateReceiverId();
    openDiscoverySocket();
    if (g_socket < 0) {
        // The UI still launches and explains the networking failure.
    }

    for (;;) {
        pollNetwork();
        SDL_Event event;
        while (SDL_PollEvent(&event)) handleController(event);
        drawUi(renderer);
        SDL_UpdateWindowSurface(window);
        SDL_Delay(33);
    }

    return 0;
}
