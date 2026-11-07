// Screenshot viewer: a glass sheet over the app with zoom (wheel, buttons, double-click for 100%),
// pan (drag), previous / next (arrows), Copy, a "..." menu (rename, favourite, folder, delete) and close.
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Thumbnailer.h"
#include "core/Util.h"
#include "core/Window.h"
#include "media/MediaCache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cs::ui {

namespace {

ButtonOpts IconOpts(const char* icon, const char* tip, float size, Btn style = Btn::Ghost, bool enabled = true)
{
    ButtonOpts o;
    o.style = style;
    o.icon = icon;
    o.size = ImVec2(size, size);
    o.tooltip = tip;
    o.round = true;
    o.enabled = enabled;
    return o;
}

int SequenceIndex(const ImageViewerState& v)
{
    for (size_t i = 0; i < v.sequence.size(); ++i)
        if (v.sequence[i] == v.current) return (int)i;
    return -1;
}

void Step(AppContext& ctx, int dir)
{
    ImageViewerState& v = ctx.ui.viewer;
    const int j = SequenceIndex(v) + dir;
    if (SequenceIndex(v) < 0 || j < 0 || j >= (int)v.sequence.size()) return;
    if (const ClipInfo* c = FindClip(ctx, v.sequence[(size_t)j])) {
        v.current = c->id;
        v.clip = *c;
        v.zoom = 1.0f;
        v.actualSize = false;
        v.pan = ImVec2(0, 0);
    }
}

bool SideArrow(const char* id, ImVec2 center, bool left, bool enabled)
{
    const float s = S(44.0f);
    ImGui::SetCursorScreenPos(ImVec2(center.x - s * 0.5f, center.y - s * 0.5f));
    return Button(id, IconOpts(left ? ICON_CHEVRON_LEFT : ICON_CHEVRON_RIGHT, left ? "Previous (\xE2\x86\x90)" : "Next (\xE2\x86\x92)", s, Btn::Secondary, enabled));
}

void Copy(AppContext& ctx, const ClipInfo& c)
{
    const bool ok = platform::CopyImageToClipboard(ctx.window.Hwnd(), c.path);
    Snack(ctx.ui, ok ? "Copied to the clipboard" : "Couldn't copy to the clipboard", L"", !ok);
}

// Title + details on the left; Copy, "..." and close on the right.
void Header(AppContext& ctx, ImVec2 a, float width, float h)
{
    ImageViewerState& v = ctx.ui.viewer;
    const ClipInfo& c = v.clip;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = a.y + h * 0.5f;
    GameAvatar(dl, ImVec2(a.x, cy - S(18.0f)), S(36.0f), GameName(c));
    PushFont(GetFonts().semibold, Type::Heading);
    const std::string title = Ellipsize(c.title.c_str(), width * 0.5f);
    dl->AddText(ImVec2(a.x + S(48.0f), cy - S(19.0f)), Pal().text, title.c_str());
    PopFont();
    const int idx = SequenceIndex(v);
    char meta[200];
    snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %s  \xC2\xB7  %d of %d", GameName(c).c_str(), RelativeTime(c.modified).c_str(), idx + 1,
             (int)v.sequence.size());
    PushFont(GetFonts().regular, Type::Caption);
    dl->AddText(ImVec2(a.x + S(48.0f), cy + S(3.0f)), Pal().textDim, meta);
    PopFont();

    const float bs = S(Tok::ControlHeight);
    float x = a.x + width - bs;
    ImGui::SetCursorScreenPos(ImVec2(x, cy - bs * 0.5f));
    if (Button("##iclose", IconOpts(ICON_CLOSE, "Close (Esc)", bs, Btn::Secondary))) CloseImage(ctx);
    x -= bs + S(14.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, cy - bs * 0.5f));
    if (Button("##imore", IconOpts(ICON_MORE, "More", bs))) {
        ctx.ui.library.target = c;
        ctx.ui.library.menuRequested = true;
    }
    ButtonOpts copy;
    copy.icon = ICON_COPY;
    copy.tooltip = "Copy to the clipboard (Ctrl+C)";
    PushFont(GetFonts().medium, Type::Body);
    const float cw = ImGui::CalcTextSize("Copy").x + S(56.0f);
    PopFont();
    x -= cw + S(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, cy - bs * 0.5f));
    copy.size = ImVec2(cw, bs);
    if (Button("Copy", copy)) Copy(ctx, c);
}

void ImageView(AppContext& ctx, ImVec2 a, ImVec2 b)
{
    ImageViewerState& v = ctx.ui.viewer;
    const ClipInfo& c = v.clip;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImageInfo img = ctx.images.Get(c.path);
    // Prefetch neighbours so arrowing through is instant.
    const int idx = SequenceIndex(v);
    for (int d : { -1, 1 })
        if (idx + d >= 0 && idx + d < (int)v.sequence.size())
            if (const ClipInfo* n = FindClip(ctx, v.sequence[(size_t)(idx + d)])) ctx.images.Prefetch(n->path);

    const float aw = b.x - a.x, ah = b.y - a.y;
    int iw = img.width, ih = img.height;
    ImTextureID tex = (ImTextureID)img.texture;
    if (!tex) {   // show the thumbnail until the full image is ready
        const ThumbInfo t = ctx.thumbs.Get(c.path, c.modified, 512);
        tex = (ImTextureID)t.texture;
        iw = c.width > 0 ? c.width : t.width;
        ih = c.height > 0 ? c.height : t.height;
    }
    if (iw <= 0 || ih <= 0) {
        if (img.failed) {
            Icon(dl, ICON_WARNING, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - S(14.0f)), S(26.0f), Pal().warning);
            const char* msg = "This image couldn't be opened.";
            dl->AddText(ImVec2((a.x + b.x - ImGui::CalcTextSize(msg).x) * 0.5f, (a.y + b.y) * 0.5f + S(10.0f)), Pal().textDim, msg);
        } else {
            Spinner(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), S(14.0f), Accent2());
        }
        return;
    }
    const float fit = std::min(aw / iw, ah / ih);
    const float target = v.actualSize ? 1.0f / std::max(fit, 1e-4f) : v.zoom;   // zoom is relative to fit
    const ImGuiID zid = ImGui::GetID("##imgzoom");
    const float zoom = Anim(zid, target, Motion::Slide * 1.4f);
    const float scale = fit * zoom;
    const ImVec2 size(iw * scale, ih * scale);
    // Pan limits keep the image on screen.
    const float mx = std::max(0.0f, (size.x - aw) * 0.5f), my = std::max(0.0f, (size.y - ah) * 0.5f);
    v.pan.x = Clamp(v.pan.x, -mx, mx);
    v.pan.y = Clamp(v.pan.y, -my, my);
    const ImVec2 c0((a.x + b.x) * 0.5f + v.pan.x, (a.y + b.y) * 0.5f + v.pan.y);
    const ImVec2 ia(c0.x - size.x * 0.5f, c0.y - size.y * 0.5f), ib(c0.x + size.x * 0.5f, c0.y + size.y * 0.5f);

    // Interaction surface over the whole area (clicks outside the image close the viewer).
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##imgarea", ImVec2(aw, ah), ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool overImage = m.x >= ia.x && m.x <= ib.x && m.y >= ia.y && m.y <= ib.y;
    if (hovered && size.x > aw + 1.0f) ImGui::SetMouseCursor(ImGui::IsItemActive() ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        v.pan.x += d.x;
        v.pan.y += d.y;
    }
    if (ImGui::IsItemDeactivated() && ImGui::GetIO().MouseDragMaxDistanceAbs[0].x + ImGui::GetIO().MouseDragMaxDistanceAbs[0].y < 4.0f && !overImage) CloseImage(ctx);
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && overImage) {
        v.actualSize = !v.actualSize;
        v.zoom = 1.0f;
        v.pan = ImVec2(0, 0);
    }
    const float wheel = ImGui::GetIO().MouseWheel;
    if (hovered && wheel != 0.0f) {
        // Zoom around the cursor.
        const float cur = v.actualSize ? 1.0f / fit : v.zoom;
        const float next = Clamp(cur * std::pow(1.2f, wheel), 1.0f, 16.0f);
        const float k = next / cur;
        const ImVec2 rel(m.x - (a.x + b.x) * 0.5f - v.pan.x, m.y - (a.y + b.y) * 0.5f - v.pan.y);
        v.pan.x -= rel.x * (k - 1.0f);
        v.pan.y -= rel.y * (k - 1.0f);
        v.zoom = next;
        v.actualSize = false;
        AnimSet(zid, next);   // wheel zoom is immediate so the point under the cursor stays put
    }

    dl->PushClipRect(a, b, true);
    Shadow(dl, ia, ib, S(6.0f), 1.6f);
    if (tex) dl->AddImageRounded(tex, ia, ib, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, S(6.0f));
    dl->PopClipRect();
    if (!img.texture && !img.failed) Spinner(dl, ImVec2(b.x - S(24.0f), a.y + S(24.0f)), S(9.0f), Accent2());

    // Side arrows.
    if (SideArrow("##iprev", ImVec2(a.x + S(34.0f), (a.y + b.y) * 0.5f), true, idx > 0)) Step(ctx, -1);
    if (SideArrow("##inext", ImVec2(b.x - S(34.0f), (a.y + b.y) * 0.5f), false, idx >= 0 && idx + 1 < (int)v.sequence.size())) Step(ctx, 1);

    // Zoom toolbar.
    const float th = S(44.0f), tw = S(250.0f);
    const ImVec2 ta((a.x + b.x - tw) * 0.5f, b.y - th - S(14.0f)), tb(ta.x + tw, ta.y + th);
    dl->AddRectFilled(ta, tb, ColA(Pal().surface, 0.92f), th * 0.5f);
    dl->AddRect(ta, tb, White(Tok::GlassBorder), th * 0.5f);
    const float bs = S(32.0f);
    float x = ta.x + S(8.0f);
    const float y = ta.y + (th - bs) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ButtonOpts zo = IconOpts(ICON_ZOOM_OUT, "Zoom out", bs);
    zo.enabled = zoom > 1.01f;
    if (Button("##zout", zo)) {
        v.zoom = std::max(1.0f, (v.actualSize ? 1.0f / fit : v.zoom) / 1.4f);
        v.actualSize = false;
    }
    x += bs + S(4.0f);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(scale * 100.0f));
    PushFont(GetFonts().medium, Type::Body);
    const float pw = S(64.0f);
    dl->AddText(ImVec2(x + (pw - ImGui::CalcTextSize(pct).x) * 0.5f, ta.y + (th - ImGui::GetFontSize()) * 0.5f), Pal().text, pct);
    PopFont();
    x += pw + S(4.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (Button("##zin", IconOpts(ICON_ZOOM_IN, "Zoom in", bs))) {
        v.zoom = std::min(16.0f, (v.actualSize ? 1.0f / fit : v.zoom) * 1.4f);
        v.actualSize = false;
    }
    x += bs + S(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ButtonOpts fit100;
    fit100.style = Btn::Ghost;
    fit100.size = ImVec2(tb.x - x - S(8.0f), bs);
    if (Button(v.actualSize || v.zoom > 1.01f ? "Fit##fit" : "100%##fit", fit100)) {
        const bool toFit = v.actualSize || v.zoom > 1.01f;
        v.actualSize = !toFit;
        v.zoom = 1.0f;
        v.pan = ImVec2(0, 0);
    }
}

} // namespace

void DrawImageViewer(AppContext& ctx)
{
    ImageViewerState& v = ctx.ui.viewer;
    const float shown = Anim(ImGui::GetID("##imageviewerA"), v.open ? 1.0f : 0.0f, Motion::Page);
    if (!v.open) return;
    if (const ClipInfo* live = FindClip(ctx, v.current)) {
        v.clip = *live;
    } else if (auto snap = LibrarySnapshot(ctx)) {
        // Renamed: its id changed with the path.
        bool found = false;
        for (const ClipInfo& c : *snap)
            if (c.path == v.clip.path) {
                for (auto& id : v.sequence)
                    if (id == v.current) id = c.id;
                v.current = c.id;
                v.clip = c;
                found = true;
                break;
            }
        (void)found;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float topH = S(Tok::TopBarHeight) + S(12.0f);
    const ImVec2 wa(vp->Pos.x, vp->Pos.y + topH + S(12.0f)), wb(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    const float e = EaseOutCubic(shown);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + topH));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - topH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, e);
    ImGui::Begin("##imageviewer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking);
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(vp->Pos.x, vp->Pos.y + topH), wb, IM_COL32(8, 2, 22, (int)(170 * e)));
    const float m = S(12.0f), lift = S(10.0f) * (1.0f - e);
    const ImVec2 pa(wa.x + m, wa.y + lift), pb(wb.x - m, wb.y - m + lift);
    Shadow(dl, pa, pb, S(Tok::RadiusPanel), 1.6f);
    dl->AddRectFilled(pa, pb, IM_COL32(20, 8, 46, 236), S(Tok::RadiusPanel));
    GradientRect(dl, pa, pb, White(0.06f), White(0.02f), S(Tok::RadiusPanel));
    dl->AddRect(pa, pb, White(Tok::GlassBorder), S(Tok::RadiusPanel));

    const float pad = S(20.0f), headerH = S(56.0f);
    Header(ctx, ImVec2(pa.x + pad, pa.y + S(8.0f)), pb.x - pa.x - pad * 2.0f, headerH);
    if (v.open) ImageView(ctx, ImVec2(pa.x + pad, pa.y + headerH + S(16.0f)), ImVec2(pb.x - pad, pb.y - pad));

    ImGuiIO& io = ImGui::GetIO();
    if (v.open && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CloseImage(ctx);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) Step(ctx, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) Step(ctx, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_0, false)) {
            v.actualSize = false;
            v.zoom = 1.0f;
            v.pan = ImVec2(0, 0);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            ctx.ui.library.target = v.clip;
            ctx.ui.library.openDelete = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            ctx.ui.library.target = v.clip;
            snprintf(ctx.ui.library.renameBuf, sizeof(ctx.ui.library.renameBuf), "%s", v.clip.title.c_str());
            ctx.ui.library.openRename = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) Copy(ctx, v.clip);
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace cs::ui
