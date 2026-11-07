#include "media/MediaCache.h"
#include "engine/MediaWriter.h"   // MediaStartup / MediaShutdown
#include "media/Banner.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

struct Bgra {
    int w = 0, h = 0;
    std::vector<uint8_t> px;   // top-down BGRA
};

uint64_t Hash(const std::wstring& path, int64_t modified)
{
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : path) {
        wchar_t l = (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
        h = (h ^ (uint64_t)l) * 1099511628211ull;
    }
    h = (h ^ (uint64_t)modified) * 1099511628211ull;
    return h;
}

ComPtr<IWICImagingFactory> Wic()
{
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool LoadImageFile(IWICImagingFactory* wic, const std::wstring& path, Bgra& out, int maxDim)
{
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return false;
    ComPtr<IWICBitmapSource> src = frame;
    if ((int)std::max(w, h) > maxDim) {
        const double k = (double)maxDim / std::max(w, h);
        ComPtr<IWICBitmapScaler> sc;
        wic->CreateBitmapScaler(&sc);
        w = std::max<UINT>(1, (UINT)(w * k));
        h = std::max<UINT>(1, (UINT)(h * k));
        if (FAILED(sc->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
        src = sc;
    }
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    out.w = (int)w;
    out.h = (int)h;
    out.px.resize((size_t)w * h * 4);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)out.px.size(), out.px.data()))) return false;
    // Screenshots are opaque (captures can carry alpha 0); the app's own badge keeps its transparency.
    if (path.find(L"\\logo-") == std::wstring::npos)
        for (size_t i = 3; i < out.px.size(); i += 4) out.px[i] = 255;
    return true;
}

bool SaveJpeg(IWICImagingFactory* wic, const std::wstring& path, const Bgra& img)
{
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppBGRA, img.w * 4, (UINT)img.px.size(),
                                           const_cast<BYTE*>(img.px.data()), &bmp)))
        return false;
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    const std::wstring tmp = path + L".tmp";
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    bool ok = SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> fe;
    ComPtr<IPropertyBag2> props;
    ok = ok && SUCCEEDED(enc->CreateNewFrame(&fe, &props));
    if (ok && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.82f;
        props->Write(1, &opt, &v);
    }
    ok = ok && SUCCEEDED(fe->Initialize(props.Get()));
    ok = ok && SUCCEEDED(fe->SetSize(img.w, img.h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(fe->SetPixelFormat(&fmt));
    ok = ok && SUCCEEDED(fe->WriteSource(conv.Get(), nullptr));
    ok = ok && SUCCEEDED(fe->Commit()) && SUCCEEDED(enc->Commit());
    stream.Reset();
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

ComPtr<ID3D11ShaderResourceView> MakeTexture(ID3D11Device* dev, const Bgra& img)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = img.w;
    td.Height = img.h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ img.px.data(), (UINT)img.w * 4, 0 };
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &tex))) dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

// Cover-fit `src` into a w x h cell of `dst` at (dx, 0), box-filtered.
void Blit(const Bgra& src, Bgra& dst, int dx, int w, int h)
{
    const double box = (double)w / h, sa = (double)src.w / src.h;
    double sx0 = 0, sy0 = 0, sw = src.w, sh = src.h;
    if (sa > box) {
        sw = src.h * box;
        sx0 = (src.w - sw) * 0.5;
    } else {
        sh = src.w / box;
        sy0 = (src.h - sh) * 0.5;
    }
    for (int y = 0; y < h; ++y) {
        const double fy0 = sy0 + sh * y / h, fy1 = sy0 + sh * (y + 1) / h;
        for (int x = 0; x < w; ++x) {
            const double fx0 = sx0 + sw * x / w, fx1 = sx0 + sw * (x + 1) / w;
            const int x0 = (int)fx0, x1 = std::max(x0 + 1, (int)fx1), y0 = (int)fy0, y1 = std::max(y0 + 1, (int)fy1);
            unsigned acc[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1 && yy < src.h; yy += std::max(1, (y1 - y0) / 3))
                for (int xx = x0; xx < x1 && xx < src.w; xx += std::max(1, (x1 - x0) / 3)) {
                    const uint8_t* s = &src.px[((size_t)yy * src.w + xx) * 4];
                    acc[0] += s[0];
                    acc[1] += s[1];
                    acc[2] += s[2];
                    ++n;
                }
            uint8_t* d = &dst.px[((size_t)y * dst.w + dx + x) * 4];
            if (n) {
                d[0] = (uint8_t)(acc[0] / n);
                d[1] = (uint8_t)(acc[1] / n);
                d[2] = (uint8_t)(acc[2] / n);
            }
            d[3] = 255;
        }
    }
}

// Decodes `count` evenly spaced frames. Takes the first frame after each seek (a keyframe), which
// costs one decode per frame instead of decoding up to the exact time.
bool DecodeStrip(const std::wstring& path, Bgra& strip, double& fps, int& vw, int& vh)
{
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), &reader))) return false;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> native;
    if (FAILED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) return false;
    UINT32 w = 0, h = 0, num = 0, den = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den) fps = (double)num / den;
    vw = (int)w;
    vh = (int)h;
    if (!w || !h) return false;
    // Ask the decoder for a small RGB frame with the same aspect (cheap to copy and scale).
    UINT32 ow = std::min<UINT32>(w, 480), oh = std::max<UINT32>(2, (UINT32)((uint64_t)ow * h / w) & ~1u);
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, ow, oh);
    out->SetUINT32(MF_MT_DEFAULT_STRIDE, ow * 4);
    if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get()))) {
        // Older decoders can't resize: full size then.
        out->DeleteItem(MF_MT_FRAME_SIZE);
        out->DeleteItem(MF_MT_DEFAULT_STRIDE);
        if (FAILED(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get()))) return false;
    }
    ComPtr<IMFMediaType> cur;
    reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &ow, &oh);

    PROPVARIANT pv;
    PropVariantInit(&pv);
    double duration = 0.0;
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
        duration = pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);

    const int n = Filmstrips::kFrames;
    strip.w = Filmstrips::kCellW * n;
    strip.h = Filmstrips::kCellH;
    strip.px.assign((size_t)strip.w * strip.h * 4, 0);
    for (size_t i = 3; i < strip.px.size(); i += 4) strip.px[i] = 255;
    Bgra frame;
    frame.w = (int)ow;
    frame.h = (int)oh;
    frame.px.resize((size_t)ow * oh * 4);
    int got = 0;
    for (int i = 0; i < n; ++i) {
        if (duration > 0.0) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = (LONGLONG)(duration * (i + 0.5) / n * 1e7);
            reader->SetCurrentPosition(GUID_NULL, pos);
        }
        ComPtr<IMFSample> s;
        for (int tries = 0; tries < 30 && !s; ++tries) {
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &ts, &s))) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        if (!s) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(&buf))) continue;
        ComPtr<IMF2DBuffer> b2;
        BYTE* data = nullptr;
        LONG pitch = (LONG)ow * 4;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) locked2d = true;
        else if (FAILED(buf->Lock(&data, nullptr, nullptr))) continue;
        for (UINT y = 0; y < oh; ++y) memcpy(&frame.px[(size_t)y * ow * 4], data + (LONG_PTR)y * pitch, (size_t)ow * 4);
        if (locked2d) b2->Unlock2D();
        else buf->Unlock();
        for (size_t k = 3; k < frame.px.size(); k += 4) frame.px[k] = 255;
        Blit(frame, strip, i * Filmstrips::kCellW, Filmstrips::kCellW, Filmstrips::kCellH);
        ++got;
    }
    return got > 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Filmstrips
// ---------------------------------------------------------------------------------------------

struct Filmstrips::Impl {
    struct Entry {
        ComPtr<ID3D11ShaderResourceView> srv;
        StripInfo info;
        bool queued = false;
        uint64_t lastUse = 0;
    };
    ComPtr<ID3D11Device> device;
    std::wstring dir;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<uint64_t, Entry> entries;
    std::deque<std::pair<uint64_t, std::pair<std::wstring, int64_t>>> queue;   // newest first
    std::thread worker;
    std::atomic<bool> quit{ false };
    uint64_t frame = 0;
    bool started = false;

    std::wstring CachePath(uint64_t h, const wchar_t* suffix) const
    {
        wchar_t name[48];
        swprintf(name, 48, L"%016llx%ls", (unsigned long long)h, suffix);
        return JoinPath(dir, name);
    }
    void Run();
};

Filmstrips::Filmstrips() : impl_(std::make_unique<Impl>()) {}
Filmstrips::~Filmstrips() { Shutdown(); }

bool Filmstrips::Init(ID3D11Device* device, const std::wstring& cacheDir)
{
    Impl& m = *impl_;
    m.device = device;
    m.dir = cacheDir;
    EnsureDir(m.dir);
    engine::MediaStartup();
    m.started = true;
    m.worker = std::thread([this] { impl_->Run(); });
    return true;
}

void Filmstrips::Shutdown()
{
    Impl& m = *impl_;
    if (!m.started) return;
    m.quit = true;
    m.cv.notify_all();
    if (m.worker.joinable()) m.worker.join();
    m.entries.clear();
    m.started = false;
    engine::MediaShutdown();
}

void Filmstrips::NewFrame()
{
    Impl& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mutex);
    ++m.frame;
    // Keep at most 32 strips in VRAM (about 1.5 MB each): drop the least recently used.
    if (m.entries.size() > 32) {
        auto oldest = m.entries.end();
        for (auto it = m.entries.begin(); it != m.entries.end(); ++it)
            if (!it->second.queued && it->second.lastUse + 2 < m.frame && (oldest == m.entries.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest != m.entries.end()) m.entries.erase(oldest);
    }
}

StripInfo Filmstrips::Get(const std::wstring& path, int64_t modified)
{
    Impl& m = *impl_;
    const uint64_t h = Hash(path, modified);
    std::lock_guard<std::mutex> lock(m.mutex);
    Impl::Entry& e = m.entries[h];
    e.lastUse = m.frame;
    if (!e.srv && !e.queued && !e.info.failed) {
        e.queued = true;
        m.queue.push_front({ h, { path, modified } });
        m.cv.notify_one();
    }
    StripInfo info = e.info;
    info.texture = (uint64_t)e.srv.Get();
    return info;
}

void Filmstrips::Request(const std::wstring& path, int64_t modified)
{
    Impl& m = *impl_;
    const uint64_t h = Hash(path, modified);
    std::lock_guard<std::mutex> lock(m.mutex);
    m.queue.push_back({ h | 0, { path, modified } });
    m.cv.notify_one();
}

std::wstring Filmstrips::PosterPath(const std::wstring& path, int64_t modified)
{
    const std::wstring p = impl_->CachePath(Hash(path, modified), L"_poster.jpg");
    return FileExists(p) ? p : L"";
}

void Filmstrips::Impl::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic = Wic();
    while (!quit) {
        uint64_t h;
        std::wstring path;
        int64_t modified;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return quit || !queue.empty(); });
            if (quit) break;
            h = queue.front().first;
            path = queue.front().second.first;
            modified = queue.front().second.second;
            queue.pop_front();
            // Skip requests for strips nobody looked at recently (scrolled past) - unless it's a warm-up.
            auto it = entries.find(h);
            if (it != entries.end() && it->second.srv) continue;
        }
        const std::wstring stripFile = CachePath(h, L".jpg"), metaFile = CachePath(h, L".txt"), posterFile = CachePath(h, L"_poster.jpg");
        Bgra strip;
        StripInfo info;
        info.frames = Filmstrips::kFrames;
        bool ok = false;
        if (FileExists(stripFile) && wic && LoadImageFile(wic.Get(), stripFile, strip, 16384)) {
            ok = true;
            if (FILE* f = _wfopen(metaFile.c_str(), L"rb")) {
                fscanf(f, "%lf %d %d", &info.fps, &info.videoWidth, &info.videoHeight);
                fclose(f);
            }
        } else if (DecodeStrip(path, strip, info.fps, info.videoWidth, info.videoHeight)) {
            ok = true;
            if (wic) {
                SaveJpeg(wic.Get(), stripFile, strip);
                Bgra poster;
                poster.w = Filmstrips::kCellW;
                poster.h = Filmstrips::kCellH;
                poster.px.resize((size_t)poster.w * poster.h * 4);
                const int cell = 1;
                for (int y = 0; y < poster.h; ++y)
                    memcpy(&poster.px[(size_t)y * poster.w * 4], &strip.px[((size_t)y * strip.w + cell * Filmstrips::kCellW) * 4], (size_t)poster.w * 4);
                SaveJpeg(wic.Get(), posterFile, poster);
            }
            if (FILE* f = _wfopen(metaFile.c_str(), L"wb")) {
                fprintf(f, "%.6f %d %d\n", info.fps, info.videoWidth, info.videoHeight);
                fclose(f);
            }
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        if (ok) srv = MakeTexture(device.Get(), strip);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(h);
        if (it == entries.end()) continue;   // warm-up only, or evicted meanwhile
        it->second.queued = false;
        it->second.info = info;
        it->second.info.failed = !ok || !srv;
        it->second.srv = srv;
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------------------------
// ImageCache
// ---------------------------------------------------------------------------------------------

struct ImageCache::Impl {
    struct Entry {
        ComPtr<ID3D11ShaderResourceView> srv;
        ImageInfo info;
        bool queued = false;
        uint64_t lastUse = 0;
    };
    ComPtr<ID3D11Device> device;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::wstring, Entry> entries;
    std::deque<std::wstring> queue;
    std::thread worker;
    std::atomic<bool> quit{ false };
    uint64_t tick = 0;
    bool started = false;
    ComPtr<ID3D11ShaderResourceView> bannerSrv;
    ImageInfo bannerInfo;
    std::string bannerKey;
    void Run();
    void Enqueue(const std::wstring& path, bool front);
};

ImageCache::ImageCache() : impl_(std::make_unique<Impl>()) {}
ImageCache::~ImageCache() { Shutdown(); }

bool ImageCache::Init(ID3D11Device* device)
{
    impl_->device = device;
    impl_->started = true;
    impl_->worker = std::thread([this] { impl_->Run(); });
    return true;
}

void ImageCache::Shutdown()
{
    Impl& m = *impl_;
    if (!m.started) return;
    m.quit = true;
    m.cv.notify_all();
    if (m.worker.joinable()) m.worker.join();
    m.entries.clear();
    m.started = false;
}

void ImageCache::Impl::Enqueue(const std::wstring& path, bool front)
{
    Entry& e = entries[path];
    e.lastUse = ++tick;
    if (e.srv || e.queued || e.info.failed) return;
    e.queued = true;
    if (front) queue.push_front(path);
    else queue.push_back(path);
    cv.notify_one();
    // Full-size images are big: keep the 5 most recent.
    while (entries.size() > 5) {
        auto oldest = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (!it->second.queued && (oldest == entries.end() || it->second.lastUse < oldest->second.lastUse)) oldest = it;
        if (oldest == entries.end()) break;
        entries.erase(oldest);
    }
}

ImageInfo ImageCache::Get(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Enqueue(path, true);
    Impl::Entry& e = impl_->entries[path];
    ImageInfo info = e.info;
    info.texture = (uint64_t)e.srv.Get();
    return info;
}

void ImageCache::Prefetch(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Enqueue(path, false);
}

ImageInfo ImageCache::Banner(int frameW, int frameH, const BannerStyle& style)
{
    Impl& m = *impl_;
    char key[160];
    snprintf(key, sizeof(key), "%d %d %d %.3f %.3f %.3f %.3f %.3f %.3f", frameW, frameH, style.corner, style.accent[0], style.accent[1],
             style.accent[2], style.accent2[0], style.accent2[1], style.accent2[2]);
    if (m.bannerKey != key) {
        m.bannerKey = key;
        m.bannerSrv.Reset();
        m.bannerInfo = ImageInfo{};
        const BannerLayout l = BannerLayoutFor(frameW, frameH, style.corner);
        Bgra img;
        if (RenderBanner(l, style, img.px)) {
            img.w = l.w + l.pad * 2;
            img.h = l.h + l.pad * 2;
            // ImGui blends with straight alpha: un-premultiply for the preview texture.
            for (size_t i = 0; i < img.px.size(); i += 4) {
                const int al = img.px[i + 3];
                if (al > 0 && al < 255)
                    for (int k = 0; k < 3; ++k) img.px[i + k] = (uint8_t)std::min(255, img.px[i + k] * 255 / al);
            }
            m.bannerSrv = MakeTexture(m.device.Get(), img);
            m.bannerInfo.width = img.w;
            m.bannerInfo.height = img.h;
        }
        m.bannerInfo.failed = !m.bannerSrv;
    }
    ImageInfo info = m.bannerInfo;
    info.texture = (uint64_t)m.bannerSrv.Get();
    return info;
}

void ImageCache::Impl::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic = Wic();
    while (!quit) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return quit || !queue.empty(); });
            if (quit) break;
            path = queue.front();
            queue.pop_front();
        }
        Bgra img;
        const bool ok = wic && LoadImageFile(wic.Get(), path, img, 8192);
        ComPtr<ID3D11ShaderResourceView> srv;
        if (ok) srv = MakeTexture(device.Get(), img);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(path);
        if (it == entries.end()) continue;
        it->second.queued = false;
        it->second.srv = srv;
        it->second.info.width = img.w;
        it->second.info.height = img.h;
        it->second.info.failed = !srv;
    }
    CoUninitialize();
}

} // namespace cs
