#pragma once
// GPU background + frosted-glass source.
//
// Every frame (at most 30 times a second, and not at all while nothing moves) it draws the animated
// gradient-mesh background into a full-size texture, then blurs it once with a dual-Kawase chain:
//
//   background (1/1) -> 1/2 -> 1/4 -> 1/8 -> 1/16
//                                  1/8 -> up -> 1/4  = Blurred()        (cards, panels, buttons)
//                                         1/16 -> up -> 1/8 = BlurredStrong()  (popups, menus)
//
// The UI draws Background() behind everything and each glass panel samples its own screen rect of a
// blurred texture, so 50 panels cost the same as one. If shaders can't be created (old drivers,
// Wine), Ok() is false and the UI falls back to its CPU-drawn backdrop.
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace cs {

struct GlassFrameParams {
    double time = 0.0;               // seconds; frozen for the static style
    float mouse[2] = { 0.5f, 0.5f }; // 0..1 inside the window
    float mouseAmount = 0.03f;       // how far blobs lean towards the cursor (fraction of the window)
    float accent[3] = { 0.545f, 0.361f, 1.0f };
    float accent2[3] = { 0.878f, 0.275f, 0.784f };
    int style = 0;                   // 0 animated, 1 static, 2 solid
    float alpha = 1.0f;              // < 1 lets Windows' Mica/Acrylic show through
    float saturation = 1.25f;        // boost applied to the blurred copies
    bool moving = true;              // false: keep the last frame (unfocused, minimised, game fullscreen)
};

class GlassRenderer {
public:
    GlassRenderer();
    ~GlassRenderer();
    GlassRenderer(const GlassRenderer&) = delete;
    GlassRenderer& operator=(const GlassRenderer&) = delete;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
    // Before ImGui::NewFrame: (re)creates the textures for this window size so the ids handed to the
    // UI this frame stay valid until it is rendered.
    void Prepare(int width, int height);
    // After ImGui::Render, before the UI is drawn: updates the textures if anything changed.
    void Render(const GlassFrameParams& p);

    bool Ok() const;
    // ImTextureIDs (ID3D11ShaderResourceView*), or 0 when unavailable.
    uint64_t Background() const;
    uint64_t Blurred() const;
    uint64_t BlurredStrong() const;

private:
    struct Impl;
    Impl* d_;
};

} // namespace cs
