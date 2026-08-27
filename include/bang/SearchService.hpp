#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bang {

class SearchService {
public:
    struct Hit {
        std::string videoId;
        std::string title;
        std::string uploader;
        std::int64_t durationSec = 0;
    };

    [[nodiscard]] std::vector<Hit> searchYouTube(
        const std::string& query, std::size_t limit = 20) const;
};

} // namespace bang
