#pragma once
#include "imgui.h"

// Per-ID easing state shared by every widget. Values live in one global table (not in a window's
// storage), so popups and their parents can animate the same thing, and entries nobody has touched
// for a few seconds are dropped.
namespace cs::ui {

// Frame-rate independent ease towards `target`: v += (target - v) * (1 - exp(-speed * dt)).
float Anim(ImGuiID id, float target, float speed);
// Hover fade: ~150 ms in, ~250 ms out.
float AnimHover(ImGuiID id, bool on);

// Damped spring, frame-rate independent. Damping ratio = damping / (2 * sqrt(stiffness)): below 1 it
// overshoots a little, which is what makes buttons and knobs feel physical.
struct SpringParams {
    float stiffness;
    float damping;
};
namespace Springs {
constexpr SpringParams Hover { 380.0f, 26.0f };   // ratio 0.67: ~6% overshoot (scale, lift)
constexpr SpringParams Press { 900.0f, 48.0f };   // ratio 0.80: quick, firm push down
constexpr SpringParams Release { 420.0f, 20.0f }; // ratio 0.49: springy rebound after a click
constexpr SpringParams Knob { 300.0f, 20.0f };    // ratio 0.58: toggle knob
constexpr SpringParams Pill { 260.0f, 24.0f };    // ratio 0.74: sidebar / tab indicator
constexpr SpringParams Tilt { 160.0f, 20.0f };    // ratio 0.79: card tilt following the cursor
} // namespace Springs
float Spring(ImGuiID id, float target, const SpringParams& p);
float SpringVelocity(ImGuiID id);
void SpringKick(ImGuiID id, float velocity);       // add velocity (e.g. a bounce on release)

void AnimSet(ImGuiID id, float value);              // jump without easing
ImGuiID AnimId(ImGuiID base, unsigned salt);        // derive a second id from a widget id

// One-shot timers (ripples, popups opening, "Saved" flashes).
void TimerStart(ImGuiID id);
float TimerElapsed(ImGuiID id);                     // seconds since TimerStart, or a large value
void TimerStop(ImGuiID id);

float EaseOutCubic(float t);
float EaseInOutCubic(float t);

void AnimNewFrame();                                // once per frame: garbage-collects stale entries

} // namespace cs::ui
