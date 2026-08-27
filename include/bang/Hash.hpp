#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace bang {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::string finish();

private:
    std::uint32_t state_[8];
    std::uint64_t bitCount_ = 0;
    unsigned char buffer_[64];
    std::size_t buffered_ = 0;
};

std::string sha256File(const std::filesystem::path& path);

} // namespace bang
