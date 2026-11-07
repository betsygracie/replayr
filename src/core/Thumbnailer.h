#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;

namespace cs {

struct ThumbInfo {
    uint64_t texture = 0;   // ID3D11ShaderResourceView*, usable as ImTextureID; 0 while loading
    int width = 0;
    int height = 0;
    bool failed = false;
};

// Asynchronous thumbnail cache.
// - Windows' own thumbnail providers (IShellItemImageFactory) decode videos and images, so there
//   is no FFmpeg dependency here, and HEVC/AV1 work if the Store codec extensions are installed.
// - A worker thread decodes and creates IMMUTABLE textures directly (ID3D11Device is free-threaded),
//   so the UI thread never blocks on disk I/O or texture uploads.
// - Requests are served newest-first and dropped if the card scrolled out of view, so fast
//   scrolling through a big library only loads what is on screen. LRU eviction caps VRAM use.
class Thumbnailer {
public:
    Thumbnailer();
    ~Thumbnailer();
    bool Init(ID3D11Device* device);
    void Shutdown();
    void NewFrame();
    ThumbInfo Get(const std::wstring& path, int64_t modified, int sizePx);
    void Forget(const std::wstring& path);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
