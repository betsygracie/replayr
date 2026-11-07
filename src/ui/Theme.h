#pragma once
#include "imgui.h"

// Replayr design system: frosted glass panels with thin bright borders, floating over a soft
// violet/magenta backdrop. Every colour, size and timing used by the UI lives here.
namespace cs::ui {

// ---- spacing, radii, sizes (unscaled px; wrap with S()) ----------------------------------
namespace Tok {
constexpr float Space1 = 4.0f, Space2 = 8.0f, Space3 = 12.0f, Space4 = 16.0f, Space5 = 20.0f, Space6 = 24.0f, Space8 = 32.0f;

constexpr float RadiusPanel = 24.0f;     // sidebar, top bar, big panels
constexpr float RadiusCard = 14.0f;      // clip cards, setting groups
constexpr float RadiusControl = 10.0f;   // buttons, inputs, dropdowns
constexpr float RadiusSmall = 6.0f;      // badges, key caps

constexpr float TopBarHeight = 56.0f;
constexpr float SidebarWidth = 236.0f;
constexpr float SidebarCollapsed = 76.0f;
constexpr float SidebarCollapseBelow = 1180.0f;   // window width (scaled) under which the sidebar shows icons only
constexpr float ControlHeight = 36.0f;
constexpr float ControlHeightSmall = 30.0f;
constexpr float NavItemHeight = 42.0f;
constexpr float CardMinWidth = 250.0f;
constexpr int CardMaxColumns = 5;
constexpr float ScrollbarSize = 8.0f;
constexpr float DisabledAlpha = 0.40f;

// Glass
constexpr float GlassFillTop = 0.085f;    // white alpha at the top of a panel
constexpr float GlassFillBottom = 0.040f; // white alpha at the bottom
constexpr float GlassBorder = 0.22f;      // panel border (white alpha), like the reference
constexpr float GlassBorderSoft = 0.10f;  // cards and controls
constexpr float ShadowAlpha = 0.32f;
} // namespace Tok

// ---- motion -------------------------------------------------------------------------------
// Anim speeds: value += (target - value) * (1 - exp(-speed * dt)); 90% there after ~2.3/speed s.
namespace Motion {
constexpr float Hover = 18.0f;            // ~125 ms
constexpr float Press = 28.0f;            // ~80 ms
constexpr float Slide = 13.0f;            // nav pill, tabs, toggles (~175 ms)
constexpr float Page = 12.0f;             // page fade/slide (~190 ms)
constexpr float PopupSeconds = 0.16f;     // menus, dropdowns, modals
constexpr float TooltipDelay = 0.45f;
constexpr float TooltipFade = 0.12f;
constexpr float RippleSeconds = 0.45f;
constexpr float HoverLift = 3.0f;         // cards rise by this many px
constexpr float PressScale = 0.97f;
constexpr float SavedFlashSeconds = 1.6f;
} // namespace Motion

// ---- type ---------------------------------------------------------------------------------
namespace Type {
constexpr float Title = 23.0f;            // page titles
constexpr float Heading = 16.5f;          // section headings
constexpr float Body = 14.0f;
constexpr float Caption = 12.0f;
} // namespace Type

struct Fonts {
    ImFont* regular = nullptr;   // Poppins Regular (+ Segoe UI fallback + icons merged)
    ImFont* medium = nullptr;    // Poppins Medium
    ImFont* semibold = nullptr;  // Poppins SemiBold
    ImFont* icons = nullptr;     // icon font alone, for big centred icons
};

struct ThemeParams {
    float accent[3] = { 0.545f, 0.247f, 0.988f };   // #8B3FFC violet
    float accent2[3] = { 0.761f, 0.122f, 0.878f };  // #C21FE0 magenta (gradient end)
    bool animations = true;
    bool backdrop = false;       // Windows Mica/Acrylic shows through instead of the built-in art
    int background = 0;          // 0 animated, 1 static, 2 solid
    float dpiScale = 1.0f;
    float uiScale = 1.0f;
};

struct Palette {
    ImVec4 accent, accent2;
    ImU32 text, textDim, textFaint;
    ImU32 danger, record, success, warning;
    ImU32 surface;               // opaque-ish surface for popups/menus
};

Fonts& GetFonts();
void LoadFonts();
void ApplyTheme(const ThemeParams& p);
const ThemeParams& Params();
const Palette& Pal();
float Scale();
inline float S(float v) { return v * Scale(); }
bool Animations();

// Colour helpers
ImU32 ColA(const ImVec4& c, float alpha);
ImU32 ColA(ImU32 c, float alphaMul);
ImU32 Mix(ImU32 a, ImU32 b, float t);
ImU32 White(float alpha);
ImU32 Accent(float alpha = 1.0f);
ImU32 Accent2(float alpha = 1.0f);

// Drawing primitives
void GradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float rounding, ImDrawFlags flags = 0);
void GradientRectH(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right, float rounding, ImDrawFlags flags = 0);
void RadialGlow(ImDrawList* dl, ImVec2 center, float radius, ImU32 inner, int segments = 48);
// Soft drop shadow. strength 1 = layer 1 (24 px), ~1.3 = layer 2, ~1.8 = layer 3 (48 px, offset down).
void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float strength = 1.0f);
// Where frosted glass gets its blur from: the GPU background and its blurred copies (GlassRenderer),
// sampled per panel by screen position. With no textures the glass falls back to tinted fills.
struct GlassSource {
    ImTextureID background = 0;
    ImTextureID blurred = 0;      // layers 1-2
    ImTextureID strong = 0;       // layer 3 (popups, menus): more frosted
    ImVec2 origin = ImVec2(0, 0); // viewport the textures cover
    ImVec2 size = ImVec2(1, 1);
};
void SetGlassSource(const GlassSource& s);
const GlassSource& GetGlassSource();

// Frosted glass. Layer 1: sidebar, top bar, page. Layer 2: cards and controls, a bit more opaque.
// Layer 3: popups, menus, toasts: more frosted, darker, stronger shadow.
// Blurred backdrop + tint + 1px gradient border (bright top-left, fading bottom-right) + top highlight.
void Glass(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float hover = 0.0f, float borderAlpha = Tok::GlassBorderSoft,
           ImDrawFlags flags = 0, int layer = 1);
// Recolours vertices [start, end) of a draw list: alpha fades from a0 at the top-left corner to a1 at
// the bottom-right (multiplies the existing alpha), optionally tinting towards `tint` by `tintAmount`.
void FadeVertsDiagonal(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, float a0, float a1, ImU32 tint = 0, float tintAmount = 0.0f);
// Fake 3D: moves vertices [start, end) as if the rect a..b were tilted by tx (left/right) and ty
// (up/down) radians, and scaled around its centre.
void TiltVerts(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, float tx, float ty, float scale, int end = -1);
// Scales vertices [start, end) around `origin` (popups opening from where they were clicked).
void ScaleVerts(ImDrawList* dl, int start, ImVec2 origin, float scale);
void AccentFill(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float hover = 0.0f, ImDrawFlags flags = 0);
void DrawBackdrop(ImDrawList* dl, ImVec2 min, ImVec2 max, float time);
void Icon(ImDrawList* dl, const char* icon, ImVec2 center, float size, ImU32 col);
ImVec2 IconSize(const char* icon, float size);

void PushFont(ImFont* font, float size);   // size before global scaling
void PopFont();

} // namespace cs::ui
