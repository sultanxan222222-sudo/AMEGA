#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace amega_transfer {

struct Offer {
    std::string transferId;
    std::string fileName;
    std::string sha256;
    uint64_t fileSize;
    bool ready;
    Offer() : fileSize(0), ready(false) {}
};

static inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

class Sha256 {
public:
    Sha256() : bitLength(0), bufferLength(0) {
        state[0]=0x6a09e667; state[1]=0xbb67ae85; state[2]=0x3c6ef372; state[3]=0xa54ff53a;
        state[4]=0x510e527f; state[5]=0x9b05688c; state[6]=0x1f83d9ab; state[7]=0x5be0cd19;
    }

    void update(const unsigned char* data, size_t length) {
        for (size_t i=0; i<length; ++i) {
            buffer[bufferLength++] = data[i];
            if (bufferLength == 64) {
                transform(buffer);
                bitLength += 512;
                bufferLength = 0;
            }
        }
    }

    std::string finish() {
        uint64_t totalBits = bitLength + (uint64_t)bufferLength * 8;
        buffer[bufferLength++] = 0x80;
        if (bufferLength > 56) {
            while (bufferLength < 64) buffer[bufferLength++] = 0;
            transform(buffer);
            bufferLength = 0;
        }
        while (bufferLength < 56) buffer[bufferLength++] = 0;
        for (int i=7; i>=0; --i) buffer[bufferLength++] = (unsigned char)((totalBits >> (i*8)) & 0xff);
        transform(buffer);

        static const char hex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i=0; i<8; ++i) {
            for (int shift=28; shift>=0; shift-=4) out.push_back(hex[(state[i] >> shift) & 0x0f]);
        }
        return out;
    }

private:
    uint32_t state[8];
    uint64_t bitLength;
    unsigned char buffer[64];
    size_t bufferLength;

    void transform(const unsigned char block[64]) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        };
        uint32_t w[64];
        for (int i=0; i<16; ++i)
            w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
                   ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
        for (int i=16; i<64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
        uint32_t e=state[4], f=state[5], g=state[6], h=state[7];
        for (int i=0; i<64; ++i) {
            uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^(~e&g);
            uint32_t t1=h+s1+ch+k[i]+w[i];
            uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=s0+maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }
};

static inline bool sendAll(int fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static inline int connectTo(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1 ||
        connect(fd, (sockaddr*)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline std::string headerValue(const std::string& headers, const char* name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string::npos) end = headers.size();
        std::string line = headers.substr(pos, end-pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string field = line.substr(0, colon);
            std::transform(field.begin(), field.end(), field.begin(), ::tolower);
            if (field == key) {
                size_t value = line.find_first_not_of(" \t", colon+1);
                return value == std::string::npos ? std::string() : line.substr(value);
            }
        }
        if (end == headers.size()) break;
        pos = end + 2;
    }
    return std::string();
}

static inline bool jsonString(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return false;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    p = json.find('"', p+1);
    if (p == std::string::npos) return false;
    ++p;
    out.clear();
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p+1 < json.size()) {
            ++p;
            if (json[p] == 'n') out.push_back('\n');
            else if (json[p] == 'r') out.push_back('\r');
            else if (json[p] == 't') out.push_back('\t');
            else out.push_back(json[p]);
        } else out.push_back(json[p]);
        ++p;
    }
    return p < json.size();
}

static inline uint64_t jsonUnsigned(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return 0;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    return (uint64_t)strtoull(json.c_str()+p, nullptr, 10);
}

static inline std::string safeFileName(const std::string& input) {
    std::string out;
    for (size_t i=0; i<input.size() && out.size()<180; ++i) {
        unsigned char c = (unsigned char)input[i];
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out.push_back('_');
        else out.push_back((char)c);
    }
    while (out.find("..") != std::string::npos) out.replace(out.find(".."), 2, "__");
    if (out.empty() || out == "." || out == "..") out = "received.pkg";
    return out;
}

static inline bool fetchOffer(const char* ip, int port, Offer& offer, std::string& error) {
    int fd = connectTo(ip, port);
    if (fd < 0) { error = "Не удаётся соединиться с ПК"; return false; }

    char request[256];
    snprintf(request, sizeof(request),
        "GET /api/v1/offer HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", ip, port);
    if (!sendAll(fd, request, strlen(request))) {
        close(fd); error = "Ошибка запроса предложения"; return false;
    }

    std::string response;
    char buffer[4096];
    for (;;) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        response.append(buffer, (size_t)n);
        if (response.size() > 65536) { close(fd); error = "Слишком большой ответ ПК"; return false; }
    }
    close(fd);

    size_t split = response.find("\r\n\r\n");
    if (split == std::string::npos) { error = "Некорректный ответ ПК"; return false; }
    std::string headers = response.substr(0, split);
    if (headers.find(" 200 ") == std::string::npos) { error = "ПК пока не отправил игру"; return false; }
    std::string body = response.substr(split+4);
    std::string status;
    if (!jsonString(body, "status", status) || status != "ready") {
        offer.ready = false;
        return true;
    }
    if (!jsonString(body, "transfer_id", offer.transferId) ||
        !jsonString(body, "file_name", offer.fileName) ||
        !jsonString(body, "sha256", offer.sha256)) {
        error = "В предложении отсутствуют данные файла"; return false;
    }
    offer.fileSize = jsonUnsigned(body, "file_size");
    if (offer.fileSize == 0 || offer.sha256.size() != 64 || offer.transferId.empty()) {
        error = "Некорректные параметры файла"; return false;
    }
    offer.fileName = safeFileName(offer.fileName);
    offer.ready = true;
    return true;
}

static inline bool hashFile(const char* path, std::string& hex) {
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    Sha256 hash;
    unsigned char buffer[1024*1024];
    size_t n;
    while ((n=fread(buffer,1,sizeof(buffer),file)) > 0) hash.update(buffer,n);
    bool ok = !ferror(file);
    fclose(file);
    if (!ok) return false;
    hex = hash.finish();
    return true;
}

static inline bool downloadFile(const char* ip, int port, const Offer& offer,
    const std::function<void(uint64_t)>& progress, std::string& savedPath, std::string& error) {

    mkdir("/data/AMEGA", 0777);
    mkdir("/data/AMEGA/Received", 0777);

    std::string id;
    for (size_t i=0; i<offer.transferId.size(); ++i) {
        unsigned char c=(unsigned char)offer.transferId[i];
        if ((c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='-') id.push_back((char)c);
    }
    if (id.empty()) { error = "Некорректный номер передачи"; return false; }

    std::string partPath = "/data/AMEGA/Received/" + id + ".part";
    std::string finalPath = "/data/AMEGA/Received/" + offer.fileName;
    struct stat st;
    uint64_t offset = 0;
    if (stat(partPath.c_str(), &st) == 0 && st.st_size > 0) {
        offset = (uint64_t)st.st_size;
        if (offset > offer.fileSize) {
            unlink(partPath.c_str());
            offset = 0;
        }
    }
    if (offset == offer.fileSize) {
        std::string digest;
        if (!hashFile(partPath.c_str(), digest) || digest != offer.sha256) {
            error = "SHA-256 не совпал у сохранённой части"; return false;
        }
    } else {
        int fd = connectTo(ip, port);
        if (fd < 0) { error = "Не удаётся подключиться к серверу ПК"; return false; }

        char request[512];
        if (offset == 0)
            snprintf(request, sizeof(request),
                "GET /api/v1/file HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n", ip, port);
        else
            snprintf(request, sizeof(request),
                "GET /api/v1/file HTTP/1.1\r\nHost: %s:%d\r\nRange: bytes=%llu-\r\nConnection: close\r\n\r\n",
                ip, port, (unsigned long long)offset);

        if (!sendAll(fd, request, strlen(request))) {
            close(fd); error = "Не удалось отправить запрос файла"; return false;
        }

        std::string headers;
        std::string initialBody;
        char buffer[65536];
        size_t split = std::string::npos;
        while (split == std::string::npos) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); error = "ПК оборвал ответ до заголовков"; return false; }
            headers.append(buffer, (size_t)n);
            split = headers.find("\r\n\r\n");
            if (headers.size() > 32768) { close(fd); error = "Слишком большой HTTP-заголовок"; return false; }
        }
        initialBody = headers.substr(split+4);
        headers.resize(split);
        bool partial = headers.find(" 206 ") != std::string::npos;
        bool complete = headers.find(" 200 ") != std::string::npos;
        if ((offset == 0 && !complete) || (offset > 0 && !partial)) {
            close(fd); error = offset > 0 ? "Сервер не поддержал докачку HTTP Range" : "ПК отклонил передачу";
            return false;
        }
        if (offset > 0) {
            std::string range = headerValue(headers, "Content-Range");
            char expected[80];
            snprintf(expected, sizeof(expected), "bytes %llu-", (unsigned long long)offset);
            if (range.find(expected) != 0) {
                close(fd); error = "Неверный Content-Range от ПК"; return false;
            }
        }

        FILE* output = fopen(partPath.c_str(), offset == 0 ? "wb" : "ab");
        if (!output) { close(fd); error = "Не удалось создать файл на PS4"; return false; }

        uint64_t received = 0;
        uint64_t expectedBytes = offer.fileSize - offset;
        uint64_t persisted = 0;
        bool writeOk = true;

        if (!initialBody.empty()) {
            size_t amount = initialBody.size();
            if ((uint64_t)amount > expectedBytes) amount = (size_t)expectedBytes;
            if (fwrite(initialBody.data(), 1, amount, output) != amount) writeOk = false;
            received += amount;
            if (progress) progress(offset + received);
        }
        while (writeOk && received < expectedBytes) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            size_t amount = (size_t)n;
            if ((uint64_t)amount > expectedBytes-received) amount = (size_t)(expectedBytes-received);
            if (fwrite(buffer, 1, amount, output) != amount) { writeOk = false; break; }
            received += amount;
            if (received - persisted >= 4*1024*1024) {
                fflush(output);
                fsync(fileno(output));
                persisted = received;
            }
            if (progress) progress(offset + received);
        }
        fflush(output);
        fsync(fileno(output));
        fclose(output);
        close(fd);
        if (!writeOk) { error = "Ошибка записи на HDD PS4"; return false; }
        if (received != expectedBytes) {
            error = "Связь прервалась. Нажми X ещё раз — загрузка продолжится с места остановки.";
            return false;
        }
    }

    std::string digest;
    if (!hashFile(partPath.c_str(), digest)) { error = "Не удалось проверить SHA-256"; return false; }
    if (digest != offer.sha256) {
        error = "Ошибка целостности: SHA-256 не совпал. Файл оставлен как .part и не готов к установке.";
        return false;
    }

    if (access(finalPath.c_str(), F_OK) == 0) {
        char suffix[32];
        int index=1;
        do {
            snprintf(suffix, sizeof(suffix), " (%d)", index++);
            std::string candidate = "/data/AMEGA/Received/" + offer.fileName;
            size_t dot = candidate.find_last_of('.');
            if (dot == std::string::npos) candidate += suffix;
            else candidate.insert(dot, suffix);
            finalPath = candidate;
        } while (access(finalPath.c_str(), F_OK) == 0 && index < 1000);
    }
    if (rename(partPath.c_str(), finalPath.c_str()) != 0) {
        error = "Файл проверен, но не удалось переименовать его в итоговый файл"; return false;
    }
    savedPath = finalPath;
    if (progress) progress(offer.fileSize);
    return true;
}

} // namespace amega_transfer
