#include "bang/Paths.hpp"

#include <cstdlib>

namespace bang {

namespace {

std::filesystem::path homeDirectory()
{
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
    return std::filesystem::current_path();
}

} // namespace

std::filesystem::path dataDirectory()
{
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
        xdgDataHome != nullptr && *xdgDataHome != '\0') {
        return std::filesystem::path(xdgDataHome) / "bang";
    }
    return homeDirectory() / ".local" / "share" / "bang";
}

std::filesystem::path temporaryDirectory()
{
    return dataDirectory() / "tmp";
}

} // namespace bang
