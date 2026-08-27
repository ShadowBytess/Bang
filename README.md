# Bang

Bang is a native C++23 music application for Wayland. It acquires music —
paste a YouTube or Spotify link, or search YouTube from inside the app —
stores normal audio as `.mp3` files with embedded tags and cover art, while preserving tracker `.mod` files, and plays
it back from an indexed local library with playlists and favorites.

The renderer, text engine, input handling, and interface are Bang's own
retained-UI stack: a Vulkan instanced-quad pipeline with a glyph atlas,
FreeType/HarfBuzz text shaping over embedded JetBrains Mono, and a Wayland
xdg-shell front end. There is no widget toolkit and no scene-graph library.

## Build

Install the following development dependencies before configuring:

- CMake 3.25 or newer, GNU Make, and a compiler toolchain with C++23 and
  link-time-optimization support;
- the Vulkan loader, headers, and `glslc`;
- the Wayland client library, `wayland-protocols`, and `wayland-scanner`;
- SQLite, TagLib, and GStreamer 1.20 or newer (`gst-plugins-base` and
  `gst-plugins-good` at runtime for MP3 decoding and a GStreamer plugin capable of MOD/tracker playback for `.mod` files);
- FreeType, HarfBuzz, and xkbcommon; and
- the external acquisition tools on `PATH`: `yt-dlp`, `spotdl`, and
  `ffmpeg` (both backends shell out to them and to ffmpeg for MP3
  extraction and thumbnail conversion).

The build downloads checksum-pinned JetBrains Mono sources and embeds the
regular and bold faces, plus the compiled SPIR-V shaders, in the Bang
executable.

```sh
cmake --workflow --preset debug
./build/debug/bang
```

Use `cmake --workflow --preset release` for an optimized build and
`cmake --workflow --preset sanitizer` for the AddressSanitizer/UBSan build
and test run. `sh build.sh` wraps the common cases (`run`, `test`,
`install`, `update`).

Set `BANG_VULKAN_VALIDATION=1` to run against the Khronos validation layer
when it is installed.

## Code layout

- `src/app/` composes the application: screens, dialogs, player bar.
- `src/library/` owns the domain: the SQLite store and catalog, the
  content-addressed track importer, TagLib metadata, the download service
  with its yt-dlp/spotdl backends, and YouTube search.
- `src/playback/` wraps GStreamer `playbin` playback.
- Playlist import/export supports M3U/M3U8 files, including local `.MOD` tracker files.

- `src/ui/` is the immediate-mode interface layer (theme, widgets, layout
  helpers) that emits draw instances.
- `src/text/` shapes and rasterizes text (HarfBuzz + FreeType).
- `src/renderer/` owns the Vulkan device, swapchain, atlas, and pipeline.
- `src/platform/wayland/` owns the window, seat input, and xdg-shell
  lifecycle.
- `src/core/` contains process execution, hashing, and XDG paths.

## Library data

Bang stores its data under `$XDG_DATA_HOME/bang`, or `~/.local/share/bang`
when `XDG_DATA_HOME` is unset:

- `library.sqlite3` contains tracks, playlists, favorites, download
  history, and settings.
- `tracks/` contains content-addressed copies of imported audio
  (SHA-256-named audio files, preserving `.mp3` and `.mod` extensions).
- `artwork/` contains extracted cover art per track.
- `tmp/` holds in-progress downloads and is cleaned as jobs finish.

Downloading a playlist URL imports every entry; duplicate content is
deduplicated by hash, never re-copied.

## Status

Working: library with live filter, playlists, favorites, download queue
with progress, YouTube search dialog, add-by-URL with backend choice,
playback with seek/volume, auto-tagging with embedded artwork.

Planned: album/artist grouping, MPRIS desktop controls, drag-to-reorder
playlists, artwork thumbnails in lists, embedded GStreamer.

## License

Apache License 2.0. See [LICENSE](LICENSE).
