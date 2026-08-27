#include "bang/Hash.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace bang {

namespace {

constexpr std::uint32_t roundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32 - bits));
}

void transform(std::uint32_t state[8], const unsigned char block[64])
{
    std::array<std::uint32_t, 64> schedule{};
    for (int i = 0; i < 16; ++i) {
        schedule[i] = static_cast<std::uint32_t>(block[i * 4]) << 24
            | static_cast<std::uint32_t>(block[i * 4 + 1]) << 16
            | static_cast<std::uint32_t>(block[i * 4 + 2]) << 8
            | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotateRight(schedule[i - 15], 7)
            ^ rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const std::uint32_t s1 = rotateRight(schedule[i - 2], 17)
            ^ rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + roundConstants[i] + schedule[i];
        const std::uint32_t s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void appendHex(std::string& target, std::uint32_t value)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        target.push_back(digits[(value >> shift) & 0xf]);
    }
}

} // namespace

Sha256::Sha256()
{
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
}

void Sha256::update(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    bitCount_ += static_cast<std::uint64_t>(size) * 8;
    while (size > 0) {
        const std::size_t take = std::min(size, std::size_t{64} - buffered_);
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == 64) {
            transform(state_, buffer_);
            buffered_ = 0;
        }
    }
}

std::string Sha256::finish()
{
    const std::uint64_t bitCount = bitCount_;
    const unsigned char padding = 0x80;
    update(&padding, 1);
    const unsigned char zero = 0;
    while (buffered_ != 56) {
        update(&zero, 1);
    }
    bitCount_ = 0;
    unsigned char lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = static_cast<unsigned char>((bitCount >> (56 - i * 8)) & 0xff);
    }
    update(lengthBytes, 8);

    std::string digest;
    digest.reserve(64);
    for (const std::uint32_t word : state_) {
        appendHex(digest, word);
    }
    return digest;
}

std::string sha256File(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open file for hashing: " + path.string());
    }
    Sha256 hash;
    std::array<char, 65536> chunk{};
    while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || file.gcount() > 0) {
        hash.update(chunk.data(), static_cast<std::size_t>(file.gcount()));
        if (!file) {
            break;
        }
    }
    return hash.finish();
}

} // namespace bang
