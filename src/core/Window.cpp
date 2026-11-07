#include "core/Window.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "AppInfo.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>

#include "imgui.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace cs {

// DWM attributes, as numbers so this builds with older Windows SDKs too.
static constexpr DWORD kDwmUseImmersiveDarkMode = 20;
static constexpr DWORD kDwmWindowCornerPreference = 33;
static constexpr DWORD kDwmBorderColor = 34;
static constexpr DWORD kDwmSystemBackdropType = 38;

struct Window::Impl {
    bool fullscreen = false, savedZoomed = false;
    WINDOWPLACEMENT savedPlacement{};
    Window* owner = nullptr;
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> dcompTarget;
    ComPtr<IDCompositionVisual> dcompVisual;
    bool composition = false;

    UINT width = 0, height = 0;
    UINT pendingW = 0, pendingH = 0;
    bool resizePending = false;
    bool inSizeMove = false;
    bool focused = true;
    UINT dpi = 96;
    bool dpiChanged = false;
    bool maxPressed = false;
    Backdrop backdrop = Backdrop::Gradient;

    UINT taskbarCreatedMsg = 0;
    UINT showRequestMsg = 0;

    CaptionLayout caption;
    KeyCapture capture;

    bool CreateDevice();
    bool CreateSwapChain();
    void CreateRenderTarget();
    void ApplyResize();
    void ApplyFrameMargins();
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);
    LRESULT HitTest(POINT screenPt) const;
};

static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Window::Impl* impl = reinterpret_cast<Window::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        impl = reinterpret_cast<Window::Impl*>(cs->lpCreateParams);
        impl->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
        EnableNonClientDpiScaling(hwnd);
    }
    if (impl) {
        LRESULT result = 0;
        if (impl->HandleMessage(msg, wp, lp, result)) return result;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

Window::Window() : impl_(std::make_unique<Impl>()) { impl_->owner = this; }
Window::~Window() { Destroy(); }

bool Window::Create(void* hinstance, const wchar_t* title, int x, int y, int w, int h, bool maximized, bool show)
{
    Impl& d = *impl_;
    d.hinst = (HINSTANCE)hinstance;
    d.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    d.showRequestMsg = RegisterWindowMessageW(CS_SHOW_MESSAGE);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = d.hinst;
    wc.hIcon = LoadIconW(d.hinst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageW(d.hinst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = CS_WINDOW_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("RegisterClassEx failed (%lu)", GetLastError());
        return false;
    }

    // WS_OVERLAPPEDWINDOW keeps Aero Snap, the DWM shadow, rounded corners and min/max animations.
    // WM_NCCALCSIZE below removes the standard frame so the whole window is ours to draw.
    DWORD exStyle = WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    d.hwnd = CreateWindowExW(exStyle, CS_WINDOW_CLASS, title, WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, d.hinst, &d);
    if (!d.hwnd) {
        Log("CreateWindowEx failed (%lu)", GetLastError());
        return false;
    }
    d.dpi = GetDpiForWindow(d.hwnd);

    BOOL dark = TRUE;
    DwmSetWindowAttribute(d.hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(d.hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
    COLORREF border = RGB(52, 44, 82);
    DwmSetWindowAttribute(d.hwnd, kDwmBorderColor, &border, sizeof(border));
    SetWindowPos(d.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    RECT rc;
    GetClientRect(d.hwnd, &rc);
    d.width = (UINT)std::max<LONG>(1, rc.right - rc.left);
    d.height = (UINT)std::max<LONG>(1, rc.bottom - rc.top);

    if (!d.CreateDevice() || !d.CreateSwapChain()) {
        Log("Direct3D 11 initialisation failed");
        return false;
    }
    d.CreateRenderTarget();
    d.ApplyFrameMargins();

    if (show) {
        ShowWindow(d.hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(d.hwnd);
    } else if (maximized) {
        // Remember the maximized state for when the window is shown from the tray.
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        GetWindowPlacement(d.hwnd, &wp);
        wp.showCmd = SW_HIDE;
        wp.flags |= WPF_RESTORETOMAXIMIZED;
        SetWindowPlacement(d.hwnd, &wp);
    }
    Log("Window created (%ux%u, dpi %u, composition %s)", d.width, d.height, d.dpi, d.composition ? "yes" : "no");
    return true;
}

void Window::Destroy()
{
    Impl& d = *impl_;
    if (d.context) d.context->ClearState();
    d.rtv.Reset();
    d.dcompVisual.Reset();
    d.dcompTarget.Reset();
    d.dcomp.Reset();
    d.swap.Reset();
    d.context.Reset();
    d.device.Reset();
    if (d.hwnd) {
        SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(d.hwnd);
        d.hwnd = nullptr;
    }
}

bool Window::Impl::CreateDevice()
{
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    // VIDEO_SUPPORT lets Media Foundation decode on this device (editor preview).
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device, &got, &context);
    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device, &got, &context);
    }
    if (FAILED(hr)) {
        Log("Hardware D3D11 device failed (0x%08lx), trying WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device, &got, &context);
    }
    if (FAILED(hr)) return false;

    // Media Foundation uses the immediate context from its own threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(context.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return true;
}

bool Window::Impl::CreateSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = factory->CreateSwapChainForComposition(device.Get(), &sd, nullptr, &swap);
    if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp));
    if (SUCCEEDED(hr)) hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget);
    if (SUCCEEDED(hr)) hr = dcomp->CreateVisual(&dcompVisual);
    if (SUCCEEDED(hr)) hr = dcompVisual->SetContent(swap.Get());
    if (SUCCEEDED(hr)) hr = dcompTarget->SetRoot(dcompVisual.Get());
    if (SUCCEEDED(hr)) hr = dcomp->Commit();
    if (SUCCEEDED(hr)) {
        composition = true;
        return true;
    }

    Log("DirectComposition unavailable (0x%08lx); using an opaque swap chain", (unsigned long)hr);
    dcompVisual.Reset();
    dcompTarget.Reset();
    dcomp.Reset();
    swap.Reset();
    composition = false;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &swap);
    if (FAILED(hr)) {
        Log("CreateSwapChainForHwnd failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

void Window::Impl::CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        device->CreateRenderTargetView(back.Get(), nullptr, &rtv);
}

void Window::Impl::ApplyResize()
{
    resizePending = false;
    if (pendingW == 0 || pendingH == 0) return;
    if (pendingW == width && pendingH == height) return;
    width = pendingW;
    height = pendingH;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    rtv.Reset();
    HRESULT hr = swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) Log("ResizeBuffers failed (0x%08lx)", (unsigned long)hr);
    CreateRenderTarget();
}

void Window::Impl::ApplyFrameMargins()
{
    // Mica/Acrylic need the frame extended over the whole client area ("sheet of glass").
    // Otherwise a 1px extension keeps the DWM drop shadow on Windows 10.
    MARGINS m = (backdrop != Backdrop::Gradient && composition) ? MARGINS{ -1, -1, -1, -1 } : MARGINS{ 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
}

LRESULT Window::Impl::HitTest(POINT pt) const
{
    RECT wr;
    GetWindowRect(hwnd, &wr);
    const int border = MulDiv(6, (int)dpi, 96);
    if (!IsZoomed(hwnd)) {
        bool left = pt.x < wr.left + border, right = pt.x >= wr.right - border;
        bool top = pt.y < wr.top + border, bottom = pt.y >= wr.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    POINT cp = pt;
    ScreenToClient(hwnd, &cp);
    const float x = (float)cp.x, y = (float)cp.y;
    auto inside = [&](const HitRect& r) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; };
    if (caption.height > 0.0f && y >= 0.0f && y < caption.height) {
        if (inside(caption.maxButton)) return HTMAXBUTTON;
        for (const HitRect& r : caption.interactive)
            if (inside(r)) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

static bool IsModifierKey(UINT vk)
{
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool Window::Impl::HandleMessage(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result)
{
    Window& w = *owner;

    // Hotkey binder: grab the next key combo before anything else sees it.
    if (capture.active) {
        bool keyDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        bool printScreenUp = (msg == WM_KEYUP || msg == WM_SYSKEYUP) && wp == VK_SNAPSHOT; // no KEYDOWN for PrtSc
        if ((keyDown || printScreenUp) && !IsModifierKey((UINT)wp)) {
            UINT vk = (UINT)wp;
            capture.active = false;
            capture.done = true;
            if (vk == VK_ESCAPE) capture.cancelled = true;
            else if (vk == VK_BACK) capture.cleared = true;
            else {
                UINT mods = 0;
                if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
                if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
                if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
                capture.vk = vk;
                capture.mods = mods;
            }
            result = 0;
            return true;
        }
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
        result = 1;
        return true;
    }

    if (msg == taskbarCreatedMsg && taskbarCreatedMsg != 0) {
        if (w.callbacks.onTaskbarCreated) w.callbacks.onTaskbarCreated();
        result = 0;
        return true;
    }
    if (msg == showRequestMsg && showRequestMsg != 0) {
        if (w.callbacks.onShowRequest) w.callbacks.onShowRequest();
        result = 0;
        return true;
    }

    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            if (IsZoomed(hwnd)) {
                // A maximized window hangs over the monitor edge by its frame size: clamp to the work area.
                HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoW(mon, &mi)) {
                    RECT r = mi.rcWork;
                    // Leave a 1px gap so an auto-hide taskbar can still be revealed.
                    APPBARDATA abd{};
                    abd.cbSize = sizeof(abd);
                    if ((SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) && SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
                        switch (abd.uEdge) {
                        case ABE_BOTTOM: r.bottom -= 1; break;
                        case ABE_TOP: r.top += 1; break;
                        case ABE_LEFT: r.left += 1; break;
                        case ABE_RIGHT: r.right -= 1; break;
                        }
                    }
                    params->rgrc[0] = r;
                }
            }
            result = 0;
            return true;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        result = HitTest(pt);
        return true;
    }

    // The maximize button reports HTMAXBUTTON (for Snap Layouts), so its clicks arrive as non-client.
    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) {
            maxPressed = true;
            result = 0;
            return true;
        }
        maxPressed = false;
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            if (maxPressed) w.ToggleMaximize();
            maxPressed = false;
            result = 0;
            return true;
        }
        maxPressed = false;
        break;
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMAXBUTTON) {
            result = 0;
            return true;
        }
        break;
    case WM_NCMOUSELEAVE:
        maxPressed = false;
        break;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            pendingW = LOWORD(lp);
            pendingH = HIWORD(lp);
            resizePending = true;
            if (inSizeMove && !w.inFrame && w.callbacks.onRenderWhileSizing)
                w.callbacks.onRenderWhileSizing();
        }
        result = 0;
        return true;

    case WM_ENTERSIZEMOVE:
        inSizeMove = true;
        break;
    case WM_EXITSIZEMOVE:
        inSizeMove = false;
        break;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = MulDiv(960, (int)dpi, 96);
        mmi->ptMinTrackSize.y = MulDiv(600, (int)dpi, 96);
        result = 0;
        return true;
    }

    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        dpiChanged = true;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        result = 0;
        return true;
    }

    case WM_ACTIVATE:
        focused = LOWORD(wp) != WA_INACTIVE;
        break;

    case WM_ERASEBKGND:
        result = 1;
        return true;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) {   // no Alt-key menu beep
            result = 0;
            return true;
        }
        break;

    case WM_HOTKEY:
        if (w.callbacks.onHotkey) w.callbacks.onHotkey((int)wp);
        result = 0;
        return true;

    case kTrayCallbackMessage:
        if (w.callbacks.onTrayMessage) w.callbacks.onTrayMessage((uint32_t)LOWORD(lp));
        result = 0;
        return true;

    case WM_CLOSE:
        if (w.callbacks.onCloseRequested) w.callbacks.onCloseRequested();
        else DestroyWindow(hwnd);
        result = 0;
        return true;

    case WM_QUERYENDSESSION:
        result = TRUE;
        return true;
    case WM_ENDSESSION:
        if (wp && w.callbacks.onEndSession) w.callbacks.onEndSession();
        result = 0;
        return true;

    case WM_DESTROY:
        PostQuitMessage(0);
        result = 0;
        return true;
    }
    return false;
}

void* Window::Hwnd() const { return impl_->hwnd; }
void* Window::Hinstance() const { return impl_->hinst; }
ID3D11Device* Window::Device() const { return impl_->device.Get(); }
ID3D11DeviceContext* Window::Context() const { return impl_->context.Get(); }
bool Window::UsesComposition() const { return impl_->composition; }

bool Window::SupportsBackdrop() const
{
    // Windows 11 22H2+: system Mica/Acrylic. Windows 10: the older accent blur-behind.
    return impl_->composition && platform::WindowsBuildNumber() >= 10240;
}

// Undocumented but stable since Windows 10 1507: blur behind a window via an "accent policy".
static void SetAccentBlur(HWND hwnd, bool enable)
{
    struct AccentPolicy {
        int state;      // 0 disabled, 3 blur behind
        int flags;
        uint32_t color;
        int animation;
    };
    struct CompositionData {
        int attribute;  // 19 = WCA_ACCENT_POLICY
        void* data;
        SIZE_T size;
    };
    using Fn = BOOL(WINAPI*)(HWND, CompositionData*);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    AccentPolicy ap{ enable ? 3 : 0, 0, 0, 0 };
    CompositionData cd{ 19, &ap, sizeof(ap) };
    fn(hwnd, &cd);
}

void Window::SetBackdrop(Backdrop b)
{
    Impl& d = *impl_;
    if (!SupportsBackdrop()) b = Backdrop::Gradient;
    d.backdrop = b;
    int type = 1; // DWMSBT_NONE
    switch (b) {
    case Backdrop::Mica: type = 2; break;      // DWMSBT_MAINWINDOW
    case Backdrop::Acrylic: type = 3; break;   // DWMSBT_TRANSIENTWINDOW
    case Backdrop::MicaAlt: type = 4; break;   // DWMSBT_TABBEDWINDOW
    default: break;
    }
    if (platform::WindowsBuildNumber() >= 22621)
        DwmSetWindowAttribute(d.hwnd, kDwmSystemBackdropType, &type, sizeof(type));
    else
        SetAccentBlur(d.hwnd, b != Backdrop::Gradient);
    d.ApplyFrameMargins();
}

void Window::BeginFrame(const float clearRgba[4])
{
    Impl& d = *impl_;
    if (d.resizePending) d.ApplyResize();
    ID3D11RenderTargetView* rtv = d.rtv.Get();
    d.context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)d.width, (float)d.height, 0.0f, 1.0f };
    d.context->RSSetViewports(1, &vp);
    if (rtv) d.context->ClearRenderTargetView(rtv, clearRgba);
}

void Window::Present(bool vsync)
{
    HRESULT hr = impl_->swap->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        static bool logged = false;
        if (!logged) {
            Log("GPU device lost (0x%08lx). Restart Replayr.", (unsigned long)impl_->device->GetDeviceRemovedReason());
            logged = true;
        }
    }
}

float Window::DpiScale() const { return (float)impl_->dpi / 96.0f; }

bool Window::TakeDpiChanged()
{
    bool c = impl_->dpiChanged;
    impl_->dpiChanged = false;
    return c;
}

bool Window::IsVisible() const { return impl_->hwnd && IsWindowVisible(impl_->hwnd); }
bool Window::Minimized() const { return impl_->hwnd && IsIconic(impl_->hwnd); }
bool Window::Maximized() const { return impl_->hwnd && IsZoomed(impl_->hwnd); }
bool Window::IsFocused() const { return impl_->focused && impl_->hwnd && GetForegroundWindow() == impl_->hwnd; }
bool Window::IsMaxButtonPressed() const { return impl_->maxPressed; }
int Window::Width() const { return (int)impl_->width; }
int Window::Height() const { return (int)impl_->height; }

void Window::Show()
{
    HWND h = impl_->hwnd;
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    else if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

void Window::ShowInitial(bool maximized)
{
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    SetForegroundWindow(impl_->hwnd);
}

void Window::Hide() { if (impl_->hwnd) ShowWindow(impl_->hwnd, SW_HIDE); }
void Window::Minimize() { if (impl_->hwnd) ShowWindow(impl_->hwnd, SW_MINIMIZE); }

void Window::ToggleMaximize()
{
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, IsZoomed(impl_->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void Window::SetFullscreen(bool on)
{
    Impl& d = *impl_;
    if (!d.hwnd || on == d.fullscreen) return;
    if (on) {
        d.savedPlacement.length = sizeof(d.savedPlacement);
        GetWindowPlacement(d.hwnd, &d.savedPlacement);
        d.savedZoomed = IsZoomed(d.hwnd) != 0;
        if (d.savedZoomed) ShowWindow(d.hwnd, SW_RESTORE);   // a maximized window keeps its frame insets
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(d.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int square = 1;   // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(d.hwnd, kDwmWindowCornerPreference, &square, sizeof(square));
        d.fullscreen = true;
        const RECT& r = mi.rcMonitor;
        SetWindowPos(d.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        d.fullscreen = false;
        int corner = 2;   // DWMWCP_ROUND
        DwmSetWindowAttribute(d.hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
        SetWindowPlacement(d.hwnd, &d.savedPlacement);
        if (d.savedZoomed) ShowWindow(d.hwnd, SW_MAXIMIZE);
        SetWindowPos(d.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

bool Window::IsFullscreen() const { return impl_->fullscreen; }

void Window::Close() { if (impl_->hwnd) DestroyWindow(impl_->hwnd); }

void Window::GetNormalRect(int& x, int& y, int& w, int& h, bool& maximized) const
{
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(impl_->hwnd, &wp);
    x = wp.rcNormalPosition.left;
    y = wp.rcNormalPosition.top;
    w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    maximized = IsZoomed(impl_->hwnd) || (wp.flags & WPF_RESTORETOMAXIMIZED);
}

bool Window::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void Window::WaitForMessages(unsigned ms)
{
    MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

CaptionLayout& Window::Caption() { return impl_->caption; }
KeyCapture& Window::Capture() { return impl_->capture; }

} // namespace cs
