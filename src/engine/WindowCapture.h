#pragma once
// Windows Graphics Capture of one window (the game), for "Game only" recording: notifications,
// Discord pop-ups and other windows never appear. Free-threaded frame pool, polled from the
// capture thread (no dispatcher queue or message loop needed). Windows 10 1903+.
//
// The WinRT interfaces are declared by hand in WindowCapture.cpp (the toolchain has no
// C++/WinRT headers); only the handful of methods used are called.
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct HWND__;

namespace cs::engine {

class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();

    static bool Supported();

    // `device` must be the device the frames are copied with. Fails if the window can't be captured.
    bool Open(ID3D11Device* device, HWND__* window, bool cursor, bool hideBorder, std::string& error);
    void Close();
    bool IsOpen() const;
    HWND__* Window() const;

    // Copies the newest frame (if any) into `canvas` at (0,0), clipped to the canvas, and reports
    // the size of the window's content. Returns true if a new frame was copied. Handles the window
    // changing size (fullscreen <-> windowed) by recreating the frame pool.
    bool Poll(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas, int canvasW, int canvasH, int& contentW, int& contentH);

    bool Closed() const;            // the window went away (capture item closed)

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

// The window the user is most likely playing in: the foreground window if it's a large, visible,
// top-level window that isn't the desktop, the taskbar or Replayr itself. nullptr if none.
HWND__* FindGameWindow(unsigned long ownProcessId);
std::string WindowTitle(HWND__* w);

} // namespace cs::engine
