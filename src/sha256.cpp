#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace loki {
namespace {

constexpr std::uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const std::uint8_t* chunk) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = std::uint32_t(chunk[i * 4]) << 24 | std::uint32_t(chunk[i * 4 + 1]) << 16 |
               std::uint32_t(chunk[i * 4 + 2]) << 8 | std::uint32_t(chunk[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (int i = 0; i < 64; ++i) {
        const auto t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const auto t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::uint32_t add[8] = {a, b, c, d, e, f, g, h};
    for (int i = 0; i < 8; ++i) state_[i] += add[i];
}

void Sha256::update(const void* data, std::size_t size) {
    auto bytes = static_cast<const std::uint8_t*>(data);
    length_ += size;
    while (size > 0) {
        const auto take = std::min(size, buffer_.size() - buffered_);
        std::copy(bytes, bytes + take, buffer_.begin() + buffered_);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == buffer_.size()) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

std::string Sha256::hex() {
    const std::uint64_t bits = length_ * 8;
    const std::uint8_t pad = 0x80;
    update(&pad, 1);
    const std::uint8_t zero = 0;
    while (buffered_ != 56) update(&zero, 1);
    std::uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = std::uint8_t(bits >> (56 - 8 * i));
    update(tail, 8);
    std::string out;
    out.reserve(64);
    char digits[3];
    for (auto word : state_) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            std::snprintf(digits, sizeof digits, "%02x", unsigned((word >> shift) & 0xff));
            out += digits;
        }
    }
    return out;
}

std::string Sha256::of(std::string_view data) {
    Sha256 h;
    h.update(data.data(), data.size());
    return h.hex();
}

std::string Sha256::of_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + file.string());
    Sha256 h;
    std::vector<char> buffer(64 * 1024);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        h.update(buffer.data(), std::size_t(in.gcount()));
    }
    if (in.bad()) throw std::runtime_error("Cannot read " + file.string());
    return h.hex();
}

}  // namespace loki
