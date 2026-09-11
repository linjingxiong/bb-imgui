cmake_minimum_required(VERSION 3.22)
project(bb_imgui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Static CRT: the packaged .exe should run on a machine without the VC++
# Redistributable installed. Must be set before any targets (incl. FetchContent
# deps built as part of this project) are created.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# Dear ImGui — docking branch (DockSpace, dockable panels, multi-viewport).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE)

# GLFW — windowing / input.
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# nlohmann/json — .bbtheme parsing.
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)

# LZ4 — chunk decompression for mcap files written with lz4-compressed
# chunks (the default for many mcap writers, e.g. the official Foxglove SDKs;
# EgoViewer's own writer leaves chunks uncompressed, so this was skipped
# until a non-EgoViewer .mcap needed it).
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  build/cmake)

# wgpu-native — WebGPU implementation (prebuilt binaries).
set(WGPU_VERSION "v29.0.1.1")
set(WGPU_BASE "https://github.com/gfx-rs/wgpu-native/releases/download/${WGPU_VERSION}")
if(WIN32)
    set(WGPU_ZIP "wgpu-windows-x86_64-msvc-release.zip")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(WGPU_ZIP "wgpu-macos-aarch64-release.zip")
    else()
        set(WGPU_ZIP "wgpu-macos-x86_64-release.zip")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(WGPU_ZIP "wgpu-linux-aarch64-release.zip")
    else()
        set(WGPU_ZIP "wgpu-linux-x86_64-release.zip")
    endif()
endif()
FetchContent_Declare(wgpu_native URL "${WGPU_BASE}/${WGPU_ZIP}")

FetchContent_MakeAvailable(imgui glfw nlohmann_json wgpu_native lz4)

# ---------------------------------------------------------------------------
# wgpu-native imported target
# ---------------------------------------------------------------------------
add_library(wgpu_native INTERFACE)
target_include_directories(wgpu_native INTERFACE ${wgpu_native_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(wgpu_native INTERFACE
        ${wgpu_native_SOURCE_DIR}/lib/wgpu_native.lib
        d3dcompiler ws2_32 userenv bcrypt ntdll opengl32 propsys runtimeobject)
elseif(APPLE)
    target_link_libraries(wgpu_native INTERFACE
        ${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a
        "-framework Metal" "-framework QuartzCore" "-framework CoreFoundation")
else()
    target_link_libraries(wgpu_native INTERFACE
        ${wgpu_native_SOURCE_DIR}/lib/libwgpu_native.a m dl pthread)
endif()

# ---------------------------------------------------------------------------
# imgui static library (core + GLFW/WebGPU backends)
# ---------------------------------------------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_WGPU)
target_link_libraries(imgui PUBLIC glfw wgpu_native)

# ---------------------------------------------------------------------------
# MCAP (header-only) + FFmpeg (prebuilt, Windows) — for the mcap-player branch.
# FFmpeg binaries live in third_party/ffmpeg (git-ignored, ~220MB) — copy them
# from D:/Ego/EgoViewer/third_party/ffmpeg before building this branch.
# ---------------------------------------------------------------------------
add_library(mcap INTERFACE)
target_include_directories(mcap INTERFACE ${CMAKE_SOURCE_DIR}/third_party/mcap/include)
target_compile_definitions(mcap INTERFACE MCAP_COMPRESSION_NO_ZSTD)
target_link_libraries(mcap INTERFACE lz4)

set(FFMPEG_ROOT ${CMAKE_SOURCE_DIR}/third_party/ffmpeg/windows)
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${FFMPEG_ROOT}/include)
target_link_libraries(ffmpeg INTERFACE
    ${FFMPEG_ROOT}/lib/x64/avcodec.lib
    ${FFMPEG_ROOT}/lib/x64/avformat.lib
    ${FFMPEG_ROOT}/lib/x64/avutil.lib
    ${FFMPEG_ROOT}/lib/x64/swscale.lib
    ${FFMPEG_ROOT}/lib/x64/swresample.lib)

# DuckDB (prebuilt, ~37MB dll, git-ignored) — reads the parquet in LeRobot
# datasets. C API only (duckdb.h); no C++ ABI coupling.
set(DUCKDB_ROOT ${CMAKE_SOURCE_DIR}/third_party/duckdb/windows)
add_library(duckdb INTERFACE)
target_include_directories(duckdb INTERFACE ${DUCKDB_ROOT}/include)
target_link_libraries(duckdb INTERFACE ${DUCKDB_ROOT}/lib/duckdb.lib)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(bb_imgui
    src/main.cpp
    src/theme.cpp
    src/fonts.cpp
    src/shell.cpp
    src/menu.cpp
    src/bb.cpp
    src/gallery.cpp
    src/logo.cpp
    src/mcap_reader.cpp
    src/parquet.cpp
    src/video_decoder.cpp
    src/mp4_source.cpp
    src/image_column_source.cpp
    src/pixel_inspector.cpp
    src/pixel_sample.cpp
    src/recording.cpp
    src/mcap_recording.cpp
    src/lerobot_recording.cpp
    src/playback.cpp
    src/video_texture.cpp
    src/player_ui.cpp
    src/settings.cpp
)
target_link_libraries(bb_imgui PRIVATE imgui glfw wgpu_native nlohmann_json::nlohmann_json
    mcap ffmpeg duckdb)
if(WIN32)
    target_link_libraries(bb_imgui PRIVATE comdlg32 ole32) # GetOpenFileNameW + IFileDialog
endif()

if(MSVC)
    target_compile_options(bb_imgui PRIVATE /utf-8)
    target_compile_options(imgui PRIVATE /utf-8)
endif()

option(BB_PROFILE "Print per-frame timing breakdown to stderr" OFF)
if(BB_PROFILE)
    target_compile_definitions(bb_imgui PRIVATE BB_PROFILE)
endif()

# Assets next to the binary.
add_custom_command(TARGET bb_imgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:bb_imgui>/assets
    VERBATIM)

# FFmpeg runtime DLLs next to the binary.
foreach(dll avcodec-58 avformat-58 avutil-56 swscale-5 swresample-3)
    add_custom_command(TARGET bb_imgui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FFMPEG_ROOT}/lib/x64/${dll}.dll $<TARGET_FILE_DIR:bb_imgui>/${dll}.dll
        VERBATIM)
endforeach()

# DuckDB runtime DLL next to the binary.
add_custom_command(TARGET bb_imgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${DUCKDB_ROOT}/lib/duckdb.dll $<TARGET_FILE_DIR:bb_imgui>/duckdb.dll
    VERBATIM)
