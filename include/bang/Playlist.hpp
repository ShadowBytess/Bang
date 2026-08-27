#pragma once

#include <cstdint>
#include <string>

namespace bang {

struct Playlist {
    std::int64_t id = 0;
    std::string name;
    std::int64_t createdAtMs = 0;
};

} // namespace bang
