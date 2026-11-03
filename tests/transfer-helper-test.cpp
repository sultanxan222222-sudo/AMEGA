#include "../src/ps4/receiver/transfer.h"
#include <stdio.h>
#include <string.h>

static bool checkDigest(const char* value, const char* expected) {
    amega_transfer::Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char*>(value), strlen(value));
    std::string actual = hash.finish();
    if (actual != expected) {
        fprintf(stderr, "SHA-256 mismatch for test input: %s\nexpected: %s\nactual:   %s\n",
            value, expected, actual.c_str());
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    ok = checkDigest("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") && ok;
    ok = checkDigest("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") && ok;
    ok = checkDigest("The quick brown fox jumps over the lazy dog",
        "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592") && ok;
    if (!ok) return 1;

    if (amega_transfer::safeFileName("../bad/name.pkg") != "___bad_name.pkg") {
        fprintf(stderr, "Filename sanitization test failed\n");
        return 2;
    }
    if (amega_transfer::headerValue("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 10-20/30\r\n",
        "content-range") != "bytes 10-20/30") {
        fprintf(stderr, "HTTP header lookup test failed\n");
        return 3;
    }
    puts("AMEGA transfer helper tests passed.");
    return 0;
}
