#pragma once
#include <cstdint>
#include <vector>

// The "Clipped using Replayr" banner. One design, used three ways:
// - exports: rendered once at the video's resolution, then blended into every frame with the
//   animation below (slide + fade in, shine sweep, slide + fade out)
// - the editor: the same bitmap as a texture, animated with the same timing, as a live preview
// - screenshots: stamped as a still
namespace cs {

enum class BannerMode { StartFiveSeconds = 0, Always = 1 };

struct BannerStyle {
    int corner = 3;                                  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    BannerMode mode = BannerMode::StartFiveSeconds;
    float accent[3] = { 0.545f, 0.247f, 0.988f };
    float accent2[3] = { 0.761f, 0.122f, 0.878f };
};

// Where the pill rests in a frame (pixels). The bitmap is larger than the pill by `pad` on every side
// (room for the soft shadow), so it's drawn at (x - pad, y - pad).
struct BannerLayout {
    int x = 0, y = 0, w = 0, h = 0;   // the pill
    int pad = 0;
    int margin = 0;
};
BannerLayout BannerLayoutFor(int frameW, int frameH, int corner);

// Animation state at time t (seconds from the start of the exported clip).
struct BannerFrame {
    bool visible = false;
    float alpha = 0.0f;               // 0..1
    float slide = 0.0f;               // pixels to move towards the nearest side edge (0 = resting)
    float shine = -1.0f;              // 0..1 position of the shine sweep, < 0 = none
};
BannerFrame BannerAt(double t, const BannerLayout& layout, BannerMode mode);

// Renders the banner (pill + shadow) as premultiplied BGRA, (w + 2 pad) x (h + 2 pad). Uses GDI+
// with the Poppins font embedded in the exe. Thread-safe; call from any thread.
bool RenderBanner(const BannerLayout& layout, const BannerStyle& style, std::vector<uint8_t>& premulBgra);

// Blends the banner into a BGRA frame (top-down, `stride` bytes per row) at its animated position.
void CompositeBanner(uint8_t* frame, int stride, int frameW, int frameH, const std::vector<uint8_t>& banner, const BannerLayout& layout,
                     const BannerFrame& f, int corner);

// Text captions burned into exports: bold white text with a dark outline and soft shadow, so it
// reads on any game. size: 0 small, 1 medium, 2 large (4.5% / 6.5% / 9% of the frame height).
// Renders premultiplied BGRA sized to the text (plus room for the outline).
bool RenderCaption(const std::wstring& text, int size, int frameW, int frameH, std::vector<uint8_t>& premulBgra, int& w, int& h);
// Alpha-blends a premultiplied BGRA image into a BGRA frame at (x, y), scaled by alpha (0..1).
void CompositeImage(uint8_t* frame, int stride, int frameW, int frameH, const std::vector<uint8_t>& img, int w, int h, int x, int y, float alpha);
// Caption height as a fraction of the frame height, for the editor preview.
float CaptionHeightFraction(int size);

} // namespace cs
