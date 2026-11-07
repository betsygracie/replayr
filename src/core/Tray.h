#pragma once
#include <cstdint>
#include <string>

namespace cs {

enum class TrayCommand : int {
    None = 0,
    SaveReplay,
    ToggleRecording,
    ToggleReplayBuffer,
    Screenshot,
    ScreenshotRegion,
    OpenApp,
    OpenFolder,
    Quit,
};

struct TrayMenuState {
    bool recording = false;
    bool bufferOn = false;
};

class Tray {
public:
    bool Create(void* hwnd, void* hinstance, const std::wstring& tooltip);
    void Remove();
    void Recreate();                        // after Explorer restarts ("TaskbarCreated")
    void SetTooltip(const std::wstring& tooltip);
    TrayCommand ShowMenu(const TrayMenuState& state);   // blocking popup menu at the cursor

private:
    void* hwnd_ = nullptr;
    void* hinst_ = nullptr;
    void* icon_ = nullptr;
    std::wstring tooltip_;
    bool added_ = false;
};

} // namespace cs
