#include "ui/Widgets.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "core/Util.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cs::ui {

// ---------------------------------------------------------------------------------------------
// Shared interaction effects
// ---------------------------------------------------------------------------------------------

namespace {

std::string VisibleLabel(const char* label)
{
    const char* hash = strstr(label, "##");
    return hash ? std::string(label, hash) : std::string(label);
}

float ResolveWidth(float w)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    if (w < 0.0f) return std::max(1.0f, avail + w + 1.0f);
    if (w == 0.0f) return std::max(1.0f, avail);
    return w;
}

struct Fx {
    float hover = 0.0f, press = 0.0f, focus = 0.0f;
    ImVec2 a, b;   // rect after the press transform
};

bool NavFocused() { return ImGui::IsItemFocused() && GImGui->NavCursorVisible; }

void (*g_uiSound)(int) = nullptr;

// Hover/press/focus state plus the spring that moves the control: hover lifts it 2 px and scales it
// to 1.03 with a little overshoot, pressing pushes it down to 0.96, releasing springs back past 1 and
// settles, and starts a ripple of light at the click point.
Fx Interact(ImGuiID id, ImVec2 a, ImVec2 b, bool hovered, bool held, bool released, bool enabled, float lift = 1.0f)
{
    Fx fx;
    hovered = hovered && enabled;
    held = held && enabled;
    fx.hover = AnimHover(AnimId(id, 1), hovered);
    fx.press = Anim(AnimId(id, 2), held ? 1.0f : 0.0f, Motion::Press);
    fx.focus = Anim(AnimId(id, 3), enabled && NavFocused() ? 1.0f : 0.0f, Motion::Hover);
    const ImGuiID wasHover = AnimId(id, 10);
    const bool before = Anim(wasHover, 0.0f, 0.0f) > 0.5f;
    if (hovered && !before && g_uiSound) g_uiSound(0);
    AnimSet(wasHover, hovered ? 1.0f : 0.0f);
    if (released && enabled) {
        TimerStart(AnimId(id, 4));
        const ImVec2 m = ImGui::GetIO().MousePos;
        AnimSet(AnimId(id, 5), m.x - a.x);
        AnimSet(AnimId(id, 6), m.y - a.y);
        if (g_uiSound) g_uiSound(1);
    }
    float scale = 1.0f, up = 0.0f;
    if (Animations()) {
        const float w = b.x - a.x;
        // Big controls scale less, so a wide button doesn't grow by a lot of pixels.
        const float grow = lift <= 0.0f ? 0.0f : std::clamp(S(5.0f) / std::max(w, 1.0f), 0.012f, 0.035f);
        const float target = held ? 0.96f : (hovered ? 1.0f + grow : 1.0f);
        scale = Spring(AnimId(id, 11), target, held ? Springs::Press : Springs::Hover);
        up = -S(2.0f) * lift * Spring(AnimId(id, 12), hovered && !held ? 1.0f : 0.0f, Springs::Hover);
    }
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + up);
    const ImVec2 h((b.x - a.x) * 0.5f * scale, (b.y - a.y) * 0.5f * scale);
    fx.a = ImVec2(c.x - h.x, c.y - h.y);
    fx.b = ImVec2(c.x + h.x, c.y + h.y);
    return fx;
}

// Soft light on the glass that follows the cursor.
void Spotlight(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col, float amount)
{
    if (amount < 0.01f || !ImGui::IsMousePosValid()) return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const ImVec2 c(std::clamp(m.x, a.x, b.x), std::clamp(m.y, a.y, b.y));
    const float rad = std::max(b.x - a.x, b.y - a.y) * 0.75f + S(20.0f);
    const float in = r * 0.29f;   // keep the light inside the rounded corners
    dl->PushClipRect(ImVec2(a.x + in * 0.5f, a.y + 1.0f), ImVec2(b.x - in * 0.5f, b.y - 1.0f), true);
    RadialGlow(dl, c, rad, ColA(col, amount), 40);
    dl->PopClipRect();
}

void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, ImU32 col, float amount)
{
    if (amount < 0.01f) return;
    for (int i = 1; i <= 6; ++i) {
        const float e = S(1.6f) * (float)i;
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), ColA(col, amount * 0.26f / (float)(i + 0.5f)), r + e, 0, S(1.8f));
    }
}

void Ripple(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, ImU32 col)
{
    const float t = TimerElapsed(AnimId(id, 4)) / Motion::RippleSeconds;
    if (t >= 1.0f || !Animations()) return;
    const ImVec2 c(a.x + Anim(AnimId(id, 5), 0.0f, 0.0f), a.y + Anim(AnimId(id, 6), 0.0f, 0.0f));
    const float maxR = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    dl->PushClipRect(a, b, true);
    dl->AddCircleFilled(c, std::max(1.0f, maxR * EaseOutCubic(t)), ColA(col, 0.28f * (1.0f - t) * (1.0f - t)), 40);
    dl->PopClipRect();
}

// Click wave: two rings that ripple outward from the control's edge and fade.
void RingWave(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, float r, ImU32 col)
{
    if (!Animations()) return;
    const float since = TimerElapsed(AnimId(id, 4));
    for (int k = 0; k < 2; ++k) {
        const float t = (since - 0.09f * k) / 0.55f;
        if (t < 0.0f || t >= 1.0f) continue;
        const float e = S(16.0f) * EaseOutCubic(t);
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), ColA(col, (k ? 0.30f : 0.55f) * (1.0f - t) * (1.0f - t)), r + e, 0,
                    S(2.0f) * (1.0f - t * 0.6f));
    }
}

// Hover sweep: when the pointer arrives, a soft band of light slides across once.
void HoverSweep(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, bool hovered, float repeatEvery = 0.0f)
{
    const ImGuiID was = AnimId(id, 8), timer = AnimId(id, 9);
    const bool before = Anim(was, 0.0f, 0.0f) > 0.5f;
    if (hovered && !before) TimerStart(timer);
    else if (hovered && repeatEvery > 0.0f && TimerElapsed(timer) > repeatEvery) TimerStart(timer);
    AnimSet(was, hovered ? 1.0f : 0.0f);
    const float t = TimerElapsed(timer) / 0.6f;
    if (t >= 1.0f || !Animations()) return;
    const float w = b.x - a.x, h = b.y - a.y;
    const float x = a.x - w * 0.35f + (w * 1.7f) * EaseOutCubic(t);
    const float band = std::max(S(18.0f), w * 0.22f);
    dl->PushClipRect(a, b, true);
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const ImU32 clear = White(0.0f), lit = White(0.20f * (1.0f - t * 0.5f));
    // A slanted band: left edge clear, centre lit, right edge clear.
    const float skew = h * 0.35f;
    const ImVec2 p0(x - band, b.y), p1(x - band + skew, a.y), p2(x + skew, a.y), p3(x, b.y), p4(x + band + skew, a.y), p5(x + band, b.y);
    dl->PrimReserve(12, 6);
    const unsigned i0 = dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(p0, uv, clear);
    dl->PrimWriteVtx(p1, uv, clear);
    dl->PrimWriteVtx(p2, uv, lit);
    dl->PrimWriteVtx(p3, uv, lit);
    dl->PrimWriteVtx(p4, uv, clear);
    dl->PrimWriteVtx(p5, uv, clear);
    const unsigned idx[] = { 0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5 };
    for (unsigned k : idx) dl->PrimWriteIdx((ImDrawIdx)(i0 + k));
    dl->PopClipRect();
}

void FocusRing(ImDrawList* dl, ImVec2 a, ImVec2 b, float r, float amount)
{
    if (amount < 0.01f) return;
    const float e = S(3.0f);
    dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), Accent(0.95f * amount), r + e, 0, S(2.0f));
}

void FadeSince(ImDrawList* dl, int v0, float alpha)
{
    for (int i = v0; i < dl->VtxBuffer.Size; ++i) {
        ImU32& c = dl->VtxBuffer[i].col;
        const int r = (c >> IM_COL32_R_SHIFT) & 0xFF, g = (c >> IM_COL32_G_SHIFT) & 0xFF, bl = (c >> IM_COL32_B_SHIFT) & 0xFF;
        const int lum = (r * 54 + g * 183 + bl * 19) >> 8;
        const int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        c = IM_COL32((r + lum) / 2, (g + lum) / 2, (bl + lum) / 2, a);
    }
}

// Popup window styling shared by dropdowns, menus and modals: frosted surface + fade-in.
// Third glass layer: more frosted, darker, with the biggest shadow.
void PopupSurface(float t, float rounding = 0.0f)
{
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = w->DrawList;
    const ImVec2 a = w->Pos, b(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    const float r = rounding > 0.0f ? rounding : S(Tok::RadiusCard);
    dl->PushClipRectFullScreen();
    Shadow(dl, a, b, r, 1.8f * t);
    dl->PopClipRect();
    Glass(dl, a, b, r, 0.0f, Tok::GlassBorder, 0, 3);
}

// Popups scale and fade in from where they were opened: their vertices are scaled at End.
struct PopupAnim {
    ImVec2 origin;
    float t;
};
std::vector<PopupAnim> g_popupAnims;

void PushPopupAnim(ImVec2 origin, float t) { g_popupAnims.push_back({ origin, t }); }
void PopPopupAnim()
{
    if (g_popupAnims.empty()) return;
    const PopupAnim pa = g_popupAnims.back();
    g_popupAnims.pop_back();
    if (pa.t >= 0.999f || !Animations()) return;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ScaleVerts(w->DrawList, 0, pa.origin, 0.90f + 0.10f * pa.t);
}

float PopupFade(ImGuiID id, bool open)
{
    if (!open) {
        TimerStop(id);
        return 0.0f;
    }
    if (TimerElapsed(id) > 1e5f) TimerStart(id);
    return Animations() ? EaseOutCubic(TimerElapsed(id) / Motion::PopupSeconds) : 1.0f;
}

void PushPopupStyle()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(6.0f), S(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(Tok::RadiusCard));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, S(2.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
}

void PopPopupStyle()
{
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

} // namespace

void SetUiSoundHook(void (*hook)(int)) { g_uiSound = hook; }

void WaveFx(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, float rounding, bool hovered, bool clicked, ImU32 color)
{
    if (clicked) TimerStart(AnimId(id, 4));
    HoverSweep(dl, id, a, b, hovered);
    RingWave(dl, id, a, b, rounding, color);
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

std::string Ellipsize(const char* text, float maxWidth)
{
    if (ImGui::CalcTextSize(text).x <= maxWidth) return text;
    static const char* ell = "\xE2\x80\xA6";
    const float ellW = ImGui::CalcTextSize(ell).x;
    const std::string s = text;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && mid < s.size() && ((unsigned char)s[mid] & 0xC0) == 0x80) --mid;
        if (mid <= lo) {
            hi = lo;
            break;
        }
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + mid).x + ellW <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && lo < s.size() && ((unsigned char)s[lo] & 0xC0) == 0x80) --lo;
    return s.substr(0, lo) + ell;
}

void Text(const char* text, ImU32 col, ImFont* font, float size)
{
    if (font) PushFont(font, size > 0 ? size : Type::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    if (font) PopFont();
}

void TextWrapped(const char* text, ImU32 col, float wrapWidth, ImFont* font, float size)
{
    if (font) PushFont(font, size > 0 ? size : Type::Body);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (font) PopFont();
}

void TextEllipsis(const char* text, float maxWidth, ImU32 col, ImFont* font, float size)
{
    if (font) PushFont(font, size > 0 ? size : Type::Body);
    const std::string shown = Ellipsize(text, maxWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    if (font) PopFont();
    if (shown != text) Tooltip(text);
}

void Gap(float px) { ImGui::Dummy(ImVec2(1.0f, S(px))); }

// ---------------------------------------------------------------------------------------------
// Tooltip
// ---------------------------------------------------------------------------------------------

void Tooltip(const char* text)
{
    if (!text || !*text) return;
    const ImGuiID id = AnimId(ImGui::GetItemID() ? ImGui::GetItemID() : ImGui::GetID(text), 77);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (!hovered) {
        TimerStop(id);
        return;
    }
    if (TimerElapsed(id) > 1e5f) TimerStart(id);
    const float t = TimerElapsed(id) - Motion::TooltipDelay;
    if (t < 0.0f) return;
    const float a = Animations() ? Clamp(t / Motion::TooltipFade, 0.0f, 1.0f) : 1.0f;
    const ImVec2 origin((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, ImGui::GetItemRectMax().y);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.0f), S(7.0f)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    if (ImGui::BeginTooltip()) {
        PopupSurface(a);
        PushPopupAnim(origin, EaseOutCubic(a));
        PushFont(GetFonts().regular, Type::Caption);
        ImGui::PushTextWrapPos(S(320.0f));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        PopFont();
        PopPopupAnim();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------------------------

static void DrawButtonSurface(ImDrawList* dl, ImGuiID id, const Fx& fx, Btn style, float r, bool enabled, ImU32& textCol)
{
    const ImVec2 a = fx.a, b = fx.b;
    const float h = enabled ? fx.hover : 0.0f, pr = fx.press;
    // Pressing tightens the glow and darkens the glass a little.
    const float glow = h * (1.0f - 0.55f * pr);
    ImU32 rippleCol = White(1.0f);
    switch (style) {
    case Btn::Primary:
        if (enabled) {
            Shadow(dl, a, b, r, 0.6f + 0.8f * h - 0.5f * pr);
            Glow(dl, a, b, r, Accent2(), glow * 1.1f);
        }
        AccentFill(dl, a, b, r, h - 0.6f * pr);
        if (enabled) Spotlight(dl, a, b, r, White(1.0f), 0.22f * h);
        textCol = White(1.0f);
        break;
    case Btn::Danger:
        if (enabled) Glow(dl, a, b, r, Pal().danger, glow);
        GradientRect(dl, a, b, Mix(Pal().danger, White(1.0f), 0.12f * h), Mix(Pal().danger, IM_COL32(120, 0, 30, 255), 0.25f), r);
        if (enabled) Spotlight(dl, a, b, r, White(1.0f), 0.18f * h);
        dl->AddRect(a, b, White(0.22f), r);
        textCol = White(1.0f);
        break;
    case Btn::White:
        if (enabled) Shadow(dl, a, b, r, 0.5f + 0.6f * h);
        dl->AddRectFilled(a, b, Mix(IM_COL32(255, 255, 255, 255), IM_COL32(236, 228, 255, 255), h), r);
        if (enabled) {
            Spotlight(dl, a, b, r, Accent(), 0.16f * h);
            Glow(dl, a, b, r, White(1.0f), 0.6f * glow);
        }
        textCol = IM_COL32(26, 12, 48, 255);
        rippleCol = Accent();
        break;
    case Btn::Secondary:
        if (enabled) Shadow(dl, a, b, r, 0.3f + 0.7f * h);
        Glass(dl, a, b, r, h, Tok::GlassBorderSoft + 0.06f, 0, 2);
        if (enabled) {
            Spotlight(dl, a, b, r, White(1.0f), 0.13f * h);
            Glow(dl, a, b, r, Accent(), 0.6f * glow);
        }
        textCol = Mix(Pal().textDim, Pal().text, 0.4f + 0.6f * h);
        rippleCol = Accent();
        break;
    case Btn::Ghost:
        if (h > 0.01f) {
            dl->AddRectFilled(a, b, White(0.08f * h), r);
            Spotlight(dl, a, b, r, White(1.0f), 0.10f * h);
        }
        textCol = Mix(Pal().textDim, Pal().text, h);
        rippleCol = Accent();
        break;
    }
    if (pr > 0.01f) dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(50 * pr)), r);
    // Shine: once on arrival; the primary button repeats it every few seconds while hovered.
    if (enabled) HoverSweep(dl, id, a, b, fx.hover > 0.5f, style == Btn::Primary ? 2.8f : 0.0f);
    Ripple(dl, id, a, b, rippleCol);
    RingWave(dl, id, a, b, r, style == Btn::Danger ? Pal().danger : Accent2());
    FocusRing(dl, a, b, r, fx.focus);
}

// Icons react too: arrows nudge 2 px in their direction, everything else bounces once on arrival.
static ImVec2 IconNudge(ImGuiID id, const char* icon, float hover)
{
    if (!icon || !Animations()) return ImVec2(0, 0);
    if (!strcmp(icon, ICON_CHEVRON_RIGHT)) return ImVec2(S(2.0f) * hover, 0);
    if (!strcmp(icon, ICON_CHEVRON_LEFT)) return ImVec2(-S(2.0f) * hover, 0);
    const float t = TimerElapsed(AnimId(id, 9));
    if (t > 0.45f) return ImVec2(0, 0);
    return ImVec2(0, -S(2.5f) * std::sin(t / 0.45f * 3.14159f) * std::exp(-t * 3.0f));
}

bool Button(const char* label, const ButtonOpts& o)
{
    const ImGuiID id = ImGui::GetID(label);
    const std::string text = VisibleLabel(label);
    const bool iconOnly = text.empty() && o.icon;
    PushFont(GetFonts().medium, Type::Body);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const float iconW = o.icon ? S(16.0f) + (text.empty() ? 0.0f : S(8.0f)) : 0.0f;
    ImVec2 size = o.size;
    if (size.y <= 0.0f) size.y = S(Tok::ControlHeight);
    if (size.x == 0.0f) size.x = iconOnly ? size.y : ts.x + iconW + S(32.0f);
    else size.x = ResolveWidth(size.x);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!o.enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const Fx fx = Interact(id, p, ImVec2(p.x + size.x, p.y + size.y), hovered, held, pressed, o.enabled);
    if (!o.enabled) ImGui::EndDisabled();
    if (hovered && o.enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (o.tooltip) Tooltip(o.tooltip);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int v0 = dl->VtxBuffer.Size;
    const float r = o.round ? (fx.b.y - fx.a.y) * 0.5f : std::min((fx.b.y - fx.a.y) * 0.5f, S(Tok::RadiusControl));
    ImU32 col = Pal().text;
    if (o.active && o.style == Btn::Ghost) {
        dl->AddRectFilled(fx.a, fx.b, White(0.14f + 0.06f * fx.hover), r);
        dl->AddRect(fx.a, fx.b, White(0.18f), r);
        HoverSweep(dl, id, fx.a, fx.b, fx.hover > 0.5f);
        Ripple(dl, id, fx.a, fx.b, Accent());
        RingWave(dl, id, fx.a, fx.b, r, Accent2());
        FocusRing(dl, fx.a, fx.b, r, fx.focus);
    } else {
        DrawButtonSurface(dl, id, fx, o.style, r, o.enabled, col);
    }
    const float cy = (fx.a.y + fx.b.y) * 0.5f;
    if (iconOnly) {
        const ImVec2 n = IconNudge(id, o.icon, fx.hover);
        Icon(dl, o.icon, ImVec2((fx.a.x + fx.b.x) * 0.5f + n.x, cy + n.y), std::min(S(16.0f), size.y * 0.46f), col);
    } else {
        const float total = iconW + ts.x;
        float x = (fx.a.x + fx.b.x) * 0.5f - total * 0.5f;
        if (o.icon) {
            const ImVec2 n = IconNudge(id, o.icon, fx.hover);
            Icon(dl, o.icon, ImVec2(x + S(8.0f) + n.x, cy + n.y), S(15.0f), col);
            x += iconW;
        }
        if (!text.empty()) dl->AddText(ImVec2(std::floor(x), std::floor(cy - ts.y * 0.5f)), col, text.c_str());
    }
    if (!o.enabled) FadeSince(dl, v0, Tok::DisabledAlpha);
    PopFont();
    return pressed && o.enabled;
}

// ---------------------------------------------------------------------------------------------
// Toggle, checkbox, sliders
// ---------------------------------------------------------------------------------------------

bool Toggle(const char* id, bool* v, bool enabled)
{
    const float h = S(22.0f), w = S(40.0f);
    const float frameH = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, frameH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const ImGuiID gid = ImGui::GetItemID();
    const ImVec2 a(p.x, p.y + (frameH - h) * 0.5f), b(a.x + w, a.y + h);
    const Fx fx = Interact(gid, a, b, hovered, held, false, enabled, 0.0f);
    if (!enabled) ImGui::EndDisabled();
    if (pressed && enabled) *v = !*v;
    if (hovered && enabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // The knob rides a spring (a little overshoot at the end) and stretches while it's moving fast.
    const float raw = Spring(gid, *v ? 1.0f : 0.0f, Springs::Knob);
    const float t = Clamp(raw, 0.0f, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int v0 = dl->VtxBuffer.Size;
    const float r = h * 0.5f;
    dl->AddRectFilled(a, b, White(0.12f + 0.06f * fx.hover), r);
    if (t > 0.01f) {
        GradientRectH(dl, a, b, Accent(t), Accent2(t), r);
        GradientRect(dl, a, ImVec2(b.x, a.y + h * 0.5f), White(0.18f * t), White(0.0f), r, ImDrawFlags_RoundCornersTop);
        Glow(dl, a, b, r, Accent2(), 0.45f * t);   // the track glows when on
    }
    dl->AddRect(a, b, White(0.20f + 0.12f * fx.hover), r);
    Glow(dl, a, b, r, Accent(), 0.5f * fx.hover * (0.3f + 0.7f * t));
    const float kr = r - S(3.0f);
    const float speed = std::fabs(SpringVelocity(gid));
    const float stretch = std::min(S(7.0f), S(1.1f) * speed) + S(4.0f) * fx.press;
    const float travel = w - 2.0f * r;
    const float kx = a.x + r + travel * raw;
    // Stretch towards where it's heading, so the knob looks like it's being pulled.
    const bool right = SpringVelocity(gid) >= 0.0f;
    const ImVec2 ka(kx - kr - (right ? stretch : 0.0f), a.y + r - kr * (1.0f - 0.06f * std::min(1.0f, speed * 0.2f)));
    const ImVec2 kb(kx + kr + (right ? 0.0f : stretch), a.y + r + kr * (1.0f - 0.06f * std::min(1.0f, speed * 0.2f)));
    dl->AddRectFilled(ImVec2(ka.x, ka.y + S(1.5f)), ImVec2(kb.x, kb.y + S(1.5f)), IM_COL32(10, 0, 30, 80), kr);
    dl->AddRectFilled(ka, kb, IM_COL32(255, 255, 255, 255), kr);
    GradientRect(dl, ka, ImVec2(kb.x, (ka.y + kb.y) * 0.5f), White(0.0f), IM_COL32(230, 220, 255, 60), kr, ImDrawFlags_RoundCornersTop);
    if (pressed && enabled) TimerStart(AnimId(gid, 4));
    RingWave(dl, gid, a, b, r, Accent2());
    FocusRing(dl, a, b, r, fx.focus);
    if (!enabled) FadeSince(dl, v0, Tok::DisabledAlpha);
    return pressed && enabled;
}

bool Slider(const char* id, float* v, float min, float max, const char* valueText, float width, float step)
{
    width = ResolveWidth(width);
    const float height = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    const ImGuiID gid = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float knobR = S(7.0f);
    // Wide sliders show their value on the right; narrow ones only in the drag bubble.
    float labelW = 0.0f;
    if (valueText && *valueText && width >= S(180.0f)) {
        PushFont(GetFonts().medium, Type::Body);
        labelW = std::max(ImGui::CalcTextSize(valueText).x, S(52.0f)) + S(14.0f);
        PopFont();
    }
    const float x0 = p.x + knobR, x1 = p.x + width - knobR - labelW;
    bool changed = false;
    if (active && x1 > x0) {
        float t = Clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        float nv = min + t * (max - min);
        if (step > 0.0f) nv = min + std::round((nv - min) / step) * step;
        nv = Clamp(nv, min, max);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    // Arrow keys when focused.
    if (ImGui::IsItemFocused() && !active) {
        const float k = step > 0 ? step : (max - min) / 50.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { *v = Clamp(*v - k, min, max); changed = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { *v = Clamp(*v + k, min, max); changed = true; }
    }
    const float t = max > min ? Clamp((*v - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    const float hov = Anim(AnimId(gid, 1), hovered || active ? 1.0f : 0.0f, Motion::Hover);
    const float grab = Anim(AnimId(gid, 2), active ? 1.0f : 0.0f, Motion::Press);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = p.y + height * 0.5f, th = S(4.0f);
    const ImVec2 ta(x0 - S(2.0f), cy - th * 0.5f), tb(x1 + S(2.0f), cy + th * 0.5f);
    dl->AddRectFilled(ta, tb, White(0.16f), th * 0.5f);
    const float kx = x0 + (x1 - x0) * t;
    if (kx > ta.x + 1.0f) {
        if (hov > 0.01f)
            for (int i = 1; i <= 3; ++i) {
                const float e = S(1.5f) * i;
                dl->AddRectFilled(ImVec2(ta.x - e, ta.y - e), ImVec2(kx + e, tb.y + e), Accent2(0.10f * hov / i), th * 0.5f + e);
            }
        GradientRectH(dl, ta, ImVec2(kx, tb.y), Accent(), Accent2(), th * 0.5f);
    }
    const float kr = knobR * Spring(AnimId(gid, 3), 1.0f + 0.25f * (hovered || active ? 1.0f : 0.0f) + 0.12f * (active ? 1.0f : 0.0f), Springs::Hover);
    (void)grab;
    if (hov > 0.01f) dl->AddCircleFilled(ImVec2(kx, cy), kr + S(5.0f) * hov, Accent2(0.20f * hov));
    dl->AddCircleFilled(ImVec2(kx, cy + S(1.0f)), kr, IM_COL32(10, 0, 30, 80));
    dl->AddCircleFilled(ImVec2(kx, cy), kr, White(1.0f));
    if (ImGui::IsItemFocused() && GImGui->NavCursorVisible) dl->AddCircle(ImVec2(kx, cy), kr + S(3.0f), Accent(), 0, S(2.0f));
    if (labelW > 0.0f) {
        PushFont(GetFonts().medium, Type::Body);
        const ImVec2 vs = ImGui::CalcTextSize(valueText);
        dl->AddText(ImVec2(p.x + width - vs.x, cy - vs.y * 0.5f), Mix(Pal().textDim, Pal().text, hov), valueText);
        PopFont();
    }
    const float bubble = Spring(AnimId(gid, 4), active && valueText && *valueText ? 1.0f : 0.0f, Springs::Hover);
    if (bubble > 0.02f && valueText && *valueText) {
        // Value bubble pops up above the knob while dragging.
        PushFont(GetFonts().medium, Type::Caption);
        const ImVec2 vs = ImGui::CalcTextSize(valueText);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 ba(kx - vs.x * 0.5f - S(8.0f), p.y - vs.y - S(10.0f)), bb(kx + vs.x * 0.5f + S(8.0f), p.y - S(2.0f));
        const int fv = fg->VtxBuffer.Size;
        Shadow(fg, ba, bb, S(8.0f), 1.4f);
        Glass(fg, ba, bb, S(8.0f), 0.0f, Tok::GlassBorder, 0, 3);
        fg->AddText(ImVec2(ba.x + S(8.0f), ba.y + (bb.y - ba.y - vs.y) * 0.5f), Pal().text, valueText);
        ScaleVerts(fg, fv, ImVec2(kx, bb.y + S(6.0f)), std::max(0.0f, bubble));
        if (bubble < 1.0f) FadeVertsDiagonal(fg, fv, ba, bb, std::min(1.0f, bubble), std::min(1.0f, bubble));
        PopFont();
    }
    return changed;
}

bool SliderInt(const char* id, int* v, int min, int max, const char* fmt, int step, float width)
{
    float f = (float)*v;
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *v);
    const bool changed = Slider(id, &f, (float)min, (float)max, buf, width, (float)std::max(1, step));
    if (changed) *v = (int)std::lround(f);
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Text field
// ---------------------------------------------------------------------------------------------

static void FieldFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover, float focus)
{
    const float r = S(Tok::RadiusControl);
    dl->AddRectFilled(a, b, IM_COL32(10, 2, 30, (int)(70 + 20 * focus)), r);
    dl->AddRectFilled(a, b, White(0.05f + 0.03f * hover), r);
    dl->AddRect(a, b, White(0.16f + 0.10f * hover), r, 0, 1.0f);
    if (focus > 0.005f) {
        // The accent border fills in from the left.
        const float x = a.x - S(8.0f) + (b.x - a.x + S(16.0f)) * EaseOutCubic(focus);
        dl->PushClipRect(ImVec2(a.x - S(12.0f), a.y - S(12.0f)), ImVec2(x, b.y + S(12.0f)), true);
        dl->AddRect(a, b, Accent(1.0f), r, 0, S(1.5f));
        Glow(dl, a, b, r, Accent(), 0.6f * focus);
        dl->PopClipRect();
    }
}

bool TextField(const char* id, char* buf, size_t size, const char* hint, float width, ImGuiInputTextFlags flags, const char* icon, float trailing)
{
    width = ResolveWidth(width);
    const float h = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImGuiID gid = ImGui::GetID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Last frame's hover/focus drive the frame drawn under this frame's text.
    const float hover = Anim(AnimId(gid, 1), 0.0f, 0.0f), focus = Anim(AnimId(gid, 2), 0.0f, 0.0f);
    const ImVec2 b(p.x + width, p.y + h);
    FieldFrame(dl, p, b, hover, focus);
    float textX = p.x + S(12.0f);
    if (icon) {
        Icon(dl, icon, ImVec2(p.x + S(20.0f), p.y + h * 0.5f), S(14.0f), Mix(Pal().textFaint, Pal().text, focus));
        textX = p.x + S(36.0f);
    }
    // Fields without an icon get a floating label: the placeholder rises onto the top edge when the
    // field is focused or has text. Fields with an icon (search, title) keep a plain placeholder.
    const bool floating = hint && *hint && !icon;
    const bool hasText = buf && buf[0];
    const float lift = floating ? Anim(AnimId(gid, 3), (focus > 0.5f || hasText) ? 1.0f : 0.0f, Motion::Slide) : 0.0f;
    if (floating) {
        const float big = Type::Body, small = Type::Caption * 0.92f;
        PushFont(GetFonts().medium, big + (small - big) * lift);
        const ImVec2 ls = ImGui::CalcTextSize(hint);
        const float y = (p.y + h * 0.5f - ls.y * 0.5f) + (p.y - ls.y * 0.5f - (p.y + h * 0.5f - ls.y * 0.5f)) * lift;
        const float x = textX - S(2.0f) * lift;
        if (lift > 0.02f) {
            // A chip of the panel colour so the border doesn't run through the label.
            const ImVec2 ca(x - S(4.0f), y + S(1.0f)), cb(x + ls.x + S(4.0f), y + ls.y - S(1.0f));
            dl->AddRectFilled(ca, cb, IM_COL32(30, 16, 58, (int)(235 * lift)), S(4.0f));
        }
        dl->AddText(ImVec2(x, y), Mix(Pal().textFaint, Mix(Pal().textDim, Accent2(1.0f), focus), lift), hint);
        PopFont();
    }
    const float fontH = ImGui::GetFontSize();
    ImGui::SetCursorScreenPos(ImVec2(textX, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, (h - fontH) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, Pal().textFaint);
    ImGui::SetNextItemWidth(b.x - textX - S(10.0f) - trailing);
    const bool changed = hint && !floating ? ImGui::InputTextWithHint(id, hint, buf, size, flags) : ImGui::InputText(id, buf, size, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    AnimHover(AnimId(gid, 1), hov);
    Anim(AnimId(gid, 2), act ? 1.0f : 0.0f, act ? 9.0f : 12.0f);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h));
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Segmented control
// ---------------------------------------------------------------------------------------------

bool Segmented(const char* id, int* current, const char* const* items, int count, float width)
{
    if (count <= 0) return false;
    ImGui::PushID(id);
    width = ResolveWidth(width);
    const float h = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(Tok::RadiusControl);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(10, 2, 30, 60), r);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), White(0.14f), r);
    const float segW = width / (float)count;
    const float sel = Spring(ImGui::GetID("##sel"), (float)Clamp(*current, 0, count - 1), Springs::Pill);
    const ImVec2 sa(p.x + sel * segW + S(3.0f), p.y + S(3.0f)), sb(sa.x + segW - S(6.0f), p.y + h - S(3.0f));
    AccentFill(dl, sa, sb, r - S(2.0f));
    bool changed = false;
    PushFont(GetFonts().medium, Type::Body);
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + segW * i, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h)) && *current != i) {
            *current = i;
            changed = true;
        }
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float hv = Anim(ImGui::GetItemID(), hov && *current != i ? 1.0f : 0.0f, Motion::Hover);
        ImGui::PopID();
        const ImVec2 a(p.x + segW * i, p.y), b(a.x + segW, p.y + h);
        if (hv > 0.01f) dl->AddRectFilled(ImVec2(a.x + S(3), a.y + S(3)), ImVec2(b.x - S(3), b.y - S(3)), White(0.08f * hv), r - S(2));
        const ImVec2 ts = ImGui::CalcTextSize(items[i]);
        const float near = 1.0f - Clamp(std::fabs(sel - (float)i), 0.0f, 1.0f);
        dl->AddText(ImVec2(std::floor(a.x + (segW - ts.x) * 0.5f), std::floor(a.y + (h - ts.y) * 0.5f)), Mix(Pal().textDim, White(1.0f), std::max(near, hv)),
                    items[i]);
    }
    PopFont();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Dropdowns
// ---------------------------------------------------------------------------------------------

namespace {
struct DropdownFrame {
    ImGuiID popupId = 0;
    float width = 0.0f;
};
DropdownFrame g_dropdown;
} // namespace

bool BeginDropdown(const char* id, const char* preview, float width, const char* icon)
{
    width = ResolveWidth(width);
    const float h = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    const ImGuiID gid = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const std::string popup = std::string("##dd") + id;
    const ImGuiID popupId = ImGui::GetID(popup.c_str());
    const bool open = ImGui::IsPopupOpen(popupId, 0);
    if (pressed && !open) ImGui::OpenPopup(popupId);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float hv = Anim(AnimId(gid, 1), hovered || open ? 1.0f : 0.0f, Motion::Hover);
    const ImVec2 b(p.x + width, p.y + h);
    FieldFrame(dl, p, b, hv, open ? 1.0f : Anim(AnimId(gid, 2), open ? 1.0f : 0.0f, Motion::Hover));
    float x = p.x + S(12.0f);
    if (icon) {
        Icon(dl, icon, ImVec2(x + S(7.0f), p.y + h * 0.5f), S(14.0f), Pal().textDim);
        x += S(24.0f);
    }
    const float chevronW = S(28.0f);
    const std::string shown = Ellipsize(preview, b.x - chevronW - x);
    dl->AddText(ImVec2(x, p.y + (h - ImGui::GetFontSize()) * 0.5f), Pal().text, shown.c_str());
    const float rot = Anim(AnimId(gid, 3), open ? 1.0f : 0.0f, Motion::Slide);
    // Chevron that flips when open.
    const ImVec2 c(b.x - S(16.0f), p.y + h * 0.5f);
    const float cw = S(4.5f), ch = S(2.5f) * (1.0f - 2.0f * rot);
    dl->AddLine(ImVec2(c.x - cw, c.y - ch), c + ImVec2(0, ch), Pal().textDim, S(1.5f));
    dl->AddLine(c + ImVec2(0, ch), ImVec2(c.x + cw, c.y - ch), Pal().textDim, S(1.5f));

    const float t = PopupFade(AnimId(popupId, 9), open);
    if (!open) return false;
    ImGui::SetNextWindowPos(ImVec2(p.x, b.y + S(6.0f) - S(6.0f) * (1.0f - t)));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(std::max(width, S(360.0f)), S(320.0f)));
    PushPopupStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    const bool began = ImGui::BeginPopupEx(popupId, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    if (!began) {
        ImGui::PopStyleVar();
        PopPopupStyle();
        return false;
    }
    PopupSurface(1.0f);
    PushPopupAnim(ImVec2(p.x + width * 0.5f, b.y), t);
    g_dropdown.popupId = popupId;
    g_dropdown.width = width - S(12.0f);
    return true;
}

bool DropdownItem(const char* label, bool selected, const char* icon, bool enabled)
{
    const float h = S(32.0f);
    const float w = std::max(g_dropdown.width, ImGui::CalcTextSize(label).x + S(56.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (!enabled) ImGui::EndDisabled();
    const float hv = Anim(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, Motion::Hover);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    if (hv > 0.01f) dl->AddRectFilled(p, b, White(0.10f * hv), S(8.0f));
    if (selected) dl->AddRectFilled(p, b, Accent(0.22f), S(8.0f));
    float x = p.x + S(10.0f);
    const ImU32 col = !enabled ? Pal().textFaint : (selected ? White(1.0f) : Mix(Pal().textDim, Pal().text, hv));
    if (icon) {
        Icon(dl, icon, ImVec2(x + S(7.0f), p.y + h * 0.5f), S(14.0f), col);
        x += S(24.0f);
    }
    const std::string text = VisibleLabel(label);
    dl->AddText(ImVec2(x, p.y + (h - ImGui::GetFontSize()) * 0.5f), col, text.c_str());
    if (selected) Icon(dl, ICON_CHECK, ImVec2(b.x - S(16.0f), p.y + h * 0.5f), S(12.0f), White(0.9f));
    if (pressed && enabled) ImGui::CloseCurrentPopup();
    return pressed && enabled;
}

void EndDropdown()
{
    PopPopupAnim();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    PopPopupStyle();
}

bool Dropdown(const char* id, int* current, const char* const* items, int count, float width)
{
    bool changed = false;
    const int cur = Clamp(*current, 0, count - 1);
    if (BeginDropdown(id, items[cur], width)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (DropdownItem(items[i], i == cur)) {
                *current = i;
                changed = true;
            }
            ImGui::PopID();
        }
        EndDropdown();
    }
    return changed;
}

// ---------------------------------------------------------------------------------------------
// Menus and modals
// ---------------------------------------------------------------------------------------------

bool BeginMenu(const char* popupId)
{
    const ImGuiID id = ImGui::GetID(popupId);
    const bool open = ImGui::IsPopupOpen(id, 0);
    const float t = PopupFade(AnimId(id, 9), open);
    if (!open) return false;
    PushPopupStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    if (!ImGui::BeginPopup(popupId, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleVar();
        PopPopupStyle();
        return false;
    }
    PopupSurface(1.0f);
    PushPopupAnim(ImGui::GetCurrentWindow()->Pos, t);   // menus open at the cursor: grow from that corner
    return true;
}

bool MenuItem(const char* icon, const char* label, const char* shortcut, bool danger, bool enabled)
{
    const float h = S(32.0f);
    const float sw = shortcut ? ImGui::CalcTextSize(shortcut).x + S(24.0f) : 0.0f;
    const float w = std::max(S(200.0f), ImGui::CalcTextSize(label).x + S(44.0f) + sw);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (!enabled) ImGui::EndDisabled();
    const float hv = Anim(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, Motion::Hover);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    const ImU32 base = danger ? Pal().danger : Pal().text;
    if (hv > 0.01f) dl->AddRectFilled(p, b, danger ? ColA(Pal().danger, 0.16f * hv) : White(0.10f * hv), S(8.0f));
    const ImU32 col = !enabled ? Pal().textFaint : (danger ? base : Mix(Pal().textDim, Pal().text, hv));
    if (icon) Icon(dl, icon, ImVec2(p.x + S(18.0f), p.y + h * 0.5f), S(14.0f), col);
    const std::string text = VisibleLabel(label);
    dl->AddText(ImVec2(p.x + S(36.0f), p.y + (h - ImGui::GetFontSize()) * 0.5f), col, text.c_str());
    if (shortcut) {
        PushFont(GetFonts().regular, Type::Caption);
        const ImVec2 ts = ImGui::CalcTextSize(shortcut);
        dl->AddText(ImVec2(b.x - ts.x - S(10.0f), p.y + (h - ts.y) * 0.5f), Pal().textFaint, shortcut);
        PopFont();
    }
    if (pressed && enabled) ImGui::CloseCurrentPopup();
    return pressed && enabled;
}

void MenuSeparator()
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
    ImGui::Dummy(ImVec2(w, S(7.0f)));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + S(8.0f), p.y + S(3.5f)), ImVec2(p.x + w - S(8.0f), p.y + S(3.5f)), White(0.12f));
}

void MenuLabel(const char* text)
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + S(10.0f));
    PushFont(GetFonts().medium, Type::Caption);
    const std::string shown = Ellipsize(text, S(240.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, Pal().textFaint);
    ImGui::Dummy(ImVec2(1, S(2.0f)));
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopStyleColor();
    PopFont();
}

void EndMenu()
{
    PopPopupAnim();
    ImGui::EndPopup();
    ImGui::PopStyleVar();
    PopPopupStyle();
}

bool BeginModal(const char* popupId, const char* title, float width)
{
    const ImGuiID id = ImGui::GetID(popupId);
    const bool open = ImGui::IsPopupOpen(id, 0);
    const float t = PopupFade(AnimId(id, 9), open);
    if (!open) return false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f + S(10.0f) * (1.0f - t)), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24.0f), S(22.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(Tok::RadiusPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.0f), S(10.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                     ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(5);
        return false;
    }
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const ImVec2 a = w->Pos, b(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    const float r = S(Tok::RadiusPanel);
    PopupSurface(1.0f, r);
    PushPopupAnim(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), t);
    if (title && *title) {
        PushFont(GetFonts().semibold, Type::Heading);
        ImGui::TextUnformatted(title);
        PopFont();
        Gap(2.0f);
    }
    return true;
}

void EndModal()
{
    PopPopupAnim();
    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(5);
}

// ---------------------------------------------------------------------------------------------
// Hotkey binder
// ---------------------------------------------------------------------------------------------

static std::vector<std::string> SplitKeys(const std::string& text)
{
    std::vector<std::string> keys;
    size_t start = 0;
    while (start <= text.size()) {
        size_t pos = text.find(" + ", start);
        keys.push_back(text.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 3;
    }
    return keys;
}

float KeyCapsWidth(const std::string& text, float height)
{
    if (text.empty()) return 0.0f;
    PushFont(GetFonts().medium, Type::Caption);
    float w = 0.0f;
    const auto keys = SplitKeys(text);
    for (size_t i = 0; i < keys.size(); ++i) w += std::max(height, ImGui::CalcTextSize(keys[i].c_str()).x + S(14.0f)) + (i ? S(5.0f) : 0.0f);
    PopFont();
    return w;
}

void KeyCaps(ImDrawList* dl, ImVec2 pos, const std::string& text, float height, bool onLight)
{
    PushFont(GetFonts().medium, Type::Caption);
    float x = pos.x;
    for (const std::string& k : SplitKeys(text)) {
        const ImVec2 ts = ImGui::CalcTextSize(k.c_str());
        const float w = std::max(height, ts.x + S(14.0f));
        const ImVec2 a(x, pos.y), b(x + w, pos.y + height);
        if (onLight) {
            dl->AddRectFilled(a, b, IM_COL32(26, 12, 48, 30), S(Tok::RadiusSmall));
            dl->AddRect(a, b, IM_COL32(26, 12, 48, 60), S(Tok::RadiusSmall));
        } else {
            dl->AddRectFilled(ImVec2(a.x, a.y + S(1.5f)), ImVec2(b.x, b.y + S(1.5f)), IM_COL32(8, 0, 24, 90), S(Tok::RadiusSmall));
            dl->AddRectFilled(a, b, White(0.12f), S(Tok::RadiusSmall));
            dl->AddRect(a, b, White(0.22f), S(Tok::RadiusSmall));
        }
        dl->AddText(ImVec2(std::floor(a.x + (w - ts.x) * 0.5f), std::floor(a.y + (height - ts.y) * 0.5f)),
                    onLight ? IM_COL32(26, 12, 48, 230) : Pal().text, k.c_str());
        x += w + S(5.0f);
    }
    PopFont();
}

bool HotkeyBinder(const char* id, const std::string& text, BinderState state, float width)
{
    width = ResolveWidth(width);
    const float h = S(Tok::ControlHeight);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImGuiID gid = ImGui::GetItemID();
    const float hv = Anim(AnimId(gid, 1), hovered ? 1.0f : 0.0f, Motion::Hover);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + width, p.y + h);
    const bool capturing = state == BinderState::Capturing;
    FieldFrame(dl, p, b, hv, capturing ? 1.0f : 0.0f);
    const float cy = p.y + h * 0.5f;
    if (capturing) {
        const float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 5.0f);
        dl->AddCircleFilled(ImVec2(p.x + S(16.0f), cy), S(4.0f), Accent2(pulse));
        dl->AddText(ImVec2(p.x + S(28.0f), cy - ImGui::GetFontSize() * 0.5f), Pal().text, "Press a key\xE2\x80\xA6");
        PushFont(GetFonts().regular, Type::Caption);
        const char* hint = "Esc cancels \xC2\xB7 Backspace clears";
        const ImVec2 hs = ImGui::CalcTextSize(hint);
        if (hs.x < width - S(160.0f)) dl->AddText(ImVec2(b.x - hs.x - S(12.0f), cy - hs.y * 0.5f), Pal().textFaint, hint);
        PopFont();
    } else if (text.empty()) {
        dl->AddText(ImVec2(p.x + S(12.0f), cy - ImGui::GetFontSize() * 0.5f), Pal().textFaint, "Not set \xE2\x80\x94 click to bind");
    } else {
        const float kh = h - S(12.0f);
        KeyCaps(dl, ImVec2(p.x + S(8.0f), p.y + S(6.0f)), text, kh);
        if (state == BinderState::Duplicate || state == BinderState::InUse) {
            const char* msg = state == BinderState::Duplicate ? "Used twice" : "Taken by another app";
            PushFont(GetFonts().medium, Type::Caption);
            const ImVec2 ms = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(b.x - ms.x - S(12.0f), cy - ms.y * 0.5f), Pal().warning, msg);
            PopFont();
        }
    }
    return pressed;
}

// ---------------------------------------------------------------------------------------------
// Decoration
// ---------------------------------------------------------------------------------------------

ImVec2 BadgeSize(const char* text)
{
    PushFont(GetFonts().medium, Type::Caption);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    PopFont();
    return ImVec2(ts.x + S(12.0f), ts.y + S(4.0f));
}

void Badge(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg)
{
    PushFont(GetFonts().medium, Type::Caption);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const ImVec2 b(pos.x + ts.x + S(12.0f), pos.y + ts.y + S(4.0f));
    dl->AddRectFilled(pos, b, bg, S(Tok::RadiusSmall));
    dl->AddText(ImVec2(pos.x + S(6.0f), pos.y + S(2.0f)), fg, text);
    PopFont();
}

void GameAvatar(ImDrawList* dl, ImVec2 pos, float size, const std::string& game)
{
    // A stable colour per game, and its initials.
    uint32_t hsh = 2166136261u;
    for (char c : game) hsh = (hsh ^ (uint8_t)c) * 16777619u;
    static const ImU32 tones[][2] = {
        { IM_COL32(139, 63, 252, 255), IM_COL32(194, 31, 224, 255) }, { IM_COL32(236, 72, 153, 255), IM_COL32(244, 114, 182, 255) },
        { IM_COL32(59, 130, 246, 255), IM_COL32(99, 102, 241, 255) },  { IM_COL32(16, 185, 129, 255), IM_COL32(20, 184, 166, 255) },
        { IM_COL32(245, 158, 11, 255), IM_COL32(239, 68, 68, 255) },   { IM_COL32(6, 182, 212, 255), IM_COL32(59, 130, 246, 255) },
    };
    const auto& tone = tones[hsh % 6];
    const ImVec2 b(pos.x + size, pos.y + size);
    const float r = size * 0.28f;
    GradientRectH(dl, pos, b, tone[0], tone[1], r);
    dl->AddRect(pos, b, White(0.25f), r);
    std::string initials;
    bool next = true;
    for (char c : game) {
        if (c == ' ' || c == '-' || c == '_') {
            next = true;
            continue;
        }
        if (next && initials.size() < 2 && (unsigned char)c < 0x80) initials += (char)toupper((unsigned char)c);
        next = false;
    }
    if (initials.empty()) initials = "?";
    ImFont* f = GetFonts().semibold;
    const float fs = size * 0.42f;
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, initials.c_str());
    dl->AddText(f, fs, ImVec2(std::floor(pos.x + (size - ts.x) * 0.5f), std::floor(pos.y + (size - ts.y) * 0.5f + size * 0.03f)), White(1.0f),
                initials.c_str());
}

void Skeleton(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding)
{
    dl->AddRectFilled(a, b, White(0.07f), rounding);
    const float w = b.x - a.x;
    const float t = std::fmod((float)ImGui::GetTime() * 0.65f, 1.6f) - 0.3f;
    const float x = a.x + w * t;
    dl->PushClipRect(a, b, true);
    GradientRectH(dl, ImVec2(x - w * 0.3f, a.y), ImVec2(x, b.y), White(0.0f), White(0.07f), 0.0f);
    GradientRectH(dl, ImVec2(x, a.y), ImVec2(x + w * 0.3f, b.y), White(0.07f), White(0.0f), 0.0f);
    dl->PopClipRect();
}

void Spinner(ImDrawList* dl, ImVec2 c, float radius, ImU32 col)
{
    const float t = (float)ImGui::GetTime() * 6.0f;
    dl->PathArcTo(c, radius, t, t + 4.2f, 32);
    dl->PathStroke(col, 0, std::max(2.0f, radius * 0.22f));
}

void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float fraction, bool onAccent)
{
    const float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, onAccent ? IM_COL32(40, 0, 80, 110) : White(0.14f), r);
    const float x = a.x + (b.x - a.x) * Clamp(fraction, 0.0f, 1.0f);
    if (x > a.x + 1.0f) {
        if (onAccent) dl->AddRectFilled(a, ImVec2(x, b.y), White(1.0f), r);
        else GradientRectH(dl, a, ImVec2(x, b.y), Accent(), Accent2(), r);
    }
}

void PulseDot(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
    const float t = (float)ImGui::GetTime();
    const float phase = std::fmod(t, 1.4f) / 1.4f;
    dl->AddCircleFilled(c, r * (1.0f + 1.6f * phase), ColA(col, 0.35f * (1.0f - phase)));
    dl->AddCircleFilled(c, r * 1.8f, ColA(col, 0.18f));
    dl->AddCircleFilled(c, r, col);
}

static float PeakToUnit(float peak)
{
    if (peak <= 0.00001f) return 0.0f;
    return Clamp((20.0f * std::log10(peak) + 60.0f) / 60.0f, 0.0f, 1.0f);
}

void LevelMeter(const char* id, float peak, ImVec2 size, bool dimmed)
{
    const ImGuiID gid = ImGui::GetID(id);
    const float lvl = PeakToUnit(peak);
    const float shown = Anim(gid, lvl, lvl > Anim(AnimId(gid, 1), 0.0f, 0.0f) ? 40.0f : 6.0f);
    AnimSet(AnimId(gid, 1), shown);
    size.x = ResolveWidth(size.x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = std::max(12, (int)(size.x / S(6.0f)));
    const float gap = S(2.0f), segW = (size.x - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        const float t0 = (float)i / n;
        const ImVec2 a(p.x + i * (segW + gap), p.y), b(a.x + segW, p.y + size.y);
        ImU32 on = t0 > 0.85f ? Pal().danger : (t0 > 0.7f ? Pal().warning : Mix(Accent(), Accent2(), t0 / 0.7f));
        if (dimmed) on = Pal().textFaint;
        dl->AddRectFilled(a, b, t0 < shown ? on : White(0.10f), S(2.0f));
    }
}

void Divider(float alpha)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 0.5f), ImVec2(p.x + w, p.y + 0.5f), White(alpha));
    ImGui::Dummy(ImVec2(w, 1.0f));
}

// ---------------------------------------------------------------------------------------------
// Panels and headers
// ---------------------------------------------------------------------------------------------

namespace {
struct PanelFrame {
    float rounding, border;
};
std::vector<PanelFrame> g_panels;
} // namespace

bool BeginPanel(const char* id, ImVec2 size, float padding, bool scroll, float rounding, float border)
{
    g_panels.push_back({ rounding, border });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(padding), S(padding)));
    ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (size.y == 0.0f) cf |= ImGuiChildFlags_AutoResizeY;
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoBackground;
    if (!scroll) wf |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool r = ImGui::BeginChild(id, size, cf, wf);
    ImGui::PopStyleVar();
    return r;
}

void EndPanel()
{
    ImGui::Dummy(ImVec2(0, 0));   // closes any SetCursorPos so the child can size itself
    ImGui::EndChild();
    const PanelFrame f = g_panels.back();
    g_panels.pop_back();
    // The child draws after its parent, so painting the glass on the parent now still lands behind it.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    Glass(dl, a, b, S(f.rounding), 0.0f, f.border);
}

void PageHeader(const char* title, const char* subtitle)
{
    PushFont(GetFonts().semibold, Type::Title);
    ImGui::TextUnformatted(title);
    PopFont();
    if (subtitle) {
        PushFont(GetFonts().regular, Type::Body);
        ImGui::PushStyleColor(ImGuiCol_Text, Pal().textDim);
        ImGui::TextUnformatted(subtitle);
        ImGui::PopStyleColor();
        PopFont();
    }
}

void SectionHeader(const char* title, const char* right)
{
    PushFont(GetFonts().semibold, Type::Heading);
    const float y = ImGui::GetCursorPosY();
    ImGui::TextUnformatted(title);
    PopFont();
    if (right) {
        PushFont(GetFonts().regular, Type::Body);
        const float w = ImGui::CalcTextSize(right).x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
        ImGui::SetCursorPosY(y + S(2.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, Pal().textFaint);
        ImGui::TextUnformatted(right);
        ImGui::PopStyleColor();
        PopFont();
    }
}

} // namespace cs::ui
