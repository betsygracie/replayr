// The editor: one clip, cut into sections, sped up or slowed down, captioned and exported.
//
//   [← Clips]  [ editable title ...................... ]            [ Export ]
//   [                 video (click = play/pause, double-click = fullscreen)    ]
//   [▶]  0:12 / 0:30                                          [🔊 ───]  [⛶]
//   [✂ Split] [🗑 Delete] [Speed 0.25× 0.5× 1× 1.5× 2×] [T Add text]   0:18 total
//   [▕ section ▏    cut     ▕ section ▏] filmstrip with playhead
//   [   caption   ]                     caption lane
//
// Keys: Space play/pause, ←/→ ±1 s, S split, Delete remove, T add text, I/O trim the section
// under the playhead, Ctrl+Z undo, Esc back.
#include "ui/AppContext.h"
#include "imgui_internal.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/MediaPlayer.h"
#include "core/Util.h"
#include "media/Banner.h"
#include "media/MediaCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cs::ui {

namespace {

constexpr double kMinSection = 0.5;       // seconds
constexpr float kSnapPx = 8.0f;           // edges snap to the playhead within this distance
const float kSpeeds[] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f };
const char* kSpeedNames[] = { "0.25\xC3\x97", "0.5\xC3\x97", "1\xC3\x97", "1.5\xC3\x97", "2\xC3\x97" };
const double kSizeLimits[] = { 0.0, 10.0, 25.0, 50.0, 100.0 };
const float kCaptionY[] = { 0.12f, 0.5f, 0.86f };

double ClipDuration(AppContext& ctx)
{
    const double d = ctx.player.Duration();
    return d > 0.0 ? d : ctx.ui.editor.clip.duration;
}

void Sync(EditorState& e)
{
    if (e.sections.empty()) return;
    e.in = e.sections.front().start;
    e.out = e.sections.back().end;
    if (e.selSection >= (int)e.sections.size()) e.selSection = -1;
    if (e.selCaption >= (int)e.captions.size()) e.selCaption = -1;
}

void PushUndo(EditorState& e)
{
    e.undo.push_back({ e.sections, e.captions });
    if (e.undo.size() > 100) e.undo.erase(e.undo.begin());
}

void Undo(EditorState& e)
{
    if (e.undo.empty()) return;
    e.sections = e.undo.back().sections;
    e.captions = e.undo.back().captions;
    e.undo.pop_back();
    Sync(e);
}

int SectionAt(const EditorState& e, double t)
{
    for (size_t i = 0; i < e.sections.size(); ++i)
        if (t >= e.sections[i].start - 1e-4 && t < e.sections[i].end - 1e-4) return (int)i;
    return -1;
}

double OutputLength(const EditorState& e)
{
    double total = 0.0;
    for (const EditSection& s : e.sections) total += (s.end - s.start) / s.speed;
    return total;
}

// Source time -> output time (a time in a cut maps to where the next section starts).
double ToOutput(const EditorState& e, double t)
{
    double out = 0.0;
    for (const EditSection& s : e.sections) {
        if (t < s.start) return out;
        if (t < s.end) return out + (t - s.start) / s.speed;
        out += (s.end - s.start) / s.speed;
    }
    return out;
}

bool Split(EditorState& e, double t)
{
    const int k = SectionAt(e, t);
    if (k < 0) return false;
    const EditSection s = e.sections[(size_t)k];
    if (t - s.start < 0.25 || s.end - t < 0.25) return false;
    PushUndo(e);
    EditSection a = s, b = s;
    a.end = t;
    b.start = t;
    e.sections[(size_t)k] = a;
    e.sections.insert(e.sections.begin() + k + 1, b);
    e.selSection = k + 1;
    e.selCaption = -1;
    Sync(e);
    return true;
}

void DeleteSelected(AppContext& ctx)
{
    EditorState& e = ctx.ui.editor;
    if (e.selCaption >= 0) {
        PushUndo(e);
        e.captions.erase(e.captions.begin() + e.selCaption);
        e.selCaption = -1;
    } else if (e.selSection >= 0) {
        if (e.sections.size() <= 1) {
            Snack(ctx.ui, "That's the only section left. Split it first (S) to cut a part out.");
            return;
        }
        PushUndo(e);
        e.sections.erase(e.sections.begin() + e.selSection);
        e.selSection = -1;
        Sync(e);
        // Land on the next kept section.
        const double pos = ctx.player.Position();
        if (SectionAt(e, pos) < 0) {
            for (const EditSection& s : e.sections)
                if (s.start > pos) {
                    ctx.player.Seek(s.start);
                    return;
                }
            ctx.player.Seek(e.sections.front().start);
        }
    }
}

void AddCaption(EditorState& e, double pos, double dur)
{
    PushUndo(e);
    EditCaption c;
    snprintf(c.text, sizeof(c.text), "%s", "Your text");
    c.start = Clamp(pos, 0.0, std::max(0.0, dur - 0.5));
    c.end = std::min(dur, c.start + 3.0);
    e.captions.push_back(c);
    e.selCaption = (int)e.captions.size() - 1;
    e.selSection = -1;
}

ButtonOpts IconOpts(const char* icon, const char* tip, float size, Btn style = Btn::Ghost)
{
    ButtonOpts o;
    o.style = style;
    o.icon = icon;
    o.size = ImVec2(size, size);
    o.tooltip = tip;
    o.round = true;
    return o;
}

// Outlined text, like the export draws it.
void CaptionPreview(ImDrawList* dl, const EditCaption& c, ImVec2 ia, ImVec2 sz, float alpha)
{
    if (!c.text[0] || alpha <= 0.01f) return;
    ImFont* f = GetFonts().semibold;
    const float px = std::max(8.0f, sz.y * CaptionHeightFraction(c.size));
    const ImVec2 ts = f->CalcTextSizeA(px, sz.x * 0.9f, sz.x * 0.9f, c.text);
    const float cx = ia.x + sz.x * 0.5f, cy = ia.y + sz.y * kCaptionY[Clamp(c.position, 0, 2)];
    const ImVec2 p(Clamp(cx - ts.x * 0.5f, ia.x, ia.x + sz.x - ts.x), Clamp(cy - ts.y * 0.5f, ia.y, ia.y + sz.y - ts.y));
    const float o = std::max(1.0f, px * 0.07f);
    const ImU32 dark = IM_COL32(12, 6, 24, (int)(235 * alpha));
    for (int k = 0; k < 8; ++k) {
        const float ang = k * 0.785398f;
        dl->AddText(f, px, ImVec2(p.x + std::cos(ang) * o, p.y + std::sin(ang) * o), dark, c.text, nullptr, sz.x * 0.9f);
    }
    dl->AddText(f, px, p, White(alpha), c.text, nullptr, sz.x * 0.9f);
}

} // namespace

// The live banner preview: the same bitmap the export uses, with the same timing.
void DrawBannerPreview(AppContext& ctx, ImVec2 ia, float scale, int vw, int vh, double t)
{
    const BannerStyle style = BannerStyleFor(ctx.settings);
    const ImageInfo img = ctx.images.Banner(vw, vh, style);
    if (!img.texture) return;
    const BannerLayout l = BannerLayoutFor(vw, vh, style.corner);
    const BannerFrame f = BannerAt(t, l, style.mode);
    if (!f.visible || f.alpha <= 0.005f) return;
    const float dir = (style.corner == 1 || style.corner == 3) ? 1.0f : -1.0f;
    const float x = ia.x + ((float)(l.x - l.pad) + f.slide * dir) * scale;
    const float y = ia.y + (float)(l.y - l.pad) * scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(x, y), b(x + img.width * scale, y + img.height * scale);
    dl->AddImage((ImTextureID)img.texture, a, b, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * f.alpha)));
    if (f.shine >= 0.0f) {
        // One slanted light band across the pill.
        const ImVec2 pa(x + l.pad * scale, y + l.pad * scale), pb(pa.x + l.w * scale, pa.y + l.h * scale);
        const float h = pb.y - pa.y, bw = h * 0.9f;
        const float cx = pa.x - bw + (pb.x - pa.x + bw * 2.0f) * f.shine;
        dl->PushClipRect(pa, pb, true);
        const ImU32 c0 = White(0.0f), c1 = White(0.32f * f.alpha);
        dl->AddRectFilledMultiColor(ImVec2(cx - bw * 0.5f, pa.y), ImVec2(cx, pb.y), c0, c1, c1, c0);
        dl->AddRectFilledMultiColor(ImVec2(cx, pa.y), ImVec2(cx + bw * 0.5f, pb.y), c1, c0, c0, c1);
        dl->PopClipRect();
    }
}

namespace {

void Video(AppContext& ctx, ImVec2 va, ImVec2 vb, double pos, float hitBottom = -1.0f)
{
    EditorState& e = ctx.ui.editor;
    MediaPlayer& pl = ctx.player;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = e.fullscreen ? 0.0f : S(Tok::RadiusCard);
    dl->AddRectFilled(va, vb, IM_COL32(4, 0, 12, e.fullscreen ? 255 : 210), r);
    if (!e.fullscreen) dl->AddRect(va, vb, White(Tok::GlassBorderSoft), r);
    if (pl.HasFrame() && pl.VideoWidth() > 0) {
        const float aw = vb.x - va.x, ah = vb.y - va.y;
        const float k = std::min(aw / pl.VideoWidth(), ah / pl.VideoHeight());
        const ImVec2 sz(pl.VideoWidth() * k, pl.VideoHeight() * k);
        const ImVec2 ia((va.x + vb.x - sz.x) * 0.5f, (va.y + vb.y - sz.y) * 0.5f);
        dl->AddImageRounded((ImTextureID)pl.Texture(), ia, ImVec2(ia.x + sz.x, ia.y + sz.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                            e.fullscreen ? 0.0f : S(8.0f));
        // Look preview: filter tint, fades, and the crop window.
        {
            const ImVec2 ib(ia.x + sz.x, ia.y + sz.y);
            static const ImU32 tints[] = { 0, IM_COL32(255, 60, 160, 22), IM_COL32(255, 150, 40, 40), IM_COL32(40, 130, 255, 40), 0 };
            if (e.filter > 0 && e.filter < 4) dl->AddRectFilled(ia, ib, tints[e.filter]);
            if (e.filter == 4) dl->AddRectFilled(ia, ib, IM_COL32(110, 110, 110, 150));   // rough B&W preview (exact in export)
            const double ot = ToOutput(e, pos), total = OutputLength(e);
            float fade = 1.0f;
            if (e.fadeIn) fade = std::min(fade, (float)std::clamp(ot / 0.5, 0.0, 1.0));
            if (e.fadeOut) fade = std::min(fade, (float)std::clamp((total - ot) / 0.5, 0.0, 1.0));
            if (fade < 0.999f) dl->AddRectFilled(ia, ib, IM_COL32(0, 0, 0, (int)(255 * (1.0f - fade))));
            if (e.aspect != 0) {
                const float vw = (float)pl.VideoWidth(), vh = (float)pl.VideoHeight();
                float cw = e.aspect == 1 ? vh * 9.0f / 16.0f : vh, ch = vh;
                if (cw > vw) { cw = vw; if (e.aspect == 1) ch = std::min(vh, vw * 16.0f / 9.0f); }
                const float cx = ia.x + (vw - cw) * e.cropX * k, cy0 = ia.y + (vh - ch) * 0.5f * k;
                const ImVec2 ca(cx, cy0), cb(cx + cw * k, cy0 + ch * k);
                const ImU32 dim = IM_COL32(0, 0, 0, 170);
                dl->AddRectFilled(ia, ImVec2(ca.x, ib.y), dim);
                dl->AddRectFilled(ImVec2(cb.x, ia.y), ib, dim);
                dl->AddRectFilled(ImVec2(ca.x, ia.y), ImVec2(cb.x, ca.y), dim);
                dl->AddRectFilled(ImVec2(ca.x, cb.y), ImVec2(cb.x, ib.y), dim);
                dl->AddRect(ca, cb, Accent2(0.9f), 0.0f, 0, S(2.0f));
            }
        }
        // Captions and banner exactly where (and when) the export puts them.
        for (size_t i = 0; i < e.captions.size(); ++i) {
            const EditCaption& c = e.captions[i];
            if (pos < c.start || pos > c.end || SectionAt(e, pos) < 0) continue;
            const float a = (float)std::min({ 1.0, (pos - c.start) / 0.15, (c.end - pos) / 0.15 });
            CaptionPreview(dl, c, ia, sz, a);
        }
        if (e.banner) DrawBannerPreview(ctx, ia, k, pl.VideoWidth(), pl.VideoHeight(), ToOutput(e, pos));
    } else if (pl.HasError()) {
        Icon(dl, ICON_WARNING, ImVec2((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f - S(16.0f)), S(26.0f), Pal().warning);
        const std::string err = "Couldn't play this clip: " + pl.Error();
        dl->AddText(ImVec2((va.x + vb.x - ImGui::CalcTextSize(err.c_str()).x) * 0.5f, (va.y + vb.y) * 0.5f + S(10.0f)), Pal().textDim, err.c_str());
    } else {
        Spinner(dl, ImVec2((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f), S(14.0f), Accent2());
    }
    // The click area stops above any controls drawn over the video, so they keep their hover/clicks.
    const float hitB = hitBottom > va.y ? std::min(hitBottom, vb.y) : vb.y;
    ImGui::SetCursorScreenPos(va);
    if (ImGui::InvisibleButton("##video", ImVec2(vb.x - va.x, std::max(1.0f, hitB - va.y)))) pl.TogglePlay();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        e.fullscreen = !e.fullscreen;
        pl.TogglePlay();   // undo the single click that came first
    }
    const float paused = Anim(ImGui::GetID("##pausedbadge"), pl.IsPlaying() ? 0.0f : 1.0f, Motion::Hover);
    if (paused > 0.01f && pl.HasFrame()) {
        const ImVec2 cc((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f);
        const float rr = S(34.0f) * (0.85f + 0.15f * paused);
        dl->AddCircleFilled(cc, rr, IM_COL32(16, 6, 36, (int)(150 * paused)));
        dl->AddCircle(cc, rr, White(0.35f * paused), 0, S(1.0f));
        Icon(dl, ICON_PLAY, ImVec2(cc.x + S(2.0f), cc.y), S(22.0f), White(paused));
    }
}

void Controls(AppContext& ctx, ImVec2 ca, ImVec2 cb, double pos, double dur)
{
    EditorState& e = ctx.ui.editor;
    Settings& s = ctx.settings;
    MediaPlayer& pl = ctx.player;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bs = S(Tok::ControlHeight), cy = (ca.y + cb.y) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(ca.x, cy - bs * 0.5f));
    if (Button("##play", IconOpts(pl.IsPlaying() ? ICON_PAUSE : ICON_PLAY, pl.IsPlaying() ? "Pause (Space)" : "Play (Space)", bs, Btn::Primary)))
        pl.TogglePlay();
    PushFont(GetFonts().medium, Type::Body);
    const std::string t = FormatDuration(pos) + " / " + FormatDuration(dur);
    dl->AddText(ImVec2(ca.x + bs + S(14.0f), cy - ImGui::GetFontSize() * 0.5f), Pal().text, t.c_str());
    PopFont();

    // Right: volume, fullscreen.
    float rx = cb.x - bs;
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - bs * 0.5f));
    if (Button("##full", IconOpts(e.fullscreen ? ICON_BACK_TO_WINDOW : ICON_FULLSCREEN, e.fullscreen ? "Exit fullscreen (Esc)" : "Fullscreen", bs)))
        e.fullscreen = !e.fullscreen;
    const float vw = S(96.0f);
    rx -= vw + S(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - S(Tok::ControlHeightSmall) * 0.5f));
    if (Slider("##vol", &s.editor.volume, 0.0f, 1.0f, nullptr, vw)) {
        pl.SetVolume(std::min(1.0f, s.editor.volume * e.gain));
        ctx.Changed();
    }
    rx -= bs + S(2.0f);
    ImGui::SetCursorScreenPos(ImVec2(rx, cy - bs * 0.5f));
    const bool muted = s.editor.volume <= 0.001f;
    static float lastVolume = 0.8f;
    if (Button("##mute", IconOpts(muted ? ICON_MUTE : ICON_VOLUME, muted ? "Unmute" : "Mute", bs))) {
        if (!muted) {
            lastVolume = s.editor.volume;
            s.editor.volume = 0.0f;
        } else {
            s.editor.volume = lastVolume > 0.0f ? lastVolume : 0.8f;
        }
        pl.SetVolume(std::min(1.0f, s.editor.volume * e.gain));
        ctx.Changed();
    }
}
// Section tools, or the caption editor while a caption is selected.
void LookPopup(AppContext& ctx)
{
    EditorState& e = ctx.ui.editor;
    if (!BeginMenu("##look")) return;
    const float w = S(300.0f);
    MenuLabel("Shape");
    static const char* aspects[] = { "Original", "9:16", "1:1" };
    Segmented("##aspect", &e.aspect, aspects, 3, w);
    Tooltip("9:16 is for TikTok / Shorts / Reels");
    if (e.aspect != 0) {
        char t[32];
        snprintf(t, sizeof(t), "%s", e.cropX < 0.34f ? "Left" : e.cropX > 0.66f ? "Right" : "Centre");
        Slider("##cropx", &e.cropX, 0.0f, 1.0f, t, w);
    }
    MenuLabel("Filter");
    static const char* filters[] = { "None", "Vivid", "Warm", "Cool", "B&W" };
    Segmented("##filter", &e.filter, filters, 5, w);
    MenuLabel("Clip volume");
    char vt[32];
    snprintf(vt, sizeof(vt), "%d%%", (int)std::lround(e.gain * 100.0f));
    if (Slider("##gain", &e.gain, 0.0f, 2.0f, vt, w, 0.05f))
        ctx.player.SetVolume(std::min(1.0f, ctx.settings.editor.volume * e.gain));
    MenuLabel("Fades");
    Toggle("##fin", &e.fadeIn);
    ImGui::SameLine();
    Text("Fade in", Pal().text);
    ImGui::SameLine(0, S(24.0f));
    Toggle("##fout", &e.fadeOut);
    ImGui::SameLine();
    Text("Fade out", Pal().text);
    MenuSeparator();
    if (MenuItem(ICON_UNDO, "Reset look")) {
        e.aspect = 0; e.cropX = 0.5f; e.filter = 0; e.gain = 1.0f; e.fadeIn = e.fadeOut = false;
        ctx.player.SetVolume(ctx.settings.editor.volume);
    }
    EndMenu();
}

void Tools(AppContext& ctx, ImVec2 a, ImVec2 b, double pos, double dur)
{
    EditorState& e = ctx.ui.editor;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = S(34.0f), cy = (a.y + b.y) * 0.5f, gap = S(8.0f);
    ImGui::SetCursorScreenPos(ImVec2(a.x, cy - h * 0.5f));
    if (e.selCaption >= 0) {
        EditCaption& c = e.captions[(size_t)e.selCaption];
        Icon(dl, ICON_RENAME, ImVec2(a.x + S(10.0f), cy), S(15.0f), Accent2());
        ImGui::SetCursorScreenPos(ImVec2(a.x + S(26.0f), cy - S(Tok::ControlHeight) * 0.5f));
        const float fieldW = std::clamp((b.x - a.x) * 0.34f, S(160.0f), S(360.0f));
        TextField("##captext", c.text, sizeof(c.text), "Caption text", fieldW, 0, nullptr);
        ImGui::SameLine(0, gap);
        static const char* sizes[] = { "S", "M", "L" };
        Segmented("##capsize", &c.size, sizes, 3, S(120.0f));
        Tooltip("Text size");
        ImGui::SameLine(0, gap);
        static const char* places[] = { "Top", "Middle", "Bottom" };
        Segmented("##cappos", &c.position, places, 3, S(210.0f));
        ImGui::SameLine(0, gap);
        if (Button("##capdel", IconOpts(ICON_DELETE, "Delete caption (Del)", S(Tok::ControlHeight)))) DeleteSelected(ctx);
        ImGui::SameLine(0, gap);
        ButtonOpts done;
        done.style = Btn::Secondary;
        done.icon = ICON_CHECK;
        if (Button("Done##capdone", done)) e.selCaption = -1;
        return;
    }
    ButtonOpts split;
    split.icon = ICON_CUT;
    split.tooltip = "Cut the clip in two at the playhead (S)";
    if (Button("Split", split)) Split(e, pos);
    ImGui::SameLine(0, gap);
    ButtonOpts del;
    del.icon = ICON_DELETE;
    del.tooltip = "Remove the selected section (Del)";
    del.enabled = e.selSection >= 0;
    if (Button("Delete", del)) DeleteSelected(ctx);
    ImGui::SameLine(0, gap * 2.0f);
    // Speed of the selected section.
    int si = 2;
    if (e.selSection >= 0)
        for (int i = 0; i < 5; ++i)
            if (std::fabs(e.sections[(size_t)e.selSection].speed - kSpeeds[i]) < 0.01f) si = i;
    ImGui::BeginDisabled(e.selSection < 0);
    if (Segmented("##speed", &si, kSpeedNames, 5, S(300.0f)) && e.selSection >= 0) {
        PushUndo(e);
        e.sections[(size_t)e.selSection].speed = kSpeeds[si];
    }
    ImGui::EndDisabled();
    Tooltip(e.selSection < 0 ? "Click a section on the timeline to change its speed" : "Speed of the selected section");
    ImGui::SameLine(0, gap * 2.0f);
    ButtonOpts text;
    text.icon = ICON_RENAME;
    text.tooltip = "Add text at the playhead (T)";
    if (Button("Add text", text)) AddCaption(e, pos, dur);
    ImGui::SameLine(0, gap);
    {
        const bool changed = e.aspect != 0 || e.filter != 0 || e.gain != 1.0f || e.fadeIn || e.fadeOut;
        ButtonOpts look;
        look.icon = ICON_COLOR;
        look.style = changed ? Btn::Secondary : Btn::Ghost;
        look.tooltip = "Crop, filters, volume and fades (L)";
        if (Button(changed ? "Look *" : "Look", look) || (ImGui::IsKeyPressed(ImGuiKey_L) && !ImGui::GetIO().WantTextInput)) ImGui::OpenPopup("##look");
        LookPopup(ctx);
    }
    ImGui::SameLine(0, gap);
    ButtonOpts undo;
    undo.style = Btn::Ghost;
    undo.icon = ICON_UNDO;
    undo.enabled = !e.undo.empty();
    undo.tooltip = "Undo (Ctrl+Z)";
    if (Button("##undo", undo)) Undo(e);

    // Output length on the right.
    const std::string total = FormatDuration(OutputLength(e)) + " total";
    PushFont(GetFonts().semibold, Type::Caption);
    const ImVec2 ts = ImGui::CalcTextSize(total.c_str());
    const ImVec2 pa(b.x - ts.x - S(20.0f), cy - ts.y * 0.5f - S(4.0f)), pb(b.x, cy + ts.y * 0.5f + S(4.0f));
    const bool trimmed = e.sections.size() > 1 || std::fabs(OutputLength(e) - dur) > 0.05;
    if (trimmed) GradientRectH(dl, pa, pb, Accent(0.9f), Accent2(0.9f), (pb.y - pa.y) * 0.5f);
    else dl->AddRectFilled(pa, pb, White(0.10f), (pb.y - pa.y) * 0.5f);
    dl->AddText(ImVec2(pa.x + S(10.0f), cy - ts.y * 0.5f), White(1.0f), total.c_str());
    PopFont();
}

// Filmstrip with sections (kept parts), cuts (dimmed), playhead and edge handles.
void Timeline(AppContext& ctx, ImVec2 a, ImVec2 b, ImVec2 la, ImVec2 lb, double dur, double pos)
{
    EditorState& e = ctx.ui.editor;
    MediaPlayer& pl = ctx.player;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(10.0f), w = b.x - a.x;
    const StripInfo strip = ctx.strips.Get(e.clip.path, e.clip.modified);
    dl->AddRectFilled(a, b, IM_COL32(10, 2, 30, 170), r);
    if (strip.texture && strip.frames > 0) {
        const int n = strip.frames;
        const float slot = w / n, slotAspect = slot / (b.y - a.y), cell = 16.0f / 9.0f;
        for (int i = 0; i < n; ++i) {
            float u0 = (float)i / n, u1 = (float)(i + 1) / n, v0 = 0.0f, v1 = 1.0f;
            if (slotAspect < cell) {
                const float k = (1.0f - slotAspect / cell) * 0.5f / n;
                u0 += k;
                u1 -= k;
            } else {
                const float k = (1.0f - cell / slotAspect) * 0.5f;
                v0 += k;
                v1 -= k;
            }
            const ImDrawFlags f = i == 0 ? ImDrawFlags_RoundCornersLeft : (i == n - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
            dl->AddImageRounded((ImTextureID)strip.texture, ImVec2(a.x + slot * i, a.y), ImVec2(a.x + slot * (i + 1), b.y), ImVec2(u0, v0),
                                ImVec2(u1, v1), IM_COL32_WHITE, r, f);
        }
    } else if (!strip.failed) {
        Skeleton(dl, a, b, r);
    }
    dl->AddRectFilled(la, lb, IM_COL32(10, 2, 30, 120), S(6.0f));
    if (dur <= 0.0 || e.sections.empty()) {
        ImGui::SetCursorScreenPos(a);
        ImGui::Dummy(ImVec2(w, lb.y - a.y));
        return;
    }
    auto xOf = [&](double t) { return a.x + (float)(t / dur) * w; };
    auto tOf = [&](float x) { return Clamp((double)((x - a.x) / w) * dur, 0.0, dur); };
    const float playX = xOf(pos);
    auto snap = [&](float x) { return std::fabs(x - playX) < S(kSnapPx) ? pos : tOf(x); };

    // Cuts: dim everything outside the sections, with a diagonal hatch so "removed" reads clearly.
    {
        double prev = 0.0;
        for (size_t i = 0; i <= e.sections.size(); ++i) {
            const double gs = prev, ge = i < e.sections.size() ? e.sections[i].start : dur;
            if (ge - gs > 1e-3) {
                const ImVec2 ga(xOf(gs), a.y), gb(xOf(ge), b.y);
                const ImDrawFlags f = (gs <= 0.0 ? ImDrawFlags_RoundCornersLeft : 0) | (ge >= dur ? ImDrawFlags_RoundCornersRight : 0);
                dl->AddRectFilled(ga, gb, IM_COL32(8, 0, 20, 200), f ? r : 0.0f, f ? f : ImDrawFlags_RoundCornersNone);
                dl->PushClipRect(ga, gb, true);
                for (float x = ga.x - (b.y - a.y); x < gb.x; x += S(9.0f)) dl->AddLine(ImVec2(x, b.y), ImVec2(x + (b.y - a.y), a.y), White(0.07f), 1.0f);
                dl->PopClipRect();
            }
            if (i < e.sections.size()) prev = e.sections[i].end;
        }
    }

    // Sections: outline, speed badge, and handles on both edges.
    const float hw = S(11.0f), hh = b.y - a.y + S(8.0f);
    for (size_t i = 0; i < e.sections.size(); ++i) {
        EditSection& s = e.sections[i];
        const float x0 = xOf(s.start), x1 = xOf(s.end);
        const bool sel = (int)i == e.selSection;
        const float sv = Anim(AnimId(ImGui::GetID("##secsel"), (unsigned)i), sel ? 1.0f : 0.0f, Motion::Hover);
        if (sv > 0.01f) {
            dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), Accent2(0.10f * sv), S(4.0f));
            for (int g = 1; g <= 3; ++g)
                dl->AddRect(ImVec2(x0 - g * S(1.5f), a.y - g * S(1.5f)), ImVec2(x1 + g * S(1.5f), b.y + g * S(1.5f)), Accent2(0.16f * sv / g), S(5.0f), 0, S(1.5f));
        }
        dl->AddRect(ImVec2(x0, a.y), ImVec2(x1, b.y), Mix(Accent(0.9f), White(1.0f), 0.4f * sv), S(4.0f), 0, S(2.0f) + S(1.0f) * sv);
        if (std::fabs(s.speed - 1.0f) > 0.01f && x1 - x0 > S(36.0f)) {
            char sp[16];
            snprintf(sp, sizeof(sp), "%g\xC3\x97", s.speed);
            const ImVec2 bs = BadgeSize(sp);
            Badge(dl, ImVec2(x0 + S(6.0f), a.y + S(5.0f)), sp, s.speed < 1.0f ? IM_COL32(40, 120, 255, 220) : IM_COL32(255, 120, 40, 220), White(1.0f));
            (void)bs;
        }
        for (int k = 0; k < 2; ++k) {
            const float hx = k == 0 ? x0 : x1;
            const ImVec2 ha(k == 0 ? hx - hw : hx, a.y - S(4.0f)), hb(ha.x + hw, ha.y + hh);
            // Hit area: wider than the drawn handle, mostly outward so the two handles of a short section don't fight.
            const float grabOut = S(10.0f), grabIn = S(4.0f);
            const float gx0 = k == 0 ? ha.x - grabOut : ha.x - grabIn, gx1 = k == 0 ? hb.x + grabIn : hb.x + grabOut;
            ImGui::SetCursorScreenPos(ImVec2(gx0, ha.y - S(4.0f)));
            ImGui::PushID((int)(i * 2 + k));
            ImGui::InvisibleButton("##edge", ImVec2(gx1 - gx0, hh + S(8.0f)));
            const ImGuiID hid = ImGui::GetItemID();
            const bool active = ImGui::IsItemActive(), hv = ImGui::IsItemHovered() || active;
            if (hv) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            Tooltip(k == 0 ? "Drag to trim the start" : "Drag to trim the end");
            if (ImGui::IsItemActivated()) {
                PushUndo(e);
                e.wasPlaying = pl.IsPlaying();
                pl.Pause();
                e.dragging = k;
                e.dragIndex = (int)i;
                e.dragGrab = ImGui::GetIO().MousePos.x - hx;   // keep the grab offset: no jump on click
                e.selSection = (int)i;
                e.selCaption = -1;
            }
            if (active) {
                // Can't cross the neighbours or get shorter than half a second.
                const double lo = i > 0 ? e.sections[i - 1].end : 0.0, hi = i + 1 < e.sections.size() ? e.sections[i + 1].start : dur;
                // No playhead snap here: the playhead follows the handle, so snapping to it made the handle stick.
                const double t = tOf(ImGui::GetIO().MousePos.x - e.dragGrab);
                if (k == 0) s.start = Clamp(t, lo, std::max(lo, s.end - kMinSection));
                else s.end = Clamp(t, std::min(hi, s.start + kMinSection), hi);
                pl.Seek(k == 0 ? s.start : s.end, true);
                Sync(e);
            }
            if (ImGui::IsItemDeactivated()) {
                pl.Seek(k == 0 ? s.start : std::max(s.start, s.end - 0.05), false);
                if (e.wasPlaying) pl.Play();
                e.dragging = -1;
            }
            const float hvA = AnimHover(hid, hv);
            ImGui::PopID();
            const ImDrawFlags f = k == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight;
            if (hvA > 0.01f) dl->AddRectFilled(ImVec2(ha.x - S(3.0f), ha.y - S(3.0f)), ImVec2(hb.x + S(3.0f), hb.y + S(3.0f)), Accent2(0.25f * hvA), S(8.0f), f);
            GradientRect(dl, ha, hb, Mix(Accent(), White(1.0f), 0.2f * hvA), Accent2(), S(6.0f), f);
            const float gx = ha.x + hw * 0.5f;
            for (int g = -1; g <= 1; g += 2)
                dl->AddLine(ImVec2(gx + g * S(1.4f), ha.y + hh * 0.36f), ImVec2(gx + g * S(1.4f), hb.y - hh * 0.36f), White(0.85f), S(1.1f));
        }
    }

    // Registered after the handles so they get the mouse first (overlapping items: the first one wins).
    // Scrub surface: click selects the section under the mouse and seeks; drag scrubs.
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##scrub", ImVec2(w, b.y - a.y));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActivated()) {
        e.wasPlaying = pl.IsPlaying();
        pl.Pause();
        e.dragging = 2;
        const int k = SectionAt(e, tOf(ImGui::GetIO().MousePos.x));
        e.selSection = k;
        e.selCaption = -1;
    }
    if (ImGui::IsItemActive()) pl.Seek(tOf(ImGui::GetIO().MousePos.x), true);
    if (ImGui::IsItemDeactivated()) {
        double t = tOf(ImGui::GetIO().MousePos.x);
        if (SectionAt(e, t) < 0)   // released in a cut: jump to the next kept part
            for (const EditSection& s : e.sections)
                if (s.start >= t) {
                    t = s.start;
                    break;
                }
        pl.Seek(t, false);
        if (e.wasPlaying) pl.Play();
        e.dragging = -1;
    }

    // Caption lane.
    for (size_t i = 0; i < e.captions.size(); ++i) {
        EditCaption& c = e.captions[i];
        const float x0 = xOf(c.start), x1 = std::max(x0 + S(14.0f), xOf(c.end));
        const ImVec2 ca(x0, la.y + S(2.0f)), cb(x1, lb.y - S(2.0f));
        ImGui::SetCursorScreenPos(ca);
        ImGui::PushID((int)(1000 + i));
        ImGui::InvisibleButton("##cap", ImVec2(cb.x - ca.x, cb.y - ca.y));
        const bool hv = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        const float edge = S(7.0f);
        const float mx = ImGui::GetIO().MousePos.x;
        if (hv) ImGui::SetMouseCursor(mx < ca.x + edge || mx > cb.x - edge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeAll);
        if (ImGui::IsItemActivated()) {
            PushUndo(e);
            e.selCaption = (int)i;
            e.selSection = -1;
            e.dragging = 3;
            e.dragIndex = mx < ca.x + edge ? 0 : (mx > cb.x - edge ? 1 : 2);
            e.dragGrab = tOf(mx) - c.start;
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            const double t = snap(mx);
            if (e.dragIndex == 0) c.start = Clamp(t, 0.0, c.end - 0.3);
            else if (e.dragIndex == 1) c.end = Clamp(t, c.start + 0.3, dur);
            else {
                const double len = c.end - c.start;
                c.start = Clamp(tOf(mx) - e.dragGrab, 0.0, dur - len);
                c.end = c.start + len;
            }
        }
        if (ImGui::IsItemDeactivated()) e.dragging = -1;
        ImGui::PopID();
        const bool sel = (int)i == e.selCaption;
        GradientRectH(dl, ca, cb, sel ? Accent(0.95f) : IM_COL32(120, 90, 200, 150), sel ? Accent2(0.95f) : IM_COL32(150, 80, 200, 150), S(5.0f));
        dl->AddRect(ca, cb, White(sel ? 0.6f : 0.2f + (hv ? 0.2f : 0.0f)), S(5.0f));
        PushFont(GetFonts().medium, Type::Caption);
        dl->PushClipRect(ca, cb, true);
        dl->AddText(ImVec2(ca.x + S(6.0f), (ca.y + cb.y - ImGui::GetFontSize()) * 0.5f), White(0.95f), c.text[0] ? c.text : "(empty)");
        dl->PopClipRect();
        PopFont();
    }
    if (e.captions.empty()) {
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(la.x + S(10.0f), (la.y + lb.y - ImGui::GetFontSize()) * 0.5f), Pal().textFaint, "Text lane: press T or \"Add text\" to put words on the video");
        PopFont();
    }

    // Playhead.
    dl->AddLine(ImVec2(playX, a.y - S(6.0f)), ImVec2(playX, lb.y), White(1.0f), S(2.0f));
    dl->AddCircleFilled(ImVec2(playX, a.y - S(7.0f)), S(5.0f), White(1.0f));

    // Hover time.
    if (ImGui::IsMouseHoveringRect(a, b) && e.dragging < 0) {
        const std::string t = FormatDuration(tOf(ImGui::GetIO().MousePos.x));
        PushFont(GetFonts().medium, Type::Caption);
        const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        const float tx = Clamp(ImGui::GetIO().MousePos.x - ts.x * 0.5f, a.x, b.x - ts.x);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 ba(tx - S(6.0f), a.y - ts.y - S(16.0f)), bb(tx + ts.x + S(6.0f), a.y - S(10.0f));
        fg->AddRectFilled(ba, bb, Pal().surface, S(6.0f));
        fg->AddRect(ba, bb, White(0.2f), S(6.0f));
        fg->AddText(ImVec2(tx, ba.y + S(3.0f)), Pal().text, t.c_str());
        PopFont();
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x, lb.y));
    ImGui::Dummy(ImVec2(0, 0));
}

engine::ExportRequest BuildExport(AppContext& ctx)
{
    EditorState& e = ctx.ui.editor;
    engine::ExportRequest req;
    req.source = e.clip.path;
    req.start = e.in;
    req.end = e.out;
    for (const EditSection& s : e.sections) req.segments.push_back(engine::Segment{ e.clip.path, s.start, s.end, (double)s.speed });
    // Captions are kept in source time; the export needs them in output time, split around cuts.
    for (const EditCaption& c : e.captions) {
        if (!c.text[0]) continue;
        double out = 0.0;
        for (const EditSection& s : e.sections) {
            const double a = std::max(c.start, s.start), b = std::min(c.end, s.end);
            if (b - a > 0.05) {
                engine::Caption ec;
                ec.text = ToWide(c.text);
                ec.start = out + (a - s.start) / s.speed;
                ec.end = out + (b - s.start) / s.speed;
                ec.y = kCaptionY[Clamp(c.position, 0, 2)];
                ec.size = c.size;
                req.captions.push_back(ec);
            }
            out += (s.end - s.start) / s.speed;
        }
    }
    req.quality = e.quality;
    req.targetMB = kSizeLimits[Clamp(e.sizeLimit, 0, 4)];
    req.banner = e.banner;
    req.bannerStyle = BannerStyleFor(ctx.settings);
    req.hardware = ctx.settings.video.encoder != "x264";
    req.look.aspect = e.aspect;
    req.look.cropX = e.cropX;
    req.look.filter = e.filter;
    req.look.volume = e.gain;
    req.look.fadeIn = e.fadeIn;
    req.look.fadeOut = e.fadeOut;
    return req;
}

void ExportPopup(AppContext& ctx)
{
    EditorState& e = ctx.ui.editor;
    Settings& s = ctx.settings;
    if (e.openExport) {
        ImGui::OpenPopup("##export");
        e.openExport = false;
        e.exportError.clear();
        e.exportFinished = false;
    }
    if (!BeginModal("##export", "Export clip", S(460.0f))) return;
    if (e.exportFinished) {
        e.exportFinished = false;
        ImGui::CloseCurrentPopup();
    }
    const float W = ImGui::GetContentRegionAvail().x;
    const bool busy = e.exporting;
    ImGui::BeginDisabled(busy);

    Text("Quality", Pal().textDim, GetFonts().medium, Type::Caption);
    static const char* kQualities[] = { "Original", "High", "Small file" };
    if (Segmented("##quality", &e.quality, kQualities, 3, W)) {
        s.editor.exportQuality = e.quality;
        ctx.Changed();
    }
    Gap(2.0f);
    Text("Fit under a size", Pal().textDim, GetFonts().medium, Type::Caption);
    static const char* kSizes[] = { "No limit", "10 MB (Discord)", "25 MB", "50 MB", "100 MB" };
    Dropdown("##sizelimit", &e.sizeLimit, kSizes, 5, W);
    const double len = OutputLength(e);
    char hint[160];
    if (e.sizeLimit > 0)
        snprintf(hint, sizeof(hint), "%s long. Replayr picks the best quality that fits, lowering the resolution if it has to.", FormatDuration(len).c_str());
    else snprintf(hint, sizeof(hint), "%s long.", FormatDuration(len).c_str());
    TextWrapped(hint, Pal().textFaint, W, nullptr, Type::Caption);
    Gap(4.0f);

    const float rowY = ImGui::GetCursorPosY();
    Text("Show Replayr banner", Pal().text, GetFonts().medium, Type::Body);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + W - S(44.0f), rowY - S(3.0f)));
    Toggle("##banner", &e.banner);
    const bool simple = e.sections.size() == 1 && e.sections[0].speed == 1.0f && e.captions.empty() && e.sizeLimit == 0 && e.quality == 0 && !e.banner;
    TextWrapped(simple ? "Saved straight from the recording: no quality loss, near instant." : "Re-encodes on your GPU, so it takes a few seconds.",
                Pal().textFaint, W, nullptr, Type::Caption);
    Gap(8.0f);
    const bool enter = TextField("##exname", e.exportName, sizeof(e.exportName), "File name", W, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();

    if (busy) {
        Gap(4.0f);
        char pr[48];
        snprintf(pr, sizeof(pr), "Exporting\xE2\x80\xA6 %d%%", (int)std::lround(e.exportProgress * 100.0f));
        Text(pr, Pal().text, GetFonts().medium, Type::Body);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ProgressBar(ImGui::GetWindowDrawList(), p, ImVec2(p.x + W, p.y + S(6.0f)), e.exportProgress);
        ImGui::Dummy(ImVec2(W, S(6.0f)));
    }
    if (!e.exportError.empty()) {
        Gap(2.0f);
        TextWrapped(e.exportError.c_str(), Pal().danger, W, GetFonts().medium, Type::Caption);
    }
    Gap(8.0f);
    const float bw = (W - S(10.0f)) * 0.5f, bh = S(40.0f);
    ButtonOpts cancel;
    cancel.size = ImVec2(bw, bh);
    if (Button(busy ? "Stop" : "Cancel", cancel)) {
        if (busy) ctx.engine.CancelExport(e.exportJob);
        else ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, S(10.0f));
    ButtonOpts save;
    save.style = Btn::Primary;
    save.icon = ICON_DOWNLOAD;
    save.size = ImVec2(bw, bh);
    save.enabled = !busy && e.exportName[0] && len > 0.05;
    if (Button("Save", save) || (enter && save.enabled)) {
        engine::ExportRequest req = BuildExport(ctx);
        req.destination = UniquePath(JoinPath(ParentDir(e.clip.path), SanitizeFileName(ToWide(e.exportName)) + L".mp4"));
        e.exportError.clear();
        e.exportProgress = 0.0f;
        e.exporting = true;
        e.exportJob = ctx.engine.Export(req);
    }
    EndModal();
}

void Keyboard(AppContext& ctx, double pos, double dur)
{
    EditorState& e = ctx.ui.editor;
    MediaPlayer& pl = ctx.player;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) pl.TogglePlay();
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) pl.Seek(std::max(0.0, pos - 1.0));
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pl.Seek(std::min(dur, pos + 1.0));
    if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl) Split(e, pos);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) AddCaption(e, pos, dur);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) DeleteSelected(ctx);
    const int k = SectionAt(e, pos);
    if (ImGui::IsKeyPressed(ImGuiKey_I, false) && k >= 0 && e.sections[(size_t)k].end - pos >= kMinSection) {
        PushUndo(e);
        e.sections[(size_t)k].start = pos;
        Sync(e);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_O, false) && k >= 0 && pos - e.sections[(size_t)k].start >= kMinSection) {
        PushUndo(e);
        e.sections[(size_t)k].end = pos;
        Sync(e);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo(e);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (e.fullscreen) e.fullscreen = false;
        else if (e.selCaption >= 0 || e.selSection >= 0) e.selCaption = e.selSection = -1;
        else LeaveEditor(ctx);
    }
}

// Playback skips cuts and plays each section at its speed.
void FollowSections(AppContext& ctx, double pos)
{
    EditorState& e = ctx.ui.editor;
    MediaPlayer& pl = ctx.player;
    static float lastRate = 1.0f;
    const int k = SectionAt(e, pos);
    const float rate = k >= 0 ? e.sections[(size_t)k].speed : 1.0f;
    if (rate != lastRate) {
        pl.SetRate(rate);
        lastRate = rate;
    }
    if (!pl.IsPlaying() || e.dragging >= 0 || e.sections.empty()) return;
    if (k >= 0 && pos < e.sections[(size_t)k].end - 0.03) return;
    for (const EditSection& s : e.sections)
        if (s.start > pos + 0.01) {
            pl.Seek(s.start);
            return;
        }
    if (ctx.settings.editor.loopSelection) pl.Seek(e.sections.front().start);
    else pl.Pause();
}

} // namespace

void DrawEditor(AppContext& ctx)
{
    EditorState& e = ctx.ui.editor;
    Settings& s = ctx.settings;
    MediaPlayer& pl = ctx.player;
    if (!e.hasClip) {
        GoTo(ctx, Page::Clips);
        return;
    }
    const ClipInfo& c = e.clip;
    if (!e.holdPlayer && pl.Path() != c.path) {
        pl.Open(c.path);
        pl.SetVolume(std::min(1.0f, s.editor.volume * e.gain));
        pl.SetRate(1.0);
    }
    const double dur = ClipDuration(ctx);
    if (!e.rangeSet && dur > 0 && pl.IsReady()) {
        e.aspect = 0; e.cropX = 0.5f; e.filter = 0; e.gain = 1.0f; e.fadeIn = e.fadeOut = false;
        e.sections = { EditSection{ 0.0, dur, 1.0f } };
        e.rangeSet = true;
        Sync(e);
        pl.Play();
    }
    if (e.rangeSet && !e.sections.empty() && e.sections.back().end > dur) {
        e.sections.back().end = dur;
        Sync(e);
    }
    const double pos = pl.Position();
    FollowSections(ctx, pos);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 a = p, b(p.x + avail.x, p.y + avail.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float controlsH = S(46.0f), toolsH = S(48.0f), stripH = S(56.0f), laneH = S(26.0f);
    const float bottomH = controlsH + toolsH + stripH + laneH + S(26.0f);
    auto bottom = [&](float x0, float x1, float y0) {
        Controls(ctx, ImVec2(x0, y0), ImVec2(x1, y0 + controlsH), pos, dur);
        Tools(ctx, ImVec2(x0, y0 + controlsH), ImVec2(x1, y0 + controlsH + toolsH), pos, dur);
        const float sy = y0 + controlsH + toolsH + S(10.0f);
        Timeline(ctx, ImVec2(x0 + S(12.0f), sy), ImVec2(x1 - S(12.0f), sy + stripH), ImVec2(x0 + S(12.0f), sy + stripH + S(6.0f)),
                 ImVec2(x1 - S(12.0f), sy + stripH + S(6.0f) + laneH), dur, pos);
    };

    if (e.fullscreen) {
        if (ImGui::GetIO().MouseDelta.x != 0.0f || ImGui::GetIO().MouseDelta.y != 0.0f) e.lastMouseMove = ImGui::GetTime();
        const bool show = ImGui::GetTime() - e.lastMouseMove < 2.5 || !pl.IsPlaying() || e.dragging >= 0;
        const float vis = Anim(ImGui::GetID("##fsctrl"), show ? 1.0f : 0.0f, Motion::Page);
        if (vis < 0.02f) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        const float ctrlTop = b.y - bottomH - S(4.0f);
        Video(ctx, a, b, pos, vis > 0.01f ? ctrlTop - S(8.0f) : -1.0f);
        if (vis > 0.01f) {
            // Top bar: title on the left, exit button on the right.
            GradientRect(dl, a, ImVec2(b.x, a.y + S(90.0f)), IM_COL32(4, 0, 12, (int)(200 * vis)), IM_COL32(4, 0, 12, 0), 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, vis);
            PushFont(GetFonts().semibold, Type::Heading);
            dl->AddText(ImVec2(a.x + S(24.0f), a.y + S(22.0f)), White(0.92f * vis), e.title);
            PopFont();
            ButtonOpts xo = IconOpts(ICON_BACK_TO_WINDOW, "Exit fullscreen (Esc)", S(38.0f));
            ImGui::SetCursorScreenPos(ImVec2(b.x - S(24.0f) - S(38.0f), a.y + S(16.0f)));
            if (Button("##fsexit", xo)) e.fullscreen = false;
            ImGui::PopStyleVar();
        }
        if (vis > 0.01f) {
            GradientRect(dl, ImVec2(a.x, b.y - bottomH - S(60.0f)), b, IM_COL32(4, 0, 12, 0), IM_COL32(4, 0, 12, (int)(230 * vis)), 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, vis);
            bottom(a.x + S(24.0f), b.x - S(24.0f), ctrlTop);
            ImGui::PopStyleVar();
        }
    } else {
        const float rowH = S(44.0f);
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + (rowH - S(Tok::ControlHeight)) * 0.5f));
        ButtonOpts back;
        back.style = Btn::Ghost;
        back.icon = ICON_CHEVRON_LEFT;
        back.tooltip = "Back to clips (Esc)";
        if (Button("Clips", back)) {
            LeaveEditor(ctx);
            ImGui::Dummy(ImVec2(0, 0));
            return;
        }
        const float backW = ImGui::GetItemRectSize().x;
        ButtonOpts ex;
        ex.style = Btn::Primary;
        ex.icon = ICON_DOWNLOAD;
        ex.size = ImVec2(S(140.0f), S(42.0f));
        ex.enabled = e.rangeSet;
        ImGui::SetCursorScreenPos(ImVec2(b.x - ex.size.x, a.y + (rowH - ex.size.y) * 0.5f));
        if (Button("Export", ex)) {
            if (!e.exportName[0]) snprintf(e.exportName, sizeof(e.exportName), "%s (edit)", e.title);
            e.openExport = true;
        }
        const float tx = a.x + backW + S(14.0f);
        const float tw = std::min(S(520.0f), b.x - ex.size.x - S(20.0f) - tx);
        ImGui::SetCursorScreenPos(ImVec2(tx, a.y + (rowH - S(Tok::ControlHeight)) * 0.5f));
        PushFont(GetFonts().semibold, Type::Heading);
        if (TextField("##title", e.title, sizeof(e.title), "Clip name", tw, ImGuiInputTextFlags_EnterReturnsTrue, ICON_RENAME)) {
            if (e.title[0] && c.title != e.title) {
                pl.Close();   // the player holds the file open
                e.holdPlayer = true;
                ctx.library.Rename(c.path, ToWide(e.title));
            }
        }
        PopFont();
        const bool editingTitle = ImGui::GetActiveID() == ImGui::GetID("##title");
        if (!editingTitle && !e.holdPlayer && c.title != e.title) snprintf(e.title, sizeof(e.title), "%s", c.title.c_str());

        const float top = a.y + rowH + S(14.0f);
        Video(ctx, ImVec2(a.x, top), ImVec2(b.x, b.y - bottomH), pos);
        bottom(a.x, b.x, b.y - bottomH + S(6.0f));
    }

    ExportPopup(ctx);
    Keyboard(ctx, pos, dur);
    ImGui::Dummy(ImVec2(0, 0));
}

} // namespace cs::ui
