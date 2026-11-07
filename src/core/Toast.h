#pragma once
#include <memory>
#include <string>

namespace cs {

enum class ToastKind { Success, Info, Recording, Error };

struct ToastStyle {
    float accent[3] = { 0.545f, 0.247f, 0.988f };
    float accent2[3] = { 0.761f, 0.122f, 0.878f };
    int corner = 1;   // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
};

// Small "Clip saved - 30s" notifications drawn in their own layered, click-through, topmost window.
// WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE means they never take focus from the game. They appear on
// the monitor of the foreground window. Rendered with GDI+ once, then only moved/faded.
class Toasts {
public:
    Toasts();
    ~Toasts();
    bool Init(void* hinstance);
    void Shutdown();
    void SetStyle(const ToastStyle& style);
    // imagePath: optional JPEG/PNG shown as a thumbnail (the saved clip's poster frame or the screenshot).
    void Show(ToastKind kind, const std::string& title, const std::string& body, const char* iconUtf8 = nullptr,
              const std::wstring& imagePath = std::wstring());

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
