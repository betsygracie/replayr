#pragma once
#include <memory>
#include <string>

#include "media/Banner.h"

namespace cs {

enum class ShotMode { FullScreen, ActiveWindow, Region };

struct ShotRequest {
    ShotMode mode = ShotMode::FullScreen;
    std::wstring folder;          // where the file goes
    std::wstring baseName;        // e.g. game name; a timestamp is appended
    bool copyToClipboard = true;
    bool jpeg = false;
    float accent[3] = { 0.545f, 0.247f, 0.988f };   // region selection colour
    bool banner = false;          // stamp the "Clipped using Replayr" banner
    BannerStyle bannerStyle;
};

struct ShotResult {
    bool ok = false;
    bool cancelled = false;
    bool copied = false;
    std::wstring path;
    std::string error;
    int width = 0, height = 0;
};

// Screenshots without any extra dependency:
// - Full screen / active window: DXGI Desktop Duplication of the monitor (works for borderless
//   and most fullscreen games), with a GDI fallback. Window shots are cropped from the monitor.
// - Region: freezes the whole desktop with GDI and shows a dimmed overlay to drag a rectangle.
// PNG/JPG encoding (WIC) happens on a worker thread; the clipboard copy happens in PollResult()
// on the UI thread because the clipboard needs a window to own it.
class Screenshotter {
public:
    Screenshotter();
    ~Screenshotter();
    void Init(void* hinstance, void* mainHwnd);
    void Shutdown();
    void Capture(const ShotRequest& req);
    bool SelectingRegion() const;
    bool PollResult(ShotResult& out);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
