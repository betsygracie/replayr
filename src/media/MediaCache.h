#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;

namespace cs {

struct BannerStyle;

// A strip of evenly spaced frames from a video (for hover scrubbing and the editor timeline),
// decoded in the background with Media Foundation and cached on disk as a small JPEG, so it's
// instant the next time. Also records the video's frame rate.
struct StripInfo {
    uint64_t texture = 0;       // ID3D11ShaderResourceView* (ImTextureID), 0 while loading
    int frames = 0;             // frames side by side, each 16:9
    double fps = 0.0;
    int videoWidth = 0, videoHeight = 0;
    bool failed = false;
};

class Filmstrips {
public:
    static constexpr int kFrames = 12;
    static constexpr int kCellW = 240, kCellH = 135;

    Filmstrips();
    ~Filmstrips();
    bool Init(ID3D11Device* device, const std::wstring& cacheDir);
    void Shutdown();
    void NewFrame();
    StripInfo Get(const std::wstring& path, int64_t modified);          // queues a decode if needed
    std::wstring PosterPath(const std::wstring& path, int64_t modified);  // JPEG of one frame, "" if not cached yet
    void Request(const std::wstring& path, int64_t modified);             // warm the disk cache only

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Full-resolution images for the in-app viewer (WIC decode on a worker thread).
struct ImageInfo {
    uint64_t texture = 0;
    int width = 0, height = 0;
    bool failed = false;
};

class ImageCache {
public:
    ImageCache();
    ~ImageCache();
    bool Init(ID3D11Device* device);
    void Shutdown();
    ImageInfo Get(const std::wstring& path);
    void Prefetch(const std::wstring& path);   // e.g. the next/previous screenshot
    // The export banner rendered for a frameW x frameH video, as a texture (cached until the inputs change).
    ImageInfo Banner(int frameW, int frameH, const BannerStyle& style);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
