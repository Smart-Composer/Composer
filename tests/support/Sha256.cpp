#include "Sha256.h"

#include <array>
#include <cstdint>

namespace composer::tests
{
namespace
{

constexpr std::array<std::uint32_t, 64> roundConstants {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

constexpr std::uint32_t rotateRight(std::uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void compress(std::array<std::uint32_t, 8>& hash, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> schedule {};

    for (std::size_t index = 0; index < 16; ++index)
        schedule[index] = (static_cast<std::uint32_t>(block[4 * index]) << 24)
                        | (static_cast<std::uint32_t>(block[4 * index + 1]) << 16)
                        | (static_cast<std::uint32_t>(block[4 * index + 2]) << 8)
                        | static_cast<std::uint32_t>(block[4 * index + 3]);

    for (std::size_t index = 16; index < 64; ++index)
    {
        const auto w15 = schedule[index - 15];
        const auto w2 = schedule[index - 2];
        const auto s0 = rotateRight(w15, 7) ^ rotateRight(w15, 18) ^ (w15 >> 3);
        const auto s1 = rotateRight(w2, 17) ^ rotateRight(w2, 19) ^ (w2 >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = hash;

    for (std::size_t index = 0; index < 64; ++index)
    {
        const auto s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto first = h + s1 + choice + roundConstants[index] + schedule[index];
        const auto s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto second = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }

    const std::array<std::uint32_t, 8> added { a, b, c, d, e, f, g, h };

    for (std::size_t index = 0; index < 8; ++index)
        hash[index] += added[index];
}

} // namespace

std::string sha256Hex(const void* data, std::size_t size)
{
    std::array<std::uint32_t, 8> hash { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;

    for (; size - offset >= 64; offset += 64)
        compress(hash, bytes + offset);

    // The final blocks: the remaining bytes, a single 1 bit, zeros, and the length in bits.
    std::array<std::uint8_t, 128> tail {};
    const std::size_t remaining = size - offset;

    for (std::size_t index = 0; index < remaining; ++index)
        tail[index] = bytes[offset + index];

    tail[remaining] = 0x80;
    const std::size_t tailSize = remaining < 56 ? 64 : 128;
    const auto bits = static_cast<std::uint64_t>(size) * 8u;

    for (std::size_t index = 0; index < 8; ++index)
        tail[tailSize - 1 - index] = static_cast<std::uint8_t>(bits >> (8 * index));

    for (std::size_t block = 0; block < tailSize; block += 64)
        compress(hash, tail.data() + block);

    constexpr auto digits = "0123456789abcdef";
    std::string text;
    text.reserve(64);

    for (const auto word : hash)
        for (int shift = 28; shift >= 0; shift -= 4)
            text.push_back(digits[(word >> shift) & 0xfu]);

    return text;
}

} // namespace composer::tests
