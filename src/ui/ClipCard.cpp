// The clip/screenshot card: thumbnail, duration badge, title, game and time.
// Hover: the card lifts, the preview plays, and Edit + "..." fade in.
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"
#include "core/Util.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace cs::ui {

float CardTextHeight() { return S(56.0f); }

// Cover-fit a texture of size (tw, th) into a box with aspect `box`, optionally zoomed.
static void CoverUv(float box, float tw, float th, float zoom, ImVec2& uv0, ImVec2& uv1)
{
    uv0 = ImVec2(0, 0);
    uv1 = ImVec2(1, 1);
    if (tw > 0 && th > 0) {
        const float tex = tw / th;
        if (tex > box) {
            const float u = (1.0f - box / tex) * 0.5f;
            uv0.x = u;
            uv1.x = 1.0f - u;
        } else {
            const float v = (1.0f - tex / box) * 0.5f;
            uv0.y = v;
            uv1.y = 1.0f - v;
        }
    }
    const ImVec2 d((uv1.x - uv0.x) * zoom * 0.5f, (uv1.y - uv0.y) * zoom * 0.5f);
    uv0 = ImVec2(uv0.x + d.x, uv0.y + d.y);
    uv1 = ImVec2(uv1.x - d.x, uv1.y - d.y);
}

CardResult ClipCard(const char* id, const CardData& d, ImVec2 size)
{
    CardResult res;
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##card", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImGuiID cid = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    res.hovered = hovered;
    res.clicked = clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    res.rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hov = AnimHover(cid, hovered);
    const float press = Anim(AnimId(cid, 2), ImGui::IsItemActive() ? 1.0f : 0.0f, Motion::Press);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Lift on hover (spring), sink a little while pressed.
    const float liftK = Animations() ? Spring(AnimId(cid, 21), hovered && !ImGui::IsItemActive() ? 1.0f : 0.0f, Springs::Hover) : 0.0f;
    const float lift = S(Motion::HoverLift) * liftK;
    const float shrink = S(3.0f) * press;
    const ImVec2 a(p.x + shrink, p.y - lift + shrink), b(p.x + size.x - shrink, p.y + size.y - lift - shrink);
    const float rad = S(Tok::RadiusCard);
    // 3D tilt towards the cursor: the side under the pointer dips away, a few degrees at most.
    float tiltX = 0.0f, tiltY = 0.0f;
    if (Animations()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float nx = hovered ? Clamp((m.x - (p.x + size.x * 0.5f)) / (size.x * 0.5f), -1.0f, 1.0f) : 0.0f;
        const float ny = hovered ? Clamp((m.y - (p.y + size.y * 0.5f)) / (size.y * 0.5f), -1.0f, 1.0f) : 0.0f;
        const float maxTilt = 3.0f * 3.14159f / 180.0f;
        tiltX = Spring(AnimId(cid, 22), -nx * maxTilt, Springs::Tilt);
        tiltY = Spring(AnimId(cid, 23), -ny * maxTilt, Springs::Tilt);
    }
    const int tiltStart = dl->VtxBuffer.Size;
    Shadow(dl, a, b, rad, 0.7f + 1.0f * hov);
    Glass(dl, a, b, rad, hov * 0.6f, Tok::GlassBorderSoft, 0, 2);

    // Thumbnail (16:9), inset in the card.
    const float inset = S(6.0f);
    const ImVec2 ia(a.x + inset, a.y + inset);
    const float thumbH = d.compact ? (b.y - a.y - inset * 2.0f) : (b.x - a.x - inset * 2.0f) * 9.0f / 16.0f;
    const ImVec2 ib(b.x - inset, ia.y + thumbH);
    const float irad = S(Tok::RadiusCard - 4.0f);
    const float boxAspect = (ib.x - ia.x) / std::max(1.0f, ib.y - ia.y);

    // Hover preview: steps through the filmstrip (about 4 frames a second) with a soft crossfade.
    const bool preview = hovered && d.strip != ImTextureID_Invalid && d.stripFrames > 1;
    const ImGuiID previewTimer = AnimId(cid, 3);
    if (!hovered) TimerStop(previewTimer);
    else if (TimerElapsed(previewTimer) > 1e5f) TimerStart(previewTimer);
    dl->AddRectFilled(ia, ib, IM_COL32(8, 0, 20, 170), irad);
    if (d.thumb != ImTextureID_Invalid && d.thumbW > 0) {
        ImVec2 uv0, uv1;
        CoverUv(boxAspect, (float)d.thumbW, (float)d.thumbH, 0.06f * hov, uv0, uv1);
        dl->AddImageRounded(d.thumb, ia, ib, uv0, uv1, IM_COL32_WHITE, irad);
    } else if (d.thumbLoading) {
        Skeleton(dl, ia, ib, irad);
    } else {
        GradientRect(dl, ia, ib, IM_COL32(60, 30, 110, 255), IM_COL32(30, 14, 60, 255), irad);
        Icon(dl, d.isImage ? ICON_PICTURE : ICON_VIDEO, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), S(26.0f), Pal().textFaint);
    }
    float progress = -1.0f;
    if (preview) {
        const float fpsPreview = 4.0f;
        const float t = TimerElapsed(previewTimer) * fpsPreview;
        const int n = d.stripFrames;
        const int f0 = (int)t % n, f1 = (f0 + 1) % n;
        const float blend = Clamp((t - std::floor(t) - 0.6f) / 0.4f, 0.0f, 1.0f);
        const float fw = 1.0f / n;
        ImVec2 uv0, uv1;
        CoverUv(boxAspect, 16.0f, 9.0f, 0.0f, uv0, uv1);
        const float fade = Clamp(TimerElapsed(previewTimer) / 0.25f, 0.0f, 1.0f);   // ease in over the thumbnail
        dl->AddImageRounded(d.strip, ia, ib, ImVec2((f0 + uv0.x) * fw, uv0.y), ImVec2((f0 + uv1.x) * fw, uv1.y), White(fade), irad);
        if (blend > 0.0f)
            dl->AddImageRounded(d.strip, ia, ib, ImVec2((f1 + uv0.x) * fw, uv0.y), ImVec2((f1 + uv1.x) * fw, uv1.y), White(blend * fade), irad);
        progress = std::fmod(t, (float)n) / n;
    }
    dl->AddRect(ia, ib, White(0.10f), irad);

    // Darken the lower part on hover so text and buttons read on any image.
    if (hov > 0.01f)
        GradientRect(dl, ImVec2(ia.x, ia.y + (ib.y - ia.y) * 0.45f), ib, IM_COL32(8, 0, 20, 0), IM_COL32(8, 0, 20, (int)(160 * hov)), irad,
                     ImDrawFlags_RoundCornersBottom);
    if (progress >= 0.0f) dl->AddRectFilled(ImVec2(ia.x + irad * 0.5f, ib.y - S(3.0f)), ImVec2(ia.x + (ib.x - ia.x) * progress, ib.y - S(1.0f)), Accent2(0.95f));

    if (d.badge && *d.badge) {
        const ImVec2 bs = BadgeSize(d.badge);
        Badge(dl, ImVec2(ib.x - bs.x - S(8.0f), ib.y - bs.y - S(8.0f)), d.badge, IM_COL32(10, 4, 24, 190), White(1.0f));
    }

    // Glare: a soft highlight on the glass that slides with the cursor.
    if (hov > 0.01f && ImGui::IsMousePosValid()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float cx = (a.x + b.x) * 0.5f;
        const ImVec2 gc(cx - (m.x - cx) * 0.8f, a.y + (b.y - a.y) * 0.15f);   // mirrored, like a reflection
        dl->PushClipRect(ImVec2(a.x + rad * 0.3f, a.y + 1.0f), ImVec2(b.x - rad * 0.3f, b.y - 1.0f), true);
        RadialGlow(dl, gc, (b.x - a.x) * 0.55f, White(0.10f * hov), 40);
        dl->PopClipRect();
    }
    const int buttonsStart = dl->VtxBuffer.Size;

    // Two quick actions (top-right): Edit (clips only) and "...". They aren't tilted, so where you
    // see them is exactly where they click.
    if (hov > 0.05f) {
        const float bs = S(30.0f), gap = S(6.0f);
        const int count = d.isImage ? 1 : 2;
        float x = ib.x - S(8.0f) - bs * count - gap * (count - 1);
        const float y = ia.y + S(8.0f) - S(4.0f) * (1.0f - hov);
        for (int i = 0; i < count; ++i) {
            const bool isMore = d.isImage || i == 1;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, hov);
            ButtonOpts o;
            o.style = Btn::Secondary;
            o.icon = isMore ? ICON_MORE : ICON_TRIM;
            o.size = ImVec2(bs, bs);
            o.tooltip = isMore ? "More" : "Edit";
            if (Button(isMore ? "##more" : "##edit", o)) {
                (isMore ? res.more : res.edit) = true;
                res.clicked = false;
            }
            ImGui::PopStyleVar();
            x += bs + gap;
        }
    }

    const int buttonsEnd = dl->VtxBuffer.Size;
    if (!d.compact) {
        // Title, then game avatar + game name and the time.
        const float tx = ia.x + S(2.0f), tw = ib.x - ia.x - S(4.0f);
        float ty = ib.y + S(10.0f);
        PushFont(GetFonts().medium, Type::Body);
        const std::string title = Ellipsize(d.title, tw);
        dl->AddText(ImVec2(tx, ty), Pal().text, title.c_str());
        ty += ImGui::GetFontSize() + S(6.0f);
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        const float av = S(16.0f);
        GameAvatar(dl, ImVec2(tx, ty - S(1.0f)), av, d.game);
        const float whenW = ImGui::CalcTextSize(d.when).x;
        const float gx = tx + av + S(6.0f);
        const std::string game = Ellipsize(d.game, tx + tw - whenW - S(10.0f) - gx);
        dl->AddText(ImVec2(gx, ty), Pal().textDim, game.c_str());
        dl->AddText(ImVec2(tx + tw - whenW, ty), Pal().textFaint, d.when);
        PopFont();
    } else if (hov > 0.01f) {
        PushFont(GetFonts().medium, Type::Caption);
        const std::string title = Ellipsize(d.title, ib.x - ia.x - S(20.0f) - (d.badge && *d.badge ? BadgeSize(d.badge).x + S(8.0f) : 0.0f));
        dl->AddText(ImVec2(ia.x + S(10.0f), ib.y - S(10.0f) - ImGui::GetFontSize()), White(hov), title.c_str());
        PopFont();
    }

    // The wave on click, and a light sweep when the pointer arrives.
    WaveFx(dl, cid, a, b, rad, hovered, clicked, Accent2());
    if (std::fabs(tiltX) > 1e-4f || std::fabs(tiltY) > 1e-4f) {
        TiltVerts(dl, tiltStart, a, b, tiltX, tiltY, 1.0f, buttonsStart);
        TiltVerts(dl, buttonsEnd, a, b, tiltX, tiltY, 1.0f);
    }

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(size);
    ImGui::PopID();
    return res;
}

} // namespace cs::ui
