#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace cs {

// Message the tray icon sends to the main window (WM_APP + 1), and the mouse messages it carries.
constexpr uint32_t kTrayCallbackMessage = 0x8000 + 1;
constexpr uint32_t kTrayLeftUp = 0x0202;       // WM_LBUTTONUP
constexpr uint32_t kTrayLeftDouble = 0x0203;   // WM_LBUTTONDBLCLK
constexpr uint32_t kTrayRightUp = 0x0205;      // WM_RBUTTONUP
constexpr uint32_t kTrayContextMenu = 0x007B;  // WM_CONTEXTMENU

struct HitRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Filled in by the UI every frame (client pixels) so WM_NCHITTEST knows which parts of the custom
// title bar drag the window (HTCAPTION), which are ImGui widgets (HTCLIENT) and where the maximize
// button is (HTMAXBUTTON: lets Windows 11 show its Snap Layouts flyout on hover).
struct CaptionLayout {
    float height = 0.0f;
    HitRect maxButton;
    std::vector<HitRect> interactive;
};

enum class Backdrop { Gradient = 0, Mica = 1, Acrylic = 2, MicaAlt = 3 };

// "Press a key..." capture for the hotkey binder. Handled inside the window procedure so the
// combo is read straight from WM_KEYDOWN/WM_SYSKEYDOWN, before ImGui or registered hotkeys see it.
struct KeyCapture {
    bool active = false;
    bool done = false;
    bool cancelled = false;   // Esc
    bool cleared = false;     // Backspace
    uint32_t vk = 0;
    uint32_t mods = 0;        // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN bits
};

struct WindowCallbacks {
    std::function<void(int id)> onHotkey;
    std::function<void(uint32_t mouseMsg)> onTrayMessage;
    std::function<void()> onTaskbarCreated;
    std::function<void()> onCloseRequested;
    std::function<void()> onShowRequest;        // a second Replayr.exe asked us to show up
    std::function<void()> onRenderWhileSizing;  // keep painting during the modal resize loop
    std::function<void()> onEndSession;
};

// Borderless Win32 window + D3D11 device + swap chain.
// Uses a DirectComposition swap chain with premultiplied alpha when possible, which is what lets
// Windows 11 Mica/Acrylic show through the UI ("glass" backdrop). Falls back to a normal flip-model
// swap chain otherwise.
class Window {
public:
    Window();
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(void* hinstance, const wchar_t* title, int x, int y, int w, int h, bool maximized, bool show);
    void Destroy();

    void* Hwnd() const;
    void* Hinstance() const;
    ID3D11Device* Device() const;
    ID3D11DeviceContext* Context() const;

    bool UsesComposition() const;
    bool SupportsBackdrop() const;     // Windows 11 22H2+ with composition
    void SetBackdrop(Backdrop b);

    void BeginFrame(const float clearRgba[4]);
    void Present(bool vsync);

    float DpiScale() const;
    bool TakeDpiChanged();             // true once after the window moved to a monitor with another DPI

    bool IsVisible() const;
    bool Minimized() const;
    bool Maximized() const;
    bool IsFocused() const;
    bool IsMaxButtonPressed() const;
    int Width() const;
    int Height() const;

    void ShowInitial(bool maximized);   // first show, after the first frame has been rendered
    void Show();
    void Hide();
    void Minimize();
    void ToggleMaximize();
    // True fullscreen on the window's monitor (no frame, no rounded corners, covers the taskbar).
    void SetFullscreen(bool on);
    bool IsFullscreen() const;
    void Close();                       // destroys the window -> WM_QUIT
    void GetNormalRect(int& x, int& y, int& w, int& h, bool& maximized) const;

    // Message loop helpers (keep <windows.h> out of the UI code).
    static bool PumpMessages();                 // false once WM_QUIT arrives
    static void WaitForMessages(unsigned ms);   // sleep until input/messages or timeout

    CaptionLayout& Caption();
    KeyCapture& Capture();

    WindowCallbacks callbacks;
    bool inFrame = false;               // set by the app between NewFrame() and Present()

    struct Impl;
    Impl* impl() const { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
