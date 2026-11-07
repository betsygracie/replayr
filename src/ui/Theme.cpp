#include "ui/Theme.h"
#include "ui/Icons.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Util.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cs::ui {

static Fonts g_fonts;
static ThemeParams g_params;
static Palette g_pal;

Fonts& GetFonts() { return g_fonts; }
const ThemeParams& Params() { return g_params; }
const Palette& Pal() { return g_pal; }
float Scale() { return g_params.dpiScale * g_params.uiScale; }
bool Animations() { return g_params.animations; }

// ---- colours -----------------------------------------------------------------------------

ImU32 ColA(const ImVec4& c, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, Clamp(alpha, 0.0f, 1.0f)));
}

ImU32 ColA(ImU32 c, float alphaMul)
{
    const int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * Clamp(alphaMul, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    t = Clamp(t, 0.0f, 1.0f);
    auto ch = [&](int shift) {
        const int x = (int)((a >> shift) & 0xFF), y = (int)((b >> shift) & 0xFF);
        return (ImU32)(x + (int)std::lround((y - x) * t)) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImU32 White(float alpha) { return IM_COL32(255, 255, 255, (int)(255 * Clamp(alpha, 0.0f, 1.0f))); }
ImU32 Accent(float alpha) { return ColA(g_pal.accent, alpha); }
ImU32 Accent2(float alpha) { return ColA(g_pal.accent2, alpha); }

// ---- fonts -------------------------------------------------------------------------------

static ImFont* AddFont(const wchar_t* resource, const wchar_t* file, const std::wstring& fallbackSystem, const ImFontConfig& base)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg = base;
    size_t size = 0;
    if (const void* data = platform::EmbeddedResource(resource, &size)) {
        cfg.FontDataOwnedByAtlas = false;   // the exe's resource section outlives the atlas
        if (ImFont* f = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), (int)size, Type::Body, &cfg)) return f;
    }
    const std::wstring local = JoinPath(JoinPath(ExeDir(), L"assets\\fonts"), file);
    if (FileExists(local))
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(ToUtf8(local).c_str(), Type::Body, &cfg)) return f;
    if (!fallbackSystem.empty() && FileExists(fallbackSystem))
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(ToUtf8(fallbackSystem).c_str(), Type::Body, &cfg)) return f;
    return nullptr;
}

void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    const std::wstring sys = WindowsFontsDir();
    const std::wstring segoe = JoinPath(sys, L"segoeui.ttf");
    std::wstring icons = JoinPath(sys, L"SegoeIcons.ttf");          // Windows 11: Segoe Fluent Icons
    if (!FileExists(icons)) icons = JoinPath(sys, L"segmdl2.ttf");  // Windows 10: Segoe MDL2 Assets
    const bool haveIcons = FileExists(icons);
    const std::string iconsUtf8 = ToUtf8(icons), segoeUtf8 = ToUtf8(segoe);

    static const ImWchar iconRanges[] = { (ImWchar)ICON_RANGE_MIN, (ImWchar)ICON_RANGE_MAX, 0 };
    auto build = [&](const wchar_t* res, const wchar_t* file, const wchar_t* sysFallback) -> ImFont* {
        ImFontConfig cfg;
        ImFont* f = AddFont(res, file, JoinPath(sys, sysFallback), cfg);
        if (!f) f = io.Fonts->AddFontDefault();
        // Characters Poppins doesn't have (accents beyond Latin, CJK game names...) come from Segoe UI.
        if (FileExists(segoe)) {
            ImFontConfig m;
            m.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(segoeUtf8.c_str(), Type::Body, &m);
        }
        if (haveIcons) {
            ImFontConfig m;
            m.MergeMode = true;
            m.GlyphMinAdvanceX = Type::Body;
            m.GlyphOffset = ImVec2(0.0f, Type::Body * 0.08f);
            io.Fonts->AddFontFromFileTTF(iconsUtf8.c_str(), Type::Body, &m, iconRanges);
        }
        return f;
    };
    g_fonts.regular = build(L"FONT_REGULAR", L"Poppins-Regular.ttf", L"segoeui.ttf");
    g_fonts.medium = build(L"FONT_MEDIUM", L"Poppins-Medium.ttf", L"seguisb.ttf");
    g_fonts.semibold = build(L"FONT_SEMIBOLD", L"Poppins-SemiBold.ttf", L"seguisb.ttf");
    g_fonts.icons = haveIcons ? io.Fonts->AddFontFromFileTTF(iconsUtf8.c_str(), Type::Body) : nullptr;
    if (!g_fonts.icons) g_fonts.icons = g_fonts.regular;
    Log("Fonts: Poppins (embedded), icons=%s", haveIcons ? iconsUtf8.c_str() : "none");
}

void PushFont(ImFont* font, float size) { ImGui::PushFont(font, size); }
void PopFont() { ImGui::PopFont(); }

// ---- style -------------------------------------------------------------------------------

void ApplyTheme(const ThemeParams& p)
{
    g_params = p;
    g_pal.accent = ImVec4(p.accent[0], p.accent[1], p.accent[2], 1.0f);
    g_pal.accent2 = ImVec4(p.accent2[0], p.accent2[1], p.accent2[2], 1.0f);
    g_pal.text = IM_COL32(255, 255, 255, 245);
    g_pal.textDim = IM_COL32(255, 255, 255, 172);
    g_pal.textFaint = IM_COL32(255, 255, 255, 115);
    g_pal.danger = IM_COL32(255, 77, 94, 255);     // #FF4D5E
    g_pal.record = IM_COL32(255, 59, 59, 255);     // #FF3B3B
    g_pal.success = IM_COL32(61, 220, 151, 255);
    g_pal.warning = IM_COL32(255, 184, 0, 255);
    g_pal.surface = IM_COL32(34, 20, 64, 242);

    ImGuiStyle s;
    s.WindowPadding = ImVec2(Tok::Space4, Tok::Space4);
    s.FramePadding = ImVec2(Tok::Space3, 8);
    s.CellPadding = ImVec2(Tok::Space2, Tok::Space2);
    s.ItemSpacing = ImVec2(Tok::Space2, Tok::Space2);
    s.ItemInnerSpacing = ImVec2(Tok::Space2, Tok::Space1);
    s.ScrollbarSize = Tok::ScrollbarSize;
    s.GrabMinSize = 24;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowRounding = Tok::RadiusCard;
    s.ChildRounding = Tok::RadiusCard;
    s.FrameRounding = Tok::RadiusControl;
    s.PopupRounding = Tok::RadiusCard;
    s.ScrollbarRounding = 8;
    s.GrabRounding = Tok::RadiusControl;
    s.DisabledAlpha = Tok::DisabledAlpha;
    s.HoverDelayShort = 0.15f;
    s.HoverDelayNormal = Motion::TooltipDelay;
    s.HoverFlagsForTooltipMouse = ImGuiHoveredFlags_Stationary | ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled;

    ImVec4* c = s.Colors;
    auto white = [](float a) { return ImVec4(1, 1, 1, a); };
    const ImVec4 acc = g_pal.accent;
    auto accA = [&](float a) { return ImVec4(acc.x, acc.y, acc.z, a); };
    c[ImGuiCol_Text] = ImVec4(1, 1, 1, 0.96f);
    c[ImGuiCol_TextDisabled] = white(0.45f);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.08f, 0.25f, 0.96f);
    c[ImGuiCol_Border] = white(Tok::GlassBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = white(0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = white(0.28f);
    c[ImGuiCol_ScrollbarGrabActive] = accA(0.8f);
    c[ImGuiCol_CheckMark] = acc;
    c[ImGuiCol_Button] = white(0.08f);
    c[ImGuiCol_ButtonHovered] = white(0.14f);
    c[ImGuiCol_ButtonActive] = accA(0.5f);
    c[ImGuiCol_Header] = accA(0.30f);
    c[ImGuiCol_HeaderHovered] = white(0.10f);
    c[ImGuiCol_HeaderActive] = accA(0.45f);
    c[ImGuiCol_Separator] = white(0.12f);
    c[ImGuiCol_InputTextCursor] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextSelectedBg] = accA(0.45f);
    c[ImGuiCol_NavCursor] = acc;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.03f, 0.01f, 0.08f, 0.55f);
    c[ImGuiCol_TextLink] = ImVec4(0.85f, 0.75f, 1.0f, 1.0f);

    const float scale = p.dpiScale * p.uiScale;
    s.ScaleAllSizes(scale);
    s.FontSizeBase = Type::Body;
    s.FontScaleMain = p.uiScale;
    s.FontScaleDpi = p.dpiScale;
    ImGui::GetStyle() = s;
}

// ---- drawing -----------------------------------------------------------------------------

static void Recolor(ImDrawList* dl, int v0, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, bool horizontal)
{
    const float span = horizontal ? (b.x - a.x) : (b.y - a.y);
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = span > 0.0f ? Clamp(((horizontal ? v.pos.x - a.x : v.pos.y - a.y)) / span, 0.0f, 1.0f) : 0.0f;
        const ImU32 c = Mix(c0, c1, t);
        const ImU32 fringe = (v.col >> IM_COL32_A_SHIFT) & 0xFF;   // keep anti-aliased edges
        const ImU32 ca = (c >> IM_COL32_A_SHIFT) & 0xFF;
        v.col = (c & ~IM_COL32_A_MASK) | ((ca * fringe / 255) << IM_COL32_A_SHIFT);
    }
}

void GradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float rounding, ImDrawFlags flags)
{
    if (b.x <= a.x || b.y <= a.y) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    Recolor(dl, v0, a, b, top, bottom, false);
}

void GradientRectH(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right, float rounding, ImDrawFlags flags)
{
    if (b.x <= a.x || b.y <= a.y) return;
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    Recolor(dl, v0, a, b, left, right, true);
}

void RadialGlow(ImDrawList* dl, ImVec2 c, float radius, ImU32 inner, int segments)
{
    if (radius <= 1.0f || ((inner >> IM_COL32_A_SHIFT) & 0xFF) == 0) return;
    const ImU32 outer = inner & ~IM_COL32_A_MASK;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(segments * 3, segments + 1);
    const unsigned base = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(c, uv, inner);
    for (int i = 0; i < segments; ++i) {
        const float ang = (float)i / segments * 6.2831853f;
        dl->PrimWriteVtx(ImVec2(c.x + std::cos(ang) * radius, c.y + std::sin(ang) * radius), uv, outer);
    }
    for (int i = 0; i < segments; ++i) {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segments));
    }
}

namespace {
GlassSource g_glass;
}
void SetGlassSource(const GlassSource& s) { g_glass = s; }
const GlassSource& GetGlassSource() { return g_glass; }

void Shadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float strength)
{
    if (strength <= 0.01f) return;
    strength = std::min(strength, 2.2f);
    // Large, soft and faint: 8 nested rounded rects whose alphas add up to a smooth falloff.
    const float spread = S(14.0f + 12.0f * strength), drop = S(2.0f + 6.0f * (strength - 0.6f));
    const float total = 255.0f * Tok::ShadowAlpha * (0.45f + 0.30f * strength);
    const int n = 8;
    for (int i = n; i >= 1; --i) {
        const float t = (float)i / n;
        const float e = spread * t;
        const float alpha = total * (1.0f - t) * (1.0f - t) * 2.2f / n;
        dl->AddRectFilled(ImVec2(a.x - e * 0.6f, a.y - e * 0.35f + drop), ImVec2(b.x + e * 0.6f, b.y + e * 0.9f + drop),
                          IM_COL32(6, 0, 18, (int)std::min(255.0f, alpha)), rounding + e);
    }
}

void FadeVertsDiagonal(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, float a0, float a1, ImU32 tint, float tintAmount)
{
    const float w = std::max(1.0f, b.x - a.x), h = std::max(1.0f, b.y - a.y);
    const ImVec4 tc = ImGui::ColorConvertU32ToFloat4(tint);
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float t = std::clamp(((v.pos.x - a.x) / w + (v.pos.y - a.y) / h) * 0.5f, 0.0f, 1.0f);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(v.col);
        if (tintAmount > 0.0f) {
            c.x += (tc.x - c.x) * tintAmount;
            c.y += (tc.y - c.y) * tintAmount;
            c.z += (tc.z - c.z) * tintAmount;
        }
        c.w *= a0 + (a1 - a0) * t;
        v.col = ImGui::ColorConvertFloat4ToU32(c);
    }
}

void TiltVerts(ImDrawList* dl, int start, ImVec2 a, ImVec2 b, float tx, float ty, float scale, int end)
{
    if (end < 0 || end > dl->VtxBuffer.Size) end = dl->VtxBuffer.Size;
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float hw = std::max(1.0f, (b.x - a.x) * 0.5f), hh = std::max(1.0f, (b.y - a.y) * 0.5f);
    // A small perspective: the edge turned towards the viewer grows, the far edge shrinks.
    const float k = 0.9f;
    for (int i = start; i < end; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        const float nx = (v.pos.x - c.x) / hw, ny = (v.pos.y - c.y) / hh;
        const float z = 1.0f + k * (nx * std::sin(tx) + ny * std::sin(ty));
        v.pos.x = c.x + (v.pos.x - c.x) * scale * z;
        v.pos.y = c.y + (v.pos.y - c.y) * scale * z;
    }
}

void ScaleVerts(ImDrawList* dl, int start, ImVec2 o, float scale)
{
    for (int i = start; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer[i];
        v.pos.x = o.x + (v.pos.x - o.x) * scale;
        v.pos.y = o.y + (v.pos.y - o.y) * scale;
    }
}

void Glass(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float hover, float borderAlpha, ImDrawFlags flags, int layer)
{
    if (b.x <= a.x || b.y <= a.y) return;
    layer = std::clamp(layer, 1, 3);
    const GlassSource& g = g_glass;
    const ImTextureID tex = layer == 3 && g.strong ? g.strong : g.blurred;
    if (tex) {
        // The frosted view of what's behind: this rect of the blurred background.
        const ImVec2 uv0((a.x - g.origin.x) / g.size.x, (a.y - g.origin.y) / g.size.y);
        const ImVec2 uv1((b.x - g.origin.x) / g.size.x, (b.y - g.origin.y) / g.size.y);
        dl->AddImageRounded(tex, a, b, uv0, uv1, IM_COL32_WHITE, rounding, flags);
    }
    // Tint: dark for contrast (more on higher layers), then a white wash.
    static const int kDark[4] = { 0, 46, 62, 150 };
    static const float kWhite[4] = { 0, 0.070f, 0.100f, 0.120f };
    dl->AddRectFilled(a, b, IM_COL32(14, 6, 34, kDark[layer] - (int)(14 * hover)), rounding, flags);
    const float wf = kWhite[layer] + 0.045f * hover;
    GradientRect(dl, a, b, White(wf * 1.15f), White(wf * 0.6f), rounding, flags);
    // Inner top highlight.
    const float h = b.y - a.y;
    const ImDrawFlags top = (flags & ImDrawFlags_RoundCornersMask_) ? (flags & ImDrawFlags_RoundCornersTop) : ImDrawFlags_RoundCornersTop;
    GradientRect(dl, a, ImVec2(b.x, a.y + std::min(h * 0.35f, S(48.0f))), White(0.07f + 0.04f * hover), White(0.0f), rounding,
                 top ? top : ImDrawFlags_RoundCornersNone);
    // Gradient border: light catches the top-left edge; on hover it picks up the accent.
    const int vs = dl->VtxBuffer.Size;
    dl->AddRect(a, b, IM_COL32_WHITE, rounding, flags, 1.0f);
    const float k = std::min(1.6f, borderAlpha / Tok::GlassBorderSoft);
    FadeVertsDiagonal(dl, vs, a, b, std::min(1.0f, (0.30f + 0.25f * hover) * k), (0.05f + 0.10f * hover) * k, Mix(Accent(), Accent2(), 0.5f),
                      0.65f * hover);
    // Rim light along the top edge, brightest in the middle.
    const float inset = std::max(rounding, S(6.0f));
    if (b.x - a.x > inset * 3.0f) {
        const float mid = (a.x + b.x) * 0.5f, y = a.y + 0.5f;
        const float r = std::min(1.0f, 0.5f * k) + 0.25f * hover;
        GradientRectH(dl, ImVec2(a.x + inset, y), ImVec2(mid, y + 1.0f), White(0.0f), White(0.35f * r), 0.0f);
        GradientRectH(dl, ImVec2(mid, y), ImVec2(b.x - inset, y + 1.0f), White(0.35f * r), White(0.0f), 0.0f);
    }
}

void AccentFill(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float hover, ImDrawFlags flags)
{
    const ImU32 l = Mix(Accent(), White(1.0f), 0.10f * hover), r = Mix(Accent2(), White(1.0f), 0.10f * hover);
    GradientRectH(dl, a, b, l, r, rounding, flags);
    GradientRect(dl, a, ImVec2(b.x, a.y + (b.y - a.y) * 0.5f), White(0.16f + 0.06f * hover), White(0.0f), rounding,
                 (flags & ImDrawFlags_RoundCornersMask_) ? (flags & ImDrawFlags_RoundCornersTop) : ImDrawFlags_RoundCornersTop);
    dl->AddRect(a, b, White(0.20f), rounding, flags, 1.0f);
}

void DrawBackdrop(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float time)
{
    const ThemeParams& p = g_params;
    if (g_glass.background) {
        // The GPU gradient mesh (it already carries the Mica/Acrylic see-through alpha).
        dl->AddImage(g_glass.background, mn, mx, ImVec2((mn.x - g_glass.origin.x) / g_glass.size.x, (mn.y - g_glass.origin.y) / g_glass.size.y),
                     ImVec2((mx.x - g_glass.origin.x) / g_glass.size.x, (mx.y - g_glass.origin.y) / g_glass.size.y));
        return;
    }
    const float w = mx.x - mn.x, h = mx.y - mn.y, R = std::max(w, h);
    const float base = p.backdrop ? 0.55f : 1.0f;   // let Windows' blur show through when it's on
    auto A = [&](float a) { return (int)(255 * a * base); };
    // Deep indigo-violet base, darker towards the bottom right.
    dl->AddRectFilledMultiColor(mn, mx, IM_COL32(30, 14, 66, A(1)), IM_COL32(22, 10, 52, A(1)), IM_COL32(12, 6, 30, A(1)),
                                IM_COL32(20, 10, 48, A(1)));
    const float t = p.animations ? time : 0.0f;
    auto at = [&](float fx, float fy, float ax, float ay, float speed, float phase) {
        return ImVec2(mn.x + w * (fx + ax * std::sin(t * speed + phase)), mn.y + h * (fy + ay * std::cos(t * speed * 0.8f + phase)));
    };
    const float k = p.backdrop ? 0.7f : 1.0f;
    // Soft light pools: violet top-left, magenta through the middle, blue lower-left, pink accents.
    RadialGlow(dl, at(0.18f, 0.12f, 0.05f, 0.04f, 0.050f, 0.0f), R * 0.55f, IM_COL32(118, 52, 238, (int)(150 * k)), 64);
    RadialGlow(dl, at(0.48f, 0.46f, 0.06f, 0.05f, 0.040f, 1.3f), R * 0.42f, IM_COL32(214, 38, 132, (int)(135 * k)), 64);
    RadialGlow(dl, at(0.62f, 0.30f, 0.05f, 0.06f, 0.035f, 2.6f), R * 0.30f, IM_COL32(236, 60, 150, (int)(90 * k)), 64);
    RadialGlow(dl, at(0.10f, 0.78f, 0.04f, 0.05f, 0.045f, 3.9f), R * 0.45f, IM_COL32(60, 54, 200, (int)(120 * k)), 64);
    RadialGlow(dl, at(0.90f, 0.86f, 0.04f, 0.03f, 0.030f, 5.1f), R * 0.50f, IM_COL32(90, 40, 170, (int)(90 * k)), 64);
    // A dark diagonal band, like the shadow sweeping across the reference.
    const ImVec2 d0(mn.x + w * 0.70f, mn.y), d1(mn.x + w * 1.05f, mn.y), d2(mn.x + w * 0.55f, mx.y), d3(mn.x + w * 0.20f, mx.y);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 clear = IM_COL32(8, 4, 22, 0), dark = IM_COL32(8, 4, 22, (int)(150 * k));
    dl->PrimReserve(6, 4);
    const unsigned i0 = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(d0, uv, clear);
    dl->PrimWriteVtx(d1, uv, dark);
    dl->PrimWriteVtx(d2, uv, dark);
    dl->PrimWriteVtx(d3, uv, clear);
    dl->PrimWriteIdx((ImDrawIdx)i0);
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 1));
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
    dl->PrimWriteIdx((ImDrawIdx)i0);
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
    dl->PrimWriteIdx((ImDrawIdx)(i0 + 3));
}

void Icon(ImDrawList* dl, const char* icon, ImVec2 center, float size, ImU32 col)
{
    ImFont* f = g_fonts.icons ? g_fonts.icons : ImGui::GetFont();
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
    dl->AddText(f, size, ImVec2(std::floor(center.x - ts.x * 0.5f), std::floor(center.y - ts.y * 0.5f)), col, icon);
}

ImVec2 IconSize(const char* icon, float size)
{
    ImFont* f = g_fonts.icons ? g_fonts.icons : ImGui::GetFont();
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
}

} // namespace cs::ui
