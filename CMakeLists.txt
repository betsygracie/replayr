cmake_minimum_required(VERSION 3.24)
project(Replayr VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Replayr is a Windows-only app (Win32 + DirectX 11).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT, so Replayr.exe runs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# -----------------------------------------------------------------------------
# Dear ImGui (docking branch). Pinned so the build never breaks on an upstream
# API change. Offline build: put an ImGui checkout in vendor/imgui instead.
# -----------------------------------------------------------------------------
set(CS_IMGUI_TAG "v1.92.2b-docking")
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/imgui/imgui.h")
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/vendor/imgui")
    message(STATUS "Using vendored Dear ImGui: ${IMGUI_DIR}")
else()
    FetchContent_Declare(imgui
        URL "https://github.com/ocornut/imgui/archive/refs/tags/${CS_IMGUI_TAG}.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui)   # ImGui has no CMakeLists, so this only downloads it
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
endif()

# -----------------------------------------------------------------------------
# nlohmann/json (single header). Offline build: vendor/nlohmann/json.hpp
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/nlohmann/json.hpp")
    set(CS_JSON_INCLUDE "${CMAKE_SOURCE_DIR}/vendor")
else()
    set(CS_JSON_INCLUDE "${CMAKE_BINARY_DIR}/_deps/json-single")
    set(CS_JSON_HPP "${CS_JSON_INCLUDE}/nlohmann/json.hpp")
    if(NOT EXISTS "${CS_JSON_HPP}")
        message(STATUS "Downloading nlohmann/json v3.11.3 (single header)")
        file(DOWNLOAD "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
             "${CS_JSON_HPP}" STATUS _cs_status TLS_VERIFY ON)
        list(GET _cs_status 0 _cs_code)
        if(NOT _cs_code EQUAL 0)
            file(REMOVE "${CS_JSON_HPP}")
            message(FATAL_ERROR "Could not download json.hpp (${_cs_status}). "
                                "Download it by hand into vendor/nlohmann/json.hpp and run CMake again.")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# ImGui static library
# -----------------------------------------------------------------------------
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD   # no XInput polling: keeps idle CPU low
    UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(imgui PRIVATE /utf-8 /MP)
endif()

# -----------------------------------------------------------------------------
# Replayr
# -----------------------------------------------------------------------------
set(CS_SOURCES
    src/main.cpp

    src/core/Log.cpp
    src/core/Util.cpp
    src/core/Platform.cpp
    src/core/Window.cpp
    src/core/GlassRenderer.cpp
    src/core/Updater.cpp
    src/core/InputOverlay.cpp
    src/account/Account.cpp
    src/core/Settings.cpp
    src/core/Hotkeys.cpp
    src/core/Tray.cpp
    src/core/Toast.cpp
    src/core/Thumbnailer.cpp
    src/core/ClipLibrary.cpp
    src/core/Bookmarks.cpp
    src/core/GameDetector.cpp
    src/core/GameProfiles.cpp
    src/core/MediaPlayer.cpp
    src/core/Screenshot.cpp
    src/core/SystemStats.cpp

    src/engine/RealClipEngine.cpp
    src/engine/DesktopCapture.cpp
    src/engine/WindowCapture.cpp
    src/engine/DualClipEngine.cpp
    src/engine/NoiseSuppression.cpp
    src/engine/HighlightDetector.cpp
    src/engine/MicTest.cpp
    src/engine/MediaWriter.cpp
    src/engine/AudioInput.cpp
    src/engine/Encoder.cpp
    src/engine/AudioCapture.cpp
    src/engine/FFmpegTools.cpp
    src/engine/Transcoder.cpp

    src/media/MediaCache.cpp
    src/media/ClipAudio.cpp
    src/media/Banner.cpp

    src/ui/App.cpp
    src/ui/Theme.cpp
    src/ui/Anim.cpp
    src/ui/Widgets.cpp
    src/ui/ClipCard.cpp
    src/ui/TopBar.cpp
    src/ui/Sidebar.cpp
    src/ui/Library.cpp
    src/ui/LibraryView.cpp
    src/ui/Editor.cpp
    src/ui/ImageViewer.cpp
    src/ui/Welcome.cpp
    src/ui/UploadsView.cpp
    src/ui/SettingsView.cpp)

add_executable(Replayr WIN32 ${CS_SOURCES} assets/app.rc assets/Replayr.manifest)
target_include_directories(Replayr PRIVATE src ${CS_JSON_INCLUDE})
target_compile_definitions(Replayr PRIVATE
    WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(Replayr PRIVATE imgui rnnoise
    d3d11 dxgi dcomp dwmapi d3dcompiler winhttp ws2_32 bcrypt msimg32
    mfplat mfreadwrite mfuuid
    ole32 oleaut32 uuid shell32 shlwapi propsys windowscodecs
    gdiplus msimg32 pdh winmm shcore version advapi32 user32 gdi32)
if(MSVC)
    target_compile_options(Replayr PRIVATE /utf-8 /W3 /permissive- /MP /Zc:__cplusplus)
endif()

# Optional extras copied next to the .exe after each build:
#   vendor/ffmpeg/ffmpeg.exe  -> export/trim + test clips
#   assets/fonts/*.ttf        -> custom UI font (see assets/fonts/README.txt)
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/ffmpeg/ffmpeg.exe")
    add_custom_command(TARGET Replayr POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/vendor/ffmpeg/ffmpeg.exe" "$<TARGET_FILE_DIR:Replayr>")
endif()
add_custom_command(TARGET Replayr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:Replayr>/assets/fonts")

set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Replayr)

# RNNoise (BSD 3-clause, xiph.org): mic noise suppression. v0.1.1, with the model built in.
enable_language(C)
add_library(rnnoise STATIC
    third_party/rnnoise/src/denoise.c third_party/rnnoise/src/rnn.c third_party/rnnoise/src/rnn_data.c
    third_party/rnnoise/src/celt_lpc.c third_party/rnnoise/src/kiss_fft.c third_party/rnnoise/src/pitch.c)
target_include_directories(rnnoise PUBLIC third_party/rnnoise/include PRIVATE third_party/rnnoise/src)
if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0 /D_USE_MATH_DEFINES)
endif()
