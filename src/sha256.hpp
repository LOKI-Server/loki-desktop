#pragma once
// SHA-256, as LOKI names blobs: lowercase hex of the digest.
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace loki {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::string hex();

    static std::string of(std::string_view data);
    /** Hashes a whole file; throws std::runtime_error when it cannot be read. */
    static std::string of_file(const std::filesystem::path& file);

private:
    void block(const std::uint8_t* chunk);
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

}  // namespace loki
