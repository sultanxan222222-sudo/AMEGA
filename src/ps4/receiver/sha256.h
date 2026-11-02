#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Small, allocation-free SHA-256 implementation for AMEGA's PS4 receiver.
// The implementation follows the SHA-256 specification (FIPS 180-4).
// It is incremental so multi-gigabyte PKG files are never loaded into RAM.
class AmegaSha256
{
public:
    AmegaSha256() { reset(); }

    void reset()
    {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
        bitLength_ = 0;
        buffered_ = 0;
        finished_ = false;
    }

    bool update(const void* input, size_t length)
    {
        if (finished_ || (input == nullptr && length != 0))
            return false;

        const auto* data = static_cast<const uint8_t*>(input);
        if (length > (UINT64_MAX - bitLength_) / 8u)
            return false;

        bitLength_ += static_cast<uint64_t>(length) * 8u;
        while (length > 0) {
            const size_t available = 64u - buffered_;
            const size_t take = length < available ? length : available;
            std::memcpy(buffer_ + buffered_, data, take);
            buffered_ += take;
            data += take;
            length -= take;

            if (buffered_ == 64u) {
                transform_(buffer_);
                buffered_ = 0;
            }
        }
        return true;
    }

    bool finish(uint8_t output[32])
    {
        if (finished_ || output == nullptr)
            return false;

        const uint64_t originalBitLength = bitLength_;
        buffer_[buffered_++] = 0x80u;

        if (buffered_ > 56u) {
            while (buffered_ < 64u)
                buffer_[buffered_++] = 0;
            transform_(buffer_);
            buffered_ = 0;
        }

        while (buffered_ < 56u)
            buffer_[buffered_++] = 0;

        for (int i = 7; i >= 0; --i) {
            buffer_[buffered_++] = static_cast<uint8_t>(
                (originalBitLength >> (static_cast<unsigned>(i) * 8u)) & 0xffu);
        }

        transform_(buffer_);
        buffered_ = 0;
        for (size_t i = 0; i < 8; ++i) {
            output[i * 4 + 0] = static_cast<uint8_t>(state_[i] >> 24);
            output[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            output[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            output[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        finished_ = true;
        return true;
    }

private:
    static uint32_t rotateRight_(uint32_t value, unsigned amount)
    {
        return (value >> amount) | (value << (32u - amount));
    }

    static uint32_t loadBigEndian_(const uint8_t* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    void transform_(const uint8_t block[64])
    {
        static constexpr uint32_t constants[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
            0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
            0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
            0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
            0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
            0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
        };

        uint32_t words[64] = {};
        for (size_t i = 0; i < 16; ++i)
            words[i] = loadBigEndian_(block + i * 4);

        for (size_t i = 16; i < 64; ++i) {
            const uint32_t x = words[i - 15];
            const uint32_t y = words[i - 2];
            const uint32_t sigma0 = rotateRight_(x, 7) ^ rotateRight_(x, 18) ^ (x >> 3);
            const uint32_t sigma1 = rotateRight_(y, 17) ^ rotateRight_(y, 19) ^ (y >> 10);
            words[i] = words[i - 16] + sigma0 + words[i - 7] + sigma1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t sum1 = rotateRight_(e, 6) ^ rotateRight_(e, 11) ^ rotateRight_(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + sum1 + choice + constants[i] + words[i];
            const uint32_t sum0 = rotateRight_(a, 2) ^ rotateRight_(a, 13) ^ rotateRight_(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;

            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    uint32_t state_[8] = {};
    uint64_t bitLength_ = 0;
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;
    bool finished_ = false;
};
