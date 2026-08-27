cmake_minimum_required(VERSION 3.25)

project(bang VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(
    BANG_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)
function(bang_enable_sanitizers TARGET)
    if(NOT BANG_ENABLE_SANITIZERS)
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "The sanitizer preset requires GCC or Clang")
    endif()
    target_compile_options(
        ${TARGET}
        PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
    )
    target_link_options(
        ${TARGET}
        PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
    )
endfunction()

function(bang_apply_warnings TARGET)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            ${TARGET}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wno-missing-field-initializers
        )
    endif()
endfunction()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED COMPONENTS glslc)

pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
find_library(WAYLAND_CLIENT_LIBRARY NAMES wayland-client REQUIRED)
find_path(WAYLAND_CLIENT_INCLUDE_DIR wayland-client.h REQUIRED)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
find_file(
    XDG_SHELL_PROTOCOL
    NAMES xdg-shell.xml
    PATH_SUFFIXES
        wayland-protocols/stable/xdg-shell
        share/wayland-protocols/stable/xdg-shell
    REQUIRED
)

pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(XDG_SHELL_HEADER "${GENERATED_DIR}/xdg-shell-client-protocol.h")
set(XDG_SHELL_SOURCE "${GENERATED_DIR}/xdg-shell-protocol.c")
add_custom_command(
    OUTPUT "${XDG_SHELL_HEADER}" "${XDG_SHELL_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_PROTOCOL}" "${XDG_SHELL_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_PROTOCOL}" "${XDG_SHELL_SOURCE}"
    DEPENDS "${XDG_SHELL_PROTOCOL}"
    VERBATIM
)

include(FetchContent)
FetchContent_Declare(
    jetbrains_mono
    URL
        https://github.com/JetBrains/JetBrainsMono/archive/refs/tags/v2.304.tar.gz
    URL_HASH
        SHA256=d8dab5ff24fc9e345d6c7987f2746cf8df4f870561ea89c14630b8d1729fd727
    DOWNLOAD_EXTRACT_TIMESTAMP
        TRUE
    SOURCE_SUBDIR
        cmake-unused
)
FetchContent_MakeAvailable(jetbrains_mono)

add_library(
    bang-core
    src/core/Hash.cpp
    src/core/Paths.cpp
    src/core/ProcessRunner.cpp
    src/library/DownloadParsing.cpp
    src/library/DownloadService.cpp
    src/library/LibraryCatalog.cpp
    src/library/LibraryStore.cpp
    src/library/MetadataService.cpp
    src/library/PlaylistService.cpp
    src/library/SearchService.cpp
    src/library/TrackImporter.cpp
    src/playback/Player.cpp
)
add_library(bang::core ALIAS bang-core)
bang_enable_sanitizers(bang-core)
bang_apply_warnings(bang-core)
target_include_directories(
    bang-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/library"
)
target_link_libraries(
    bang-core
    PUBLIC
        PkgConfig::SQLITE
        PkgConfig::GSTREAMER
        Threads::Threads
    PRIVATE
        PkgConfig::TAGLIB
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(cmake/embed_spirv.cmake)

embed_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.vert" quad_vertex VERTEX_SHADER_HEADER)
embed_shader("${CMAKE_CURRENT_SOURCE_DIR}/shaders/quad.frag" quad_fragment FRAGMENT_SHADER_HEADER)

embed_binary_file(
    "${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf"
    jetbrains_mono_regular
    JETBRAINS_MONO_REGULAR_HEADER
)
embed_binary_file(
    "${jetbrains_mono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Bold.ttf"
    jetbrains_mono_bold
    JETBRAINS_MONO_BOLD_HEADER
)

add_library(
    bang-gfx
    src/text/TextEngine.cpp
    src/renderer/Renderer.cpp
    src/ui/Ui.cpp
    "${VERTEX_SHADER_HEADER}"
    "${FRAGMENT_SHADER_HEADER}"
)
add_library(bang::gfx ALIAS bang-gfx)
bang_enable_sanitizers(bang-gfx)
bang_apply_warnings(bang-gfx)
target_include_directories(
    bang-gfx
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    PRIVATE
        "${GENERATED_DIR}"
        "${WAYLAND_CLIENT_INCLUDE_DIR}"
)
target_link_libraries(
    bang-gfx
    PUBLIC
        bang::core
        Vulkan::Vulkan
        PkgConfig::FREETYPE
        PkgConfig::HARFBUZZ
        "${WAYLAND_CLIENT_LIBRARY}"
    PRIVATE
        "${VERTEX_SHADER_HEADER}"
        "${FRAGMENT_SHADER_HEADER}"
        "${JETBRAINS_MONO_REGULAR_HEADER}"
        "${JETBRAINS_MONO_BOLD_HEADER}"
)

add_executable(
    bang
    src/platform/wayland/Window.cpp
    "${XDG_SHELL_SOURCE}"
    "${XDG_SHELL_HEADER}"
    "${JETBRAINS_MONO_REGULAR_HEADER}"
    "${JETBRAINS_MONO_BOLD_HEADER}"
    src/app/main.cpp
)
bang_enable_sanitizers(bang)
bang_apply_warnings(bang)
target_compile_definitions(bang PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
target_include_directories(
    bang
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${GENERATED_DIR}"
        "${WAYLAND_CLIENT_INCLUDE_DIR}"
)
target_link_libraries(
    bang
    PRIVATE
        bang::core
        bang::gfx
        Vulkan::Vulkan
        PkgConfig::XKBCOMMON
        "${WAYLAND_CLIENT_LIBRARY}"
)

install(
    TARGETS bang
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    FILES bang.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
