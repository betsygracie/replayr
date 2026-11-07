#include "engine/DesktopCapture.h"
#include "core/InputOverlay.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Util.h"

#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

const char* FramePathLabel(FramePath p)
{
    switch (p) {
    case FramePath::GpuSurfaces: return "GPU";
    case FramePath::GpuReadback: return "GPU + copy";
    case FramePath::CpuRgb: return "CPU";
    }
    return "?";
}

struct DesktopCapture::Impl {
    DesktopCaptureParams params;
    FramePath path = FramePath::GpuSurfaces;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> dup;
    RECT monitorRect{};
    std::string monitorName, gpuName;
    int srcW = 0, srcH = 0, outW = 0, outH = 0;
    double lastReopenTry = 0.0;

    // GDI fallback when Desktop Duplication isn't available (Remote Desktop, some hybrid-GPU
    // laptops, virtual machines). Slower, but works everywhere.
    bool gdi = false;
    HDC gdiMem = nullptr;
    HBITMAP gdiBmp = nullptr;
    void* gdiBits = nullptr;
    bool GdiInit();
    void GdiGrab();
    void GdiFree();

    // frame: newest desktop image. compose: frame + cursor (GDI-compatible, so DrawIconEx works).
    ComPtr<ID3D11Texture2D> frame, compose;
    ComPtr<ID3D11RenderTargetView> frameRtv;

    // Video processor (BGRA -> NV12 + scaling)
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> venum;
    ComPtr<ID3D11VideoProcessor> vproc;
    ComPtr<ID3D11VideoProcessorInputView> inView;

    // Readback
    ComPtr<ID3D11Texture2D> nv12Scratch, stagingNv12, stagingBgra;

    bool FindOutput(std::string& error);
    bool CreateDup();
    bool InitVideoProcessor();
    bool ConvertTo(ID3D11Texture2D* nv12);
    void DrawCursor();
    void DrawInputs();   // keyboard/mouse overlay, when it's set to show only in clips
};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() { Close(); }

bool DesktopCapture::Impl::FindOutput(std::string& error)
{
    std::vector<platform::MonitorDesc> mons = platform::EnumerateMonitors();
    if (mons.empty()) {
        error = "No displays found.";
        return false;
    }
    int idx = params.monitorIndex;
    if (idx < 0) {   // the monitor the foreground window is on
        idx = 0;
        HMONITOR hm = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi))
            for (size_t i = 0; i < mons.size(); ++i)
                if (mons[i].x == mi.rcMonitor.left && mons[i].y == mi.rcMonitor.top) idx = (int)i;
    }
    if (idx >= (int)mons.size()) idx = 0;
    const platform::MonitorDesc& m = mons[idx];
    monitorName = "Display " + std::to_string(idx + 1);

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "DXGI isn't available.";
        return false;
    }
    ComPtr<IDXGIAdapter1> a;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &a) != DXGI_ERROR_NOT_FOUND; ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; a->EnumOutputs(oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC od{};
            o->GetDesc(&od);
            if (od.DesktopCoordinates.left == m.x && od.DesktopCoordinates.top == m.y) {
                adapter = a;
                o.As(&output);
                monitorRect = od.DesktopCoordinates;
                DXGI_ADAPTER_DESC1 ad{};
                a->GetDesc1(&ad);
                gpuName = ToUtf8(ad.Description);
                return output != nullptr;
            }
            o.Reset();
        }
        a.Reset();
    }
    error = "Couldn't find the GPU output for " + monitorName + ".";
    return false;
}

bool DesktopCapture::Impl::CreateDup()
{
    dup.Reset();
    HRESULT hr = output->DuplicateOutput(device.Get(), &dup);
    if (FAILED(hr)) {
        Log("DuplicateOutput failed: 0x%08lx", (unsigned long)hr);
        return false;
    }
    return true;
}

bool DesktopCapture::Open(const DesktopCaptureParams& p, std::string& error)
{
    Close();
    Impl& m = *impl_;
    m.params = p;
    m.path = FramePath::GpuSurfaces;
    if (!m.FindOutput(error)) return false;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(m.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                   &m.device, nullptr, &m.context);
    if (FAILED(hr)) {
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        m.path = FramePath::CpuRgb;
        hr = D3D11CreateDevice(m.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &m.device,
                               nullptr, &m.context);
    }
    if (FAILED(hr)) {
        error = "Couldn't create a Direct3D 11 device on " + m.gpuName + ".";
        return false;
    }
    // Media Foundation uses the device from its own threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m.device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (m.CreateDup()) {
        DXGI_OUTDUPL_DESC dd{};
        m.dup->GetDesc(&dd);
        m.srcW = (int)dd.ModeDesc.Width;
        m.srcH = (int)dd.ModeDesc.Height;
        if (dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 || dd.Rotation == DXGI_MODE_ROTATION_ROTATE270) std::swap(m.srcW, m.srcH);
    } else {
        m.gdi = true;
        m.srcW = m.monitorRect.right - m.monitorRect.left;
        m.srcH = m.monitorRect.bottom - m.monitorRect.top;
        if (!m.GdiInit()) {
            error = "Windows refused to share the screen. Close other capture apps or update your GPU driver.";
            return false;
        }
        Log("Desktop Duplication unavailable: using GDI capture (slower)");
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m.srcW;
    td.Height = m.srcH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.frame))) {
        error = "Out of video memory.";
        return false;
    }
    m.device->CreateRenderTargetView(m.frame.Get(), nullptr, &m.frameRtv);
    if (m.frameRtv) {
        const float black[4] = { 0, 0, 0, 1 };
        m.context->ClearRenderTargetView(m.frameRtv.Get(), black);
    }
    td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.compose))) {
        td.MiscFlags = 0;   // no cursor drawing then
        m.params.cursor = false;
        if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.compose))) {
            error = "Out of video memory.";
            return false;
        }
    }

    // Output size: keep the aspect ratio, never upscale, even dimensions (NV12 needs them).
    int oh = (p.outputHeight > 0 && p.outputHeight < m.srcH) ? p.outputHeight : m.srcH;
    int ow = (int)((int64_t)m.srcW * oh / m.srcH);
    m.outW = std::max(2, ow & ~1);
    m.outH = std::max(2, oh & ~1);

    if (m.path != FramePath::CpuRgb && !m.InitVideoProcessor()) {
        Log("Video processor unavailable, using CPU frames");
        m.path = FramePath::CpuRgb;
    }
    if (m.path == FramePath::CpuRgb) {   // MF's software converter can't scale reliably
        m.outW = m.srcW & ~1;
        m.outH = m.srcH & ~1;
    }
    Log("Capture: %s on %s, %dx%d -> %dx%d @ %d fps (%s)", m.monitorName.c_str(), m.gpuName.c_str(), m.srcW, m.srcH, m.outW, m.outH,
        p.fps, FramePathLabel(m.path));
    return true;
}

bool DesktopCapture::Impl::InitVideoProcessor()
{
    if (FAILED(device.As(&vdev)) || FAILED(context.As(&vctx))) return false;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = { (UINT)params.fps, 1 };
    cd.InputWidth = srcW;
    cd.InputHeight = srcH;
    cd.OutputFrameRate = { (UINT)params.fps, 1 };
    cd.OutputWidth = outW;
    cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &venum))) return false;
    UINT support = 0;
    if (FAILED(venum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        return false;
    if (FAILED(vdev->CreateVideoProcessor(venum.Get(), 0, &vproc))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    if (FAILED(vdev->CreateVideoProcessorInputView(compose.Get(), venum.Get(), &ivd, &inView))) return false;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
    in.RGB_Range = 0;          // full range RGB
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.YCbCr_Matrix = 1;      // BT.709
    out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vctx->VideoProcessorSetStreamColorSpace(vproc.Get(), 0, &in);
    vctx->VideoProcessorSetOutputColorSpace(vproc.Get(), &out);
    vctx->VideoProcessorSetStreamFrameFormat(vproc.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    RECT src{ 0, 0, srcW, srcH }, dst{ 0, 0, outW, outH };
    vctx->VideoProcessorSetStreamSourceRect(vproc.Get(), 0, TRUE, &src);
    vctx->VideoProcessorSetStreamDestRect(vproc.Get(), 0, TRUE, &dst);
    vctx->VideoProcessorSetOutputTargetRect(vproc.Get(), TRUE, &dst);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vproc.Get(), 0, FALSE);
    return true;
}

bool DesktopCapture::Impl::GdiInit()
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = srcW;
    bi.bmiHeader.biHeight = -srcH;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    gdiMem = CreateCompatibleDC(screen);
    gdiBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &gdiBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!gdiMem || !gdiBmp || !gdiBits) {
        GdiFree();
        return false;
    }
    SelectObject(gdiMem, gdiBmp);
    return true;
}

void DesktopCapture::Impl::GdiGrab()
{
    HDC screen = GetDC(nullptr);
    BitBlt(gdiMem, 0, 0, srcW, srcH, screen, monitorRect.left, monitorRect.top, SRCCOPY);
    ReleaseDC(nullptr, screen);
    GdiFlush();
    context->UpdateSubresource(frame.Get(), 0, nullptr, gdiBits, (UINT)srcW * 4, 0);
}

void DesktopCapture::Impl::GdiFree()
{
    if (gdiMem) DeleteDC(gdiMem);
    if (gdiBmp) DeleteObject(gdiBmp);
    gdiMem = nullptr;
    gdiBmp = nullptr;
    gdiBits = nullptr;
}

void DesktopCapture::Close()
{
    Impl& m = *impl_;
    m.GdiFree();
    m.gdi = false;
    if (m.context) m.context->ClearState();
    m.inView.Reset();
    m.vproc.Reset();
    m.venum.Reset();
    m.vctx.Reset();
    m.vdev.Reset();
    m.nv12Scratch.Reset();
    m.stagingNv12.Reset();
    m.stagingBgra.Reset();
    m.frameRtv.Reset();
    m.frame.Reset();
    m.compose.Reset();
    m.dup.Reset();
    m.context.Reset();
    m.device.Reset();
    m.output.Reset();
    m.adapter.Reset();
}

bool DesktopCapture::Update()
{
    Impl& m = *impl_;
    if (m.gdi) {
        m.GdiGrab();
        return true;
    }
    if (!m.dup) {
        const double now = NowSeconds();
        if (now - m.lastReopenTry < 0.5) return false;
        m.lastReopenTry = now;
        if (!m.CreateDup()) return false;
        DXGI_OUTDUPL_DESC dd{};
        m.dup->GetDesc(&dd);
        int w = (int)dd.ModeDesc.Width, h = (int)dd.ModeDesc.Height;
        if (dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 || dd.Rotation == DXGI_MODE_ROTATION_ROTATE270) std::swap(w, h);
        if (w != m.srcW || h != m.srcH) {   // resolution changed: the session has to restart
            m.dup.Reset();
            return false;
        }
    }
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = m.dup->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {   // mode change, UAC/lock screen, fullscreen switch
        m.dup.Reset();
        return false;
    }
    if (FAILED(hr)) return true;
    if (fi.LastPresentTime.QuadPart != 0 && res) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(res.As(&tex))) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            if ((int)d.Width == m.srcW && (int)d.Height == m.srcH && d.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
                m.context->CopyResource(m.frame.Get(), tex.Get());
        }
    }
    m.dup->ReleaseFrame();
    return true;
}

void DesktopCapture::Impl::DrawCursor()
{
    if (!params.cursor) return;
    CURSORINFO ci{ sizeof(ci) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    const int x = ci.ptScreenPos.x - monitorRect.left - (int)ii.xHotspot;
    const int y = ci.ptScreenPos.y - monitorRect.top - (int)ii.yHotspot;
    if (x > srcW || y > srcH || x < -128 || y < -128) return;
    ComPtr<IDXGISurface1> surf;
    if (FAILED(compose.As(&surf))) return;
    HDC dc = nullptr;
    if (FAILED(surf->GetDC(FALSE, &dc))) return;
    DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
    surf->ReleaseDC(nullptr);
}

void DesktopCapture::Impl::DrawInputs()
{
    ComPtr<IDXGISurface1> surf;
    if (FAILED(compose.As(&surf))) return;
    HDC dc = nullptr;
    if (FAILED(surf->GetDC(FALSE, &dc))) return;   // compose isn't GDI-compatible on this GPU
    InputOverlay::DrawIntoCapture(dc, srcW, srcH, monitorRect.left, monitorRect.top, monitorRect.right, monitorRect.bottom);
    surf->ReleaseDC(nullptr);
}

bool DesktopCapture::Impl::ConvertTo(ID3D11Texture2D* nv12)
{
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(vdev->CreateVideoProcessorOutputView(nv12, venum.Get(), &ovd, &outView))) return false;
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    return SUCCEEDED(vctx->VideoProcessorBlt(vproc.Get(), outView.Get(), 0, 1, &stream));
}

static bool MakeNv12(ID3D11Device* dev, int w, int h, D3D11_USAGE usage, UINT bind, UINT cpu, ID3D11Texture2D** out)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = usage;
    td.BindFlags = bind;
    td.CPUAccessFlags = cpu;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, out));
}

bool DesktopCapture::ComposeGpu(ID3D11Texture2D** texture)
{
    Impl& m = *impl_;
    *texture = nullptr;
    if (!m.vproc) return false;
    m.context->CopyResource(m.compose.Get(), m.frame.Get());
    m.DrawCursor();
    m.DrawInputs();
    // A fresh texture per frame: the encoder may still be reading the previous ones.
    ComPtr<ID3D11Texture2D> nv12;
    if (!MakeNv12(m.device.Get(), m.outW, m.outH, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &nv12)) return false;
    if (!m.ConvertTo(nv12.Get())) return false;
    *texture = nv12.Detach();
    return true;
}

bool DesktopCapture::ComposeCpu(uint8_t* dst, size_t dstSize)
{
    Impl& m = *impl_;
    m.context->CopyResource(m.compose.Get(), m.frame.Get());
    m.DrawCursor();
    m.DrawInputs();
    D3D11_MAPPED_SUBRESOURCE map{};
    if (m.path == FramePath::GpuReadback && m.vproc) {
        if (!m.nv12Scratch && !MakeNv12(m.device.Get(), m.outW, m.outH, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, &m.nv12Scratch))
            return false;
        if (!m.stagingNv12 && !MakeNv12(m.device.Get(), m.outW, m.outH, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, &m.stagingNv12))
            return false;
        if (!m.ConvertTo(m.nv12Scratch.Get())) return false;
        m.context->CopyResource(m.stagingNv12.Get(), m.nv12Scratch.Get());
        if (FAILED(m.context->Map(m.stagingNv12.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
        const size_t need = (size_t)m.outW * m.outH * 3 / 2;
        if (dstSize >= need) {
            const uint8_t* src = (const uint8_t*)map.pData;
            for (int y = 0; y < m.outH; ++y) memcpy(dst + (size_t)y * m.outW, src + (size_t)y * map.RowPitch, m.outW);
            const uint8_t* uv = src + (size_t)map.RowPitch * m.outH;   // chroma plane follows the luma rows
            uint8_t* duv = dst + (size_t)m.outW * m.outH;
            for (int y = 0; y < m.outH / 2; ++y) memcpy(duv + (size_t)y * m.outW, uv + (size_t)y * map.RowPitch, m.outW);
        }
        m.context->Unmap(m.stagingNv12.Get(), 0);
        return dstSize >= need;
    }
    if (!m.stagingBgra) {
        D3D11_TEXTURE2D_DESC td{};
        m.compose->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.stagingBgra))) return false;
    }
    m.context->CopyResource(m.stagingBgra.Get(), m.compose.Get());
    if (FAILED(m.context->Map(m.stagingBgra.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    const size_t row = (size_t)m.outW * 4, need = row * m.outH;
    if (dstSize >= need)
        for (int y = 0; y < m.outH; ++y) memcpy(dst + (size_t)y * row, (const uint8_t*)map.pData + (size_t)y * map.RowPitch, row);
    m.context->Unmap(m.stagingBgra.Get(), 0);
    return dstSize >= need;
}

bool DesktopCapture::SetPath(FramePath p)
{
    Impl& m = *impl_;
    if (p == FramePath::CpuRgb && m.outW != (m.srcW & ~1)) return false;   // can't scale on the CPU path
    if (p != FramePath::CpuRgb && !m.vproc) return false;
    m.path = p;
    return true;
}

FramePath DesktopCapture::Path() const { return impl_->path; }
int DesktopCapture::OutWidth() const { return impl_->outW; }
int DesktopCapture::OutHeight() const { return impl_->outH; }
int DesktopCapture::SourceWidth() const { return impl_->srcW; }
int DesktopCapture::SourceHeight() const { return impl_->srcH; }
ID3D11Device* DesktopCapture::Device() const { return impl_->device.Get(); }
std::string DesktopCapture::MonitorName() const { return impl_->monitorName; }
std::string DesktopCapture::GpuName() const { return impl_->gpuName; }

} // namespace cs::engine
