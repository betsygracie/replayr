#include "core/Thumbnailer.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <d3d11.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {
struct Entry {
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    uint64_t lastUsed = 0;
    bool pending = true;
    bool queued = false;
    bool failed = false;
};
struct Job {
    std::wstring key;
    std::wstring path;
    int size = 256;
};
} // namespace

struct Thumbnailer::Impl {
    ComPtr<ID3D11Device> device;
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::wstring, Entry> cache;
    std::vector<Job> stack;           // LIFO: the most recently requested thumbnail loads first
    std::thread worker;
    bool quit = false;
    uint64_t frame = 0;
    size_t maxEntries = 360;

    void Run();
};

static bool LoadShellThumbnail(const std::wstring& path, int size, std::vector<unsigned char>& px, int& w, int& h)
{
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP bmp = nullptr;
    SIZE sz{ size, size };
    HRESULT hr = factory->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &bmp);
    if (FAILED(hr) || !bmp) return false;

    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    w = bm.bmWidth;
    h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        DeleteObject(bmp);
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    px.resize((size_t)w * h * 4);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, (UINT)h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    if (lines != h) return false;

    // Many thumbnail providers leave alpha at 0 for opaque images.
    bool anyAlpha = false;
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i]) { anyAlpha = true; break; }
    if (!anyAlpha)
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    return true;
}

void Thumbnailer::Impl::Run()
{
    // Shell thumbnail handlers expect an STA.
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::vector<unsigned char> px;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return quit || !stack.empty(); });
            if (quit) break;
            job = std::move(stack.back());
            stack.pop_back();
            auto it = cache.find(job.key);
            if (it == cache.end()) continue;
            if (frame > it->second.lastUsed + 60) {   // scrolled away: re-queued if it becomes visible again
                it->second.queued = false;
                continue;
            }
        }

        int w = 0, h = 0;
        ComPtr<ID3D11ShaderResourceView> srv;
        bool ok = LoadShellThumbnail(job.path, job.size, px, w, h);
        if (ok) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w;
            td.Height = (UINT)h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{ px.data(), (UINT)(w * 4), 0 };
            ComPtr<ID3D11Texture2D> tex;
            ok = SUCCEEDED(device->CreateTexture2D(&td, &init, &tex)) &&
                 SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
        }

        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(job.key);
        if (it == cache.end()) continue;
        it->second.pending = false;
        it->second.queued = false;
        it->second.failed = !ok;
        it->second.srv = srv;
        it->second.w = w;
        it->second.h = h;
    }
    if (SUCCEEDED(coHr)) CoUninitialize();
}

Thumbnailer::Thumbnailer() : impl_(std::make_unique<Impl>()) {}
Thumbnailer::~Thumbnailer() { Shutdown(); }

bool Thumbnailer::Init(ID3D11Device* device)
{
    impl_->device = device;
    impl_->quit = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
    return true;
}

void Thumbnailer::Shutdown()
{
    if (!impl_ || !impl_->worker.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    impl_->worker.join();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cache.clear();
    impl_->stack.clear();
}

void Thumbnailer::NewFrame()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->frame++;
    if (impl_->cache.size() <= impl_->maxEntries) return;

    // Evict the least recently drawn thumbnails (never ones still loading or drawn last frame).
    std::vector<std::pair<uint64_t, std::wstring>> candidates;
    for (auto& kv : impl_->cache)
        if (!kv.second.pending && kv.second.lastUsed + 2 < impl_->frame)
            candidates.emplace_back(kv.second.lastUsed, kv.first);
    std::sort(candidates.begin(), candidates.end());
    size_t target = impl_->maxEntries * 3 / 4;
    for (auto& c : candidates) {
        if (impl_->cache.size() <= target) break;
        impl_->cache.erase(c.second);
    }
}

ThumbInfo Thumbnailer::Get(const std::wstring& path, int64_t modified, int sizePx)
{
    const int bucket = sizePx <= 256 ? 256 : 512;
    std::wstring key = path;
    key += L"|";
    key += IntToW(modified);
    key += L"|";
    key += IntToW(bucket);

    ThumbInfo info;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->cache.find(key);
    if (it == impl_->cache.end()) {
        Entry e;
        e.lastUsed = impl_->frame;
        e.queued = true;
        impl_->cache.emplace(key, std::move(e));
        impl_->stack.push_back(Job{ key, path, bucket });
        impl_->cv.notify_one();
        return info;
    }
    Entry& e = it->second;
    e.lastUsed = impl_->frame;
    if (e.pending) {
        if (!e.queued) {
            e.queued = true;
            impl_->stack.push_back(Job{ key, path, bucket });
            impl_->cv.notify_one();
        }
        return info;
    }
    info.failed = e.failed;
    info.texture = (uint64_t)(uintptr_t)e.srv.Get();
    info.width = e.w;
    info.height = e.h;
    return info;
}

void Thumbnailer::Forget(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::wstring prefix = path + L"|";
    for (auto it = impl_->cache.begin(); it != impl_->cache.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0 && !it->second.pending) it = impl_->cache.erase(it);
        else ++it;
    }
}

} // namespace cs
