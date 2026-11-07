#include "core/Screenshot.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> bgra;   // top-down, 4 bytes per pixel
};

struct Job {
    bool fromMonitor = false;
    HMONITOR monitor = nullptr;
    bool crop = false;
    RECT cropRect{};                   // screen coordinates
    Image image;                       // when !fromMonitor
    std::wstring path;
    bool clipboard = true;
    bool jpeg = false;
    bool banner = false;
    BannerStyle bannerStyle;
};

struct Done {
    ShotResult result;
    HGLOBAL dib = nullptr;
};

void ForceOpaque(Image& img)
{
    for (size_t i = 3; i < img.bgra.size(); i += 4) img.bgra[i] = 255;
}

bool CaptureRectGDI(const RECT& r, Image& out)
{
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        if (ok) {
            out.w = w;
            out.h = h;
            out.bgra.assign((unsigned char*)bits, (unsigned char*)bits + (size_t)w * h * 4);
            ForceOpaque(out);
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool CaptureMonitorDXGI(HMONITOR mon, Image& out)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    bool found = false;
    for (UINT a = 0; !found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (od.Monitor == mon) {
                found = true;
                break;
            }
            output.Reset();
        }
        if (!found) adapter.Reset();
    }
    if (!found) return false;

    // The device must live on the adapter that drives this monitor (hybrid-GPU laptops).
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        return false;
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) return false;
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = output1->DuplicateOutput(dev.Get(), &dup);
    if (FAILED(hr)) {
        Log("DuplicateOutput failed (0x%08lx), falling back to GDI", (unsigned long)hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        return false;   // rotated display: GDI handles the orientation for us

    ComPtr<ID3D11Texture2D> staging;
    bool haveImage = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(attempt == 0 ? 300 : 80, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (haveImage) break;   // static screen: the first frame is all we get
            continue;
        }
        if (FAILED(hr)) break;
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(res.As(&frame))) {
            if (!staging) {
                D3D11_TEXTURE2D_DESC td{};
                frame->GetDesc(&td);
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.SampleDesc.Count = 1;
                dev->CreateTexture2D(&td, nullptr, &staging);
            }
            if (staging) {
                ctx->CopyResource(staging.Get(), frame.Get());
                haveImage = true;
            }
        }
        dup->ReleaseFrame();
        if (haveImage && fi.LastPresentTime.QuadPart != 0) break;
    }
    if (!haveImage) return false;

    D3D11_TEXTURE2D_DESC td{};
    staging->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return false;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out.w = (int)td.Width;
    out.h = (int)td.Height;
    out.bgra.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < out.h; ++y)
        memcpy(&out.bgra[(size_t)y * out.w * 4], (const unsigned char*)map.pData + (size_t)y * map.RowPitch, (size_t)out.w * 4);
    ctx->Unmap(staging.Get(), 0);
    ForceOpaque(out);
    return true;
}

Image Crop(const Image& src, int x, int y, int w, int h)
{
    Image out;
    x = Clamp(x, 0, src.w);
    y = Clamp(y, 0, src.h);
    w = Clamp(w, 0, src.w - x);
    h = Clamp(h, 0, src.h - y);
    out.w = w;
    out.h = h;
    out.bgra.resize((size_t)w * h * 4);
    for (int row = 0; row < h; ++row)
        memcpy(&out.bgra[(size_t)row * w * 4], &src.bgra[((size_t)(y + row) * src.w + x) * 4], (size_t)w * 4);
    return out;
}

HRESULT SaveImage(const Image& img, const std::wstring& path, bool jpeg)
{
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory((UINT)img.w, (UINT)img.h, GUID_WICPixelFormat32bppBGRA, (UINT)img.w * 4,
                                         (UINT)img.bgra.size(), const_cast<BYTE*>(img.bgra.data()), &bitmap);
    if (FAILED(hr)) return hr;
    ComPtr<IWICStream> stream;
    if (FAILED(hr = factory->CreateStream(&stream))) return hr;
    if (FAILED(hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return hr;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(hr = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder))) return hr;
    if (FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return hr;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(hr = encoder->CreateNewFrame(&frame, &props))) return hr;
    if (jpeg && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(hr = frame->Initialize(props.Get()))) return hr;
    if (FAILED(hr = frame->SetSize((UINT)img.w, (UINT)img.h))) return hr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;   // no alpha needed; WriteSource converts
    if (FAILED(hr = frame->SetPixelFormat(&fmt))) return hr;
    if (FAILED(hr = frame->WriteSource(bitmap.Get(), nullptr))) return hr;
    if (FAILED(hr = frame->Commit())) return hr;
    return encoder->Commit();
}

HGLOBAL MakeClipboardDib(const Image& img)
{
    const size_t stride = (size_t)img.w * 4;
    const size_t bytes = sizeof(BITMAPINFOHEADER) + stride * img.h;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) return nullptr;
    auto* p = (unsigned char*)GlobalLock(g);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = img.w;
    bih.biHeight = img.h;   // bottom-up: the most compatible CF_DIB layout
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)(stride * img.h);
    memcpy(p, &bih, sizeof(bih));
    for (int y = 0; y < img.h; ++y)
        memcpy(p + sizeof(bih) + (size_t)(img.h - 1 - y) * stride, &img.bgra[(size_t)y * stride], stride);
    GlobalUnlock(g);
    return g;
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct Screenshotter::Impl {
    HINSTANCE hinst = nullptr;
    HWND mainHwnd = nullptr;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Done> done;
    bool quit = false;

    // Region overlay state (UI thread only).
    HWND overlay = nullptr;
    Image frozen;
    HBITMAP brightBmp = nullptr, dimBmp = nullptr;
    HDC brightDC = nullptr, dimDC = nullptr;
    RECT virt{};
    bool dragging = false;
    POINT dragStart{}, dragNow{};
    ShotRequest pending;

    void Run();
    void Enqueue(Job job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }
    void PushDone(Done d)
    {
        std::lock_guard<std::mutex> lock(mutex);
        done.push_back(std::move(d));
    }
    std::wstring MakePath(const ShotRequest& r) const
    {
        EnsureDir(r.folder);
        std::wstring base = SanitizeFileName(r.baseName.empty() ? L"Screenshot" : r.baseName);
        return UniquePath(JoinPath(r.folder, base + L" " + TimestampForFile() + (r.jpeg ? L".jpg" : L".png")));
    }
    bool BeginRegion(const ShotRequest& req);
    void EndRegion(bool accept);
    void PaintOverlay(HDC hdc);
    RECT Selection() const
    {
        RECT r{ std::min(dragStart.x, dragNow.x), std::min(dragStart.y, dragNow.y), std::max(dragStart.x, dragNow.x),
                std::max(dragStart.y, dragNow.y) };
        return r;
    }
};

static const wchar_t* kRegionClass = L"ReplayrRegionSelect";

static LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* d = reinterpret_cast<Screenshotter::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!d) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        d->PaintOverlay(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        d->dragging = true;
        d->dragStart = d->dragNow = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (d->dragging) {
            d->dragNow = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (d->dragging) {
            d->dragNow = POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            d->dragging = false;
            ReleaseCapture();
            d->EndRegion(true);
        }
        return 0;
    case WM_RBUTTONUP:
        d->EndRegion(false);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) d->EndRegion(false);
        return 0;
    case WM_KILLFOCUS:
        if (!d->dragging) d->EndRegion(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Screenshotter::Impl::BeginRegion(const ShotRequest& req)
{
    if (overlay) return false;
    virt.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virt.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virt.right = virt.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt.bottom = virt.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!CaptureRectGDI(virt, frozen)) return false;
    pending = req;

    // Two copies of the frozen desktop: as-is (inside the selection) and dimmed (outside).
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = frozen.w;
    bi.bmiHeader.biHeight = -frozen.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* brightBits = nullptr;
    void* dimBits = nullptr;
    brightBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &brightBits, nullptr, 0);
    dimBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &dimBits, nullptr, 0);
    if (!brightBmp || !dimBmp) {
        if (brightBmp) DeleteObject(brightBmp);
        if (dimBmp) DeleteObject(dimBmp);
        brightBmp = dimBmp = nullptr;
        ReleaseDC(nullptr, screen);
        return false;
    }
    memcpy(brightBits, frozen.bgra.data(), frozen.bgra.size());
    auto* dst = (unsigned char*)dimBits;
    for (size_t i = 0; i < frozen.bgra.size(); i += 4) {
        dst[i + 0] = (unsigned char)(frozen.bgra[i + 0] * 0.42f + 14);
        dst[i + 1] = (unsigned char)(frozen.bgra[i + 1] * 0.42f + 8);
        dst[i + 2] = (unsigned char)(frozen.bgra[i + 2] * 0.42f + 10);
        dst[i + 3] = 255;
    }
    brightDC = CreateCompatibleDC(screen);
    dimDC = CreateCompatibleDC(screen);
    SelectObject(brightDC, brightBmp);
    SelectObject(dimDC, dimBmp);
    ReleaseDC(nullptr, screen);

    dragging = false;
    overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kRegionClass, L"Replayr region", WS_POPUP, virt.left,
                              virt.top, virt.right - virt.left, virt.bottom - virt.top, nullptr, nullptr, hinst, this);
    if (!overlay) return false;
    ShowWindow(overlay, SW_SHOW);
    SetForegroundWindow(overlay);
    SetFocus(overlay);
    return true;
}

void Screenshotter::Impl::PaintOverlay(HDC hdc)
{
    const int W = virt.right - virt.left, H = virt.bottom - virt.top;
    HDC back = CreateCompatibleDC(hdc);
    HBITMAP backBmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBack = SelectObject(back, backBmp);

    BitBlt(back, 0, 0, W, H, dimDC, 0, 0, SRCCOPY);
    RECT sel = Selection();
    const bool hasSel = (dragging || sel.right - sel.left > 1) && sel.right > sel.left && sel.bottom > sel.top;
    const float dpiScale = (float)GetDpiForWindow(overlay) / 96.0f;
    COLORREF accent = RGB((int)(pending.accent[0] * 255), (int)(pending.accent[1] * 255), (int)(pending.accent[2] * 255));

    HFONT font = CreateFontW(-(int)(14 * dpiScale), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(back, font);
    SetBkMode(back, TRANSPARENT);

    if (hasSel) {
        BitBlt(back, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top, brightDC, sel.left, sel.top, SRCCOPY);
        HPEN pen = CreatePen(PS_SOLID, std::max(2, (int)(2 * dpiScale)), accent);
        HGDIOBJ oldPen = SelectObject(back, pen);
        HGDIOBJ oldBrush = SelectObject(back, GetStockObject(NULL_BRUSH));
        Rectangle(back, sel.left, sel.top, sel.right, sel.bottom);
        SelectObject(back, oldBrush);
        SelectObject(back, oldPen);
        DeleteObject(pen);

        wchar_t label[64];
        swprintf(label, 64, L"  %ld × %ld  ", sel.right - sel.left, sel.bottom - sel.top);
        SIZE ts{};
        GetTextExtentPoint32W(back, label, (int)wcslen(label), &ts);
        int lx = sel.left, ly = sel.top - ts.cy - (int)(8 * dpiScale);
        if (ly < 0) ly = sel.top + (int)(6 * dpiScale);
        RECT lr{ lx, ly, lx + ts.cx, ly + ts.cy + (int)(4 * dpiScale) };
        HBRUSH bg = CreateSolidBrush(RGB(20, 20, 34));
        FillRect(back, &lr, bg);
        DeleteObject(bg);
        SetTextColor(back, RGB(240, 240, 255));
        DrawTextW(back, label, -1, &lr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    // Hint on the monitor under the cursor.
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    const wchar_t* hint = L"   Drag to capture  ·  click for the whole screen  ·  Esc to cancel   ";
    SIZE hs{};
    GetTextExtentPoint32W(back, hint, (int)wcslen(hint), &hs);
    int hx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2 - hs.cx / 2 - virt.left;
    int hy = mi.rcMonitor.top - virt.top + (int)(28 * dpiScale);
    RECT hr{ hx, hy, hx + hs.cx, hy + hs.cy + (int)(12 * dpiScale) };
    HBRUSH hbg = CreateSolidBrush(RGB(24, 22, 40));
    FillRect(back, &hr, hbg);
    DeleteObject(hbg);
    SetTextColor(back, RGB(225, 228, 250));
    DrawTextW(back, hint, -1, &hr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    SelectObject(back, oldFont);
    DeleteObject(font);
    BitBlt(hdc, 0, 0, W, H, back, 0, 0, SRCCOPY);
    SelectObject(back, oldBack);
    DeleteObject(backBmp);
    DeleteDC(back);
}

void Screenshotter::Impl::EndRegion(bool accept)
{
    if (!overlay) return;
    HWND w = overlay;
    overlay = nullptr;
    RECT sel = Selection();
    DestroyWindow(w);
    DeleteDC(brightDC);
    DeleteDC(dimDC);
    DeleteObject(brightBmp);
    DeleteObject(dimBmp);
    brightDC = dimDC = nullptr;
    brightBmp = dimBmp = nullptr;

    if (!accept) {
        frozen = Image();
        Done d;
        d.result.cancelled = true;
        PushDone(std::move(d));
        return;
    }
    Job job;
    job.path = MakePath(pending);
    job.clipboard = pending.copyToClipboard;
    job.banner = pending.banner;
    job.bannerStyle = pending.bannerStyle;
    job.jpeg = pending.jpeg;
    if (sel.right - sel.left < 4 || sel.bottom - sel.top < 4) {
        // A click without dragging: the whole monitor under the cursor.
        POINT pt{ sel.left + virt.left, sel.top + virt.top };
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        job.image = Crop(frozen, mi.rcMonitor.left - virt.left, mi.rcMonitor.top - virt.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    } else {
        job.image = Crop(frozen, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top);
    }
    frozen = Image();
    Enqueue(std::move(job));
}

void Screenshotter::Impl::Run()
{
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return quit || !jobs.empty(); });
            if (quit) break;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        Done d;
        Image img;
        if (job.fromMonitor) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(job.monitor, &mi);
            Image full;
            if (!CaptureMonitorDXGI(job.monitor, full) && !CaptureRectGDI(mi.rcMonitor, full)) {
                d.result.error = "Couldn't capture the screen.";
                PushDone(std::move(d));
                continue;
            }
            if (job.crop) {
                RECT r{};
                if (IntersectRect(&r, &job.cropRect, &mi.rcMonitor))
                    img = Crop(full, r.left - mi.rcMonitor.left, r.top - mi.rcMonitor.top, r.right - r.left, r.bottom - r.top);
                else
                    img = std::move(full);
            } else {
                img = std::move(full);
            }
        } else {
            img = std::move(job.image);
        }
        if (img.w <= 0 || img.h <= 0) {
            d.result.error = "Nothing to capture.";
            PushDone(std::move(d));
            continue;
        }
        if (job.banner) {
            const BannerLayout l = BannerLayoutFor(img.w, img.h, job.bannerStyle.corner);
            std::vector<uint8_t> banner;
            if (RenderBanner(l, job.bannerStyle, banner)) {
                BannerFrame f;
                f.visible = true;
                f.alpha = 1.0f;
                CompositeBanner(img.bgra.data(), img.w * 4, img.w, img.h, banner, l, f, job.bannerStyle.corner);
            }
        }
        HRESULT hr = SaveImage(img, job.path, job.jpeg);
        if (FAILED(hr)) {
            DeleteFileW(job.path.c_str());
            char buf[96];
            snprintf(buf, sizeof(buf), "Couldn't save the screenshot (0x%08lx).", (unsigned long)hr);
            d.result.error = buf;
        } else {
            d.result.ok = true;
            d.result.path = job.path;
            d.result.width = img.w;
            d.result.height = img.h;
            if (job.clipboard) d.dib = MakeClipboardDib(img);
        }
        PushDone(std::move(d));
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
}

Screenshotter::Screenshotter() : impl_(std::make_unique<Impl>()) {}
Screenshotter::~Screenshotter() { Shutdown(); }

void Screenshotter::Init(void* hinstance, void* mainHwnd)
{
    impl_->hinst = (HINSTANCE)hinstance;
    impl_->mainHwnd = (HWND)mainHwnd;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RegionWndProc;
    wc.hInstance = impl_->hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kRegionClass;
    RegisterClassExW(&wc);
    impl_->quit = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
}

void Screenshotter::Shutdown()
{
    if (!impl_) return;
    impl_->EndRegion(false);
    if (impl_->worker.joinable()) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->quit = true;
        }
        impl_->cv.notify_all();
        impl_->worker.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& d : impl_->done)
        if (d.dib) GlobalFree(d.dib);
    impl_->done.clear();
}

void Screenshotter::Capture(const ShotRequest& req)
{
    Impl& d = *impl_;
    if (req.mode == ShotMode::Region) {
        if (!d.BeginRegion(req)) {
            Done done;
            done.result.error = "Couldn't start region capture.";
            d.PushDone(std::move(done));
        }
        return;
    }
    Job job;
    job.fromMonitor = true;
    job.path = d.MakePath(req);
    job.clipboard = req.copyToClipboard;
    job.jpeg = req.jpeg;
    job.banner = req.banner;
    job.bannerStyle = req.bannerStyle;
    HWND fg = GetForegroundWindow();
    job.monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    if (req.mode == ShotMode::ActiveWindow && fg && fg != d.mainHwnd && fg != GetShellWindow() && fg != GetDesktopWindow()) {
        RECT r{};
        // The visible frame, without the invisible resize borders.
        if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(fg, &r);
        job.crop = true;
        job.cropRect = r;
    }
    d.Enqueue(std::move(job));
}

bool Screenshotter::SelectingRegion() const { return impl_->overlay != nullptr; }

bool Screenshotter::PollResult(ShotResult& out)
{
    Done d;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->done.empty()) return false;
        d = std::move(impl_->done.front());
        impl_->done.pop_front();
    }
    if (d.dib) {
        bool placed = false;
        if (OpenClipboard(impl_->mainHwnd)) {
            EmptyClipboard();
            placed = SetClipboardData(CF_DIB, d.dib) != nullptr;
            CloseClipboard();
        }
        if (!placed) GlobalFree(d.dib);   // on success the clipboard owns the memory
        d.result.copied = placed;
    }
    out = std::move(d.result);
    return true;
}

} // namespace cs
