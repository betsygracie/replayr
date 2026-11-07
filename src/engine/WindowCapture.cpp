#include "engine/WindowCapture.h"
#include "core/Log.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <inspectable.h>
#include <winstring.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

// ---------------------------------------------------------------------------------------------
// Hand-declared WinRT ABI (Windows.Graphics.Capture). Method order matches the Windows SDK
// headers exactly; only the methods used here are ever called.
// ---------------------------------------------------------------------------------------------
namespace wgc {

struct SizeInt32 {
    int32_t Width, Height;
};
struct Token {
    int64_t value;
};
constexpr int32_t kBgra8 = 87;   // DirectXPixelFormat::B8G8R8A8UIntNormalized

struct IGraphicsCaptureItemInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID riid, void** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID riid, void** result) = 0;
};
struct IGraphicsCaptureItem : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(IUnknown* handler, Token* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(Token token) = 0;
};
struct IDirect3D11CaptureFrame : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(int64_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32* value) = 0;
};
struct IGraphicsCaptureSession : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0;
};
struct IGraphicsCaptureSession2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean value) = 0;
};
struct IGraphicsCaptureSession3 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean value) = 0;
};
struct IGraphicsCaptureSessionStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(boolean* result) = 0;
};
struct IDirect3D11CaptureFramePool : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable* device, int32_t format, int32_t buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(IDirect3D11CaptureFrame** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(IUnknown* handler, Token* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(Token token) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(IGraphicsCaptureItem* item, IGraphicsCaptureSession** result) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable** value) = 0;
};
struct IDirect3D11CaptureFramePoolStatics2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable* device, int32_t format, int32_t buffers, SizeInt32 size,
                                                         IDirect3D11CaptureFramePool** result) = 0;
};
struct IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** p) = 0;
};
struct IClosable : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

constexpr GUID IID_ItemInterop = { 0x3628E81B, 0x3CAC, 0x4C60, { 0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56 } };
constexpr GUID IID_Item = { 0x79C3F95B, 0x31F7, 0x4EC2, { 0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60 } };
constexpr GUID IID_PoolStatics2 = { 0x589B103F, 0x6BBC, 0x5DF5, { 0xA9, 0x91, 0x15, 0xE3, 0xD5, 0xB2, 0xE3, 0x3D } };
constexpr GUID IID_Session2 = { 0x2C39AE40, 0x7D2E, 0x5044, { 0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E } };
constexpr GUID IID_Session3 = { 0xF2CDD966, 0x22AE, 0x5EA1, { 0x95, 0x96, 0x3A, 0x28, 0x9A, 0x44, 0xC3, 0xBE } };
constexpr GUID IID_SessionStatics = { 0x2224A540, 0x5974, 0x49AA, { 0xB2, 0x32, 0x08, 0x82, 0x53, 0x6F, 0x4C, 0xB5 } };
constexpr GUID IID_DxgiAccess = { 0xA9B3D012, 0x3DF2, 0x4EE3, { 0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1 } };
constexpr GUID IID_Closable = { 0x30D5A829, 0x7FA4, 0x4026, { 0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E } };

// combase / d3d11 entry points, loaded at runtime so Replayr still starts on systems without them.
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringFn = HRESULT(WINAPI*)(PCNZWCH, UINT32, HSTRING*);
using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
using CreateD3DDeviceFn = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);

struct Api {
    RoGetActivationFactoryFn getFactory = nullptr;
    WindowsCreateStringFn createString = nullptr;
    WindowsDeleteStringFn deleteString = nullptr;
    CreateD3DDeviceFn createDevice = nullptr;
    bool ok = false;
};

const Api& GetApi()
{
    static Api a = [] {
        Api r;
        HMODULE combase = LoadLibraryW(L"combase.dll"), d3d = LoadLibraryW(L"d3d11.dll");
        if (combase) {
            r.getFactory = (RoGetActivationFactoryFn)(void*)GetProcAddress(combase, "RoGetActivationFactory");
            r.createString = (WindowsCreateStringFn)(void*)GetProcAddress(combase, "WindowsCreateString");
            r.deleteString = (WindowsDeleteStringFn)(void*)GetProcAddress(combase, "WindowsDeleteString");
        }
        if (d3d) r.createDevice = (CreateD3DDeviceFn)(void*)GetProcAddress(d3d, "CreateDirect3D11DeviceFromDXGIDevice");
        r.ok = r.getFactory && r.createString && r.deleteString && r.createDevice;
        return r;
    }();
    return a;
}

HRESULT Factory(const wchar_t* cls, REFIID iid, void** out)
{
    const Api& a = GetApi();
    if (!a.ok) return E_NOTIMPL;
    HSTRING s = nullptr;
    HRESULT hr = a.createString(cls, (UINT32)wcslen(cls), &s);
    if (FAILED(hr)) return hr;
    hr = a.getFactory(s, iid, out);
    a.deleteString(s);
    return hr;
}

void CloseObject(IUnknown* o)
{
    if (!o) return;
    ComPtr<IClosable> c;
    if (SUCCEEDED(o->QueryInterface(IID_Closable, (void**)c.GetAddressOf())) && c) c->Close();
}

} // namespace wgc

// ---------------------------------------------------------------------------------------------

struct WindowCapture::Impl {
    HWND window = nullptr;
    ComPtr<IInspectable> device;   // WinRT IDirect3DDevice
    ComPtr<wgc::IGraphicsCaptureItem> item;
    ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
    ComPtr<wgc::IGraphicsCaptureSession> session;
    wgc::SizeInt32 poolSize{};
    bool closed = false;
};

WindowCapture::WindowCapture() : d_(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() { Close(); }

bool WindowCapture::Supported()
{
    static int cached = -1;
    if (cached < 0) {
        ComPtr<wgc::IGraphicsCaptureSessionStatics> st;
        boolean ok = 0;
        cached = SUCCEEDED(wgc::Factory(L"Windows.Graphics.Capture.GraphicsCaptureSession", wgc::IID_SessionStatics, (void**)st.GetAddressOf())) && st &&
                         SUCCEEDED(st->IsSupported(&ok)) && ok
                     ? 1
                     : 0;
    }
    return cached == 1;
}

bool WindowCapture::Open(ID3D11Device* device, HWND__* window, bool cursor, bool hideBorder, std::string& error)
{
    Close();
    Impl& m = *d_;
    if (!window || !IsWindow(window)) {
        error = "The game window isn't there.";
        return false;
    }
    if (!Supported()) {
        error = "Game-only capture needs Windows 10 (version 1903) or newer.";
        return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(wgc::GetApi().createDevice(dxgi.Get(), &m.device))) {
        error = "Couldn't share the GPU with Windows capture.";
        return false;
    }
    ComPtr<wgc::IGraphicsCaptureItemInterop> interop;
    HRESULT hr = wgc::Factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", wgc::IID_ItemInterop, (void**)interop.GetAddressOf());
    if (SUCCEEDED(hr)) hr = interop->CreateForWindow(window, wgc::IID_Item, (void**)m.item.GetAddressOf());
    if (FAILED(hr) || !m.item) {
        error = "Windows won't capture that window (it may be protected or elevated).";
        Log("WGC CreateForWindow failed: 0x%08lx", (unsigned long)hr);
        Close();
        return false;
    }
    m.item->get_Size(&m.poolSize);
    m.poolSize.Width = std::max(1, m.poolSize.Width);
    m.poolSize.Height = std::max(1, m.poolSize.Height);
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> statics;
    hr = wgc::Factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", wgc::IID_PoolStatics2, (void**)statics.GetAddressOf());
    if (SUCCEEDED(hr)) hr = statics->CreateFreeThreaded(m.device.Get(), wgc::kBgra8, 2, m.poolSize, &m.pool);
    if (SUCCEEDED(hr)) hr = m.pool->CreateCaptureSession(m.item.Get(), &m.session);
    if (FAILED(hr) || !m.session) {
        error = "Couldn't start Windows capture for the game.";
        Log("WGC pool/session failed: 0x%08lx", (unsigned long)hr);
        Close();
        return false;
    }
    ComPtr<wgc::IGraphicsCaptureSession2> s2;
    if (SUCCEEDED(m.session->QueryInterface(wgc::IID_Session2, (void**)s2.GetAddressOf())) && s2) s2->put_IsCursorCaptureEnabled(cursor ? 1 : 0);
    ComPtr<wgc::IGraphicsCaptureSession3> s3;
    if (hideBorder && SUCCEEDED(m.session->QueryInterface(wgc::IID_Session3, (void**)s3.GetAddressOf())) && s3) s3->put_IsBorderRequired(0);   // Windows 11; ignored where not allowed
    hr = m.session->StartCapture();
    if (FAILED(hr)) {
        error = "Windows capture didn't start.";
        Close();
        return false;
    }
    m.window = window;
    Log("WGC: capturing \"%s\" (%dx%d)", WindowTitle(window).c_str(), m.poolSize.Width, m.poolSize.Height);
    return true;
}

void WindowCapture::Close()
{
    Impl& m = *d_;
    wgc::CloseObject(m.session.Get());
    wgc::CloseObject(m.pool.Get());
    m.session.Reset();
    m.pool.Reset();
    m.item.Reset();
    m.device.Reset();
    m.window = nullptr;
    m.closed = false;
}

bool WindowCapture::IsOpen() const { return d_->session != nullptr; }
HWND__* WindowCapture::Window() const { return d_->window; }
bool WindowCapture::Closed() const { return d_->closed || (d_->window && !IsWindow(d_->window)); }

bool WindowCapture::Poll(ID3D11DeviceContext* ctx, ID3D11Texture2D* canvas, int canvasW, int canvasH, int& contentW, int& contentH)
{
    Impl& m = *d_;
    if (!m.pool) return false;
    // Drain to the newest frame.
    ComPtr<wgc::IDirect3D11CaptureFrame> frame, next;
    while (SUCCEEDED(m.pool->TryGetNextFrame(next.ReleaseAndGetAddressOf())) && next) {
        if (frame) wgc::CloseObject(frame.Get());
        frame = next;
    }
    if (!frame) return false;
    bool copied = false;
    wgc::SizeInt32 size{};
    frame->get_ContentSize(&size);
    ComPtr<IInspectable> surface;
    ComPtr<wgc::IDirect3DDxgiInterfaceAccess> access;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(frame->get_Surface(&surface)) && surface && SUCCEEDED(surface->QueryInterface(wgc::IID_DxgiAccess, (void**)access.GetAddressOf())) && access &&
        SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex))) && tex) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        const int w = std::min({ (int)size.Width, (int)td.Width, canvasW }), h = std::min({ (int)size.Height, (int)td.Height, canvasH });
        if (w > 0 && h > 0) {
            const D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)h, 1 };
            ctx->CopySubresourceRegion(canvas, 0, 0, 0, 0, tex.Get(), 0, &box);
            contentW = w;
            contentH = h;
            copied = true;
        }
    }
    wgc::CloseObject(frame.Get());
    // The window changed size (fullscreen <-> windowed, resolution change): match the pool to it.
    if (size.Width > 0 && size.Height > 0 && (size.Width != m.poolSize.Width || size.Height != m.poolSize.Height)) {
        m.poolSize = size;
        m.pool->Recreate(m.device.Get(), wgc::kBgra8, 2, m.poolSize);
    }
    return copied;
}

// ---------------------------------------------------------------------------------------------

std::string WindowTitle(HWND__* w)
{
    wchar_t buf[256] = {};
    GetWindowTextW(w, buf, 255);
    char out[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

HWND__* FindGameWindow(unsigned long ownProcessId)
{
    HWND w = GetForegroundWindow();
    if (!w || w == GetDesktopWindow() || w == GetShellWindow() || !IsWindowVisible(w) || IsIconic(w)) return nullptr;
    w = GetAncestor(w, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == ownProcessId) return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 63);
    static const wchar_t* shell[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow",
                                      L"MultitaskingViewFrame", L"TaskListThumbnailWnd", L"XamlExplorerHostIslandWindow" };
    for (const wchar_t* s : shell)
        if (!wcscmp(cls, s)) return nullptr;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return nullptr;
    // Big enough to be a game: at least 60% of its monitor (fullscreen, borderless or a large window).
    RECT r{};
    GetWindowRect(w, &r);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return nullptr;
    const double area = (double)(r.right - r.left) * (r.bottom - r.top);
    const double mon = (double)(mi.rcMonitor.right - mi.rcMonitor.left) * (mi.rcMonitor.bottom - mi.rcMonitor.top);
    if (mon <= 0 || area < mon * 0.6) return nullptr;
    return w;
}

} // namespace cs::engine
