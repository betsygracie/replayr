#pragma once
// Keyboard & mouse overlay ("key viewer"): a small glass keyboard (the gaming keys) and a mouse that
// light up as you press them. Other keys pop up as pills above it.
//
// Input comes from Raw Input on a private thread (nothing is logged or saved). The overlay can be
//   - on screen and in clips: a click-through topmost window, which the capture picks up anyway;
//   - only on screen: the same window, excluded from capture;
//   - only in clips: no window; the capture draws it into each frame (DrawIntoCapture).
#include <cstdint>
#include <memory>
#include <vector>

namespace cs {

struct InputOverlayConfig {
    bool enabled = false;
    int mode = 0;          // 0 screen + clips, 1 screen only, 2 clips only
    int corner = 2;        // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    int size = 1;          // 0 small, 1 medium, 2 large
    int show = 0;          // 0 keyboard + mouse, 1 keyboard, 2 mouse
    float opacity = 0.9f;
    bool gamesOnly = true; // only while a fullscreen / borderless app is in front
    float accent[3] = { 0.545f, 0.247f, 0.988f };
};

// One key of the drawn layout, in key units (1 = a letter key).
struct OverlayKey {
    float x, y, w, h;
    int vk;                // Windows virtual-key code; mouse parts use 0x01 L, 0x02 R, 0x04 M, 0x05/0x06 side
    const char* label;
};
// The layout for `show`, and its size in key units. Shared by the renderer and the settings preview.
const std::vector<OverlayKey>& OverlayLayout(int show, float& widthUnits, float& heightUnits);
// Display name of a key that isn't in the layout ("Enter", "F5", ...).
const char* OverlayKeyName(int vk);

class InputOverlay {
public:
    InputOverlay();
    ~InputOverlay();
    void Start();
    void SetConfig(const InputOverlayConfig& c);
    // Live state for the settings preview: is `vk` (or mouse button) down right now.
    bool IsDown(int vk) const;
    std::vector<int> RecentExtraKeys() const;   // non-layout keys pressed in the last ~1.2 s, newest last

    // Called by the capture on its own thread: draws the overlay into the frame's DC when the
    // mode is "only in clips" (or "screen + clips" while the on-screen window can't be shown).
    static void DrawIntoCapture(void* hdc, int frameW, int frameH, int monitorLeft, int monitorTop, int monitorRight, int monitorBottom);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace cs
