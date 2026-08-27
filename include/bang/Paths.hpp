#pragma once

#include <filesystem>

namespace bang {

std::filesystem::path dataDirectory();
std::filesystem::path temporaryDirectory();

} // namespace bang
