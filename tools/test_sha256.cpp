#include "../src/ps4/receiver/sha256.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

static std::string hex(const uint8_t digest[32])
{
    static const char* digits = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (size_t i = 0; i < 32; ++i) {
        output.push_back(digits[digest[i] >> 4]);
        output.push_back(digits[digest[i] & 0x0f]);
    }
    return output;
}

static bool check(const char* name, const void* bytes, size_t length, const char* expected)
{
    AmegaSha256 hash;
    if (!hash.update(bytes, length)) {
        std::fprintf(stderr, "%s: update failed\n", name);
        return false;
    }
    std::array<uint8_t, 32> digest{};
    if (!hash.finish(digest.data())) {
        std::fprintf(stderr, "%s: finish failed\n", name);
        return false;
    }
    const std::string actual = hex(digest.data());
    if (actual != expected) {
        std::fprintf(stderr, "%s: expected %s, got %s\n", name, expected, actual.c_str());
        return false;
    }
    return true;
}

int main()
{
    bool ok = true;
    ok &= check("empty", "", 0,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ok &= check("abc", "abc", 3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // NIST/RFC-style long-input test: one million ASCII 'a' bytes.
    AmegaSha256 million;
    char block[1000];
    std::memset(block, 'a', sizeof(block));
    for (int i = 0; i < 1000; ++i)
        if (!million.update(block, sizeof(block))) return 2;
    std::array<uint8_t, 32> longDigest{};
    if (!million.finish(longDigest.data()) ||
        hex(longDigest.data()) !=
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") {
        std::fprintf(stderr, "million-a: digest mismatch\n");
        ok = false;
    }

    // Same byte stream, deliberately split at uneven boundaries.
    AmegaSha256 segmented;
    const char* sample = "A MEGA PS4 resume test stream";
    const size_t size = std::strlen(sample);
    for (size_t offset = 0; offset < size;) {
        const size_t chunk = (offset % 5) + 1;
        const size_t take = chunk < size - offset ? chunk : size - offset;
        if (!segmented.update(sample + offset, take)) return 3;
        offset += take;
    }
    std::array<uint8_t, 32> segmentedDigest{};
    std::array<uint8_t, 32> singleDigest{};
    AmegaSha256 single;
    single.update(sample, size);
    segmented.finish(segmentedDigest.data());
    single.finish(singleDigest.data());
    if (segmentedDigest != singleDigest) {
        std::fprintf(stderr, "segmented-update: digest mismatch\n");
        ok = false;
    }

    if (!ok) return 1;
    std::puts("AMEGA SHA-256 self-tests passed");
    return 0;
}
