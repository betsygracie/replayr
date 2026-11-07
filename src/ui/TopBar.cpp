// Top bar: search, recording indicator and window controls. (The logo sits in the sidebar header.)
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Util.h"
#include "core/Window.h"

#include <algorithm>
#include <cmath>

namespace cs::ui {

void DrawTopBar(AppContext& ctx, float height)
{
    UiState& ui = ctx.ui;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x;
    const ImVec2 a = p, b(p.x + W, p.y + height);
    const float r = S(18.0f);
    Glass(dl, a, b, r, 0.0f, Tok::GlassBorder);

    CaptionLayout& cap = ctx.window.Caption();
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    auto hit = [&](ImVec2 ra, ImVec2 rb) { cap.interactive.push_back(HitRect{ ra.x - origin.x, ra.y - origin.y, rb.x - origin.x, rb.y - origin.y }); };
    const float cy = p.y + height * 0.5f;
    const float ch = S(Tok::ControlHeight);

    // Right cluster, laid out right-to-left: window controls, then the recording pill.
    const float cw = S(40.0f);
    float x = b.x - S(8.0f) - cw * 3.0f;
    const char* capIcons[3] = { ICON_MINIMIZE, ctx.window.Maximized() ? ICON_RESTORE : ICON_MAXIMIZE, ICON_CLOSE };
    for (int i = 0; i < 3; ++i) {
        const ImVec2 ba(x + cw * i, cy - ch * 0.5f), bb(ba.x + cw, ba.y + ch);
        bool hovered, held;
        ImGui::PushID(i);
        if (i == 1) {
            // HTMAXBUTTON: Windows handles the click and shows Snap Layouts on hover.
            cap.maxButton = HitRect{ ba.x - origin.x, ba.y - origin.y, bb.x - origin.x, bb.y - origin.y };
            hovered = ImGui::IsMouseHoveringRect(ba, bb, false);
            held = ctx.window.IsMaxButtonPressed();
        } else {
            ImGui::SetCursorScreenPos(ba);
            if (ImGui::InvisibleButton("##cap", ImVec2(cw, ch))) ctx.Do(i == 0 ? Action::Minimize : Action::Close);
            hovered = ImGui::IsItemHovered();
            held = ImGui::IsItemActive();
            hit(ba, bb);
        }
        const float h = Anim(ImGui::GetID("##caphov"), hovered ? 1.0f : 0.0f, Motion::Hover);
        ImGui::PopID();
        const ImU32 fill = i == 2 ? ColA(Pal().danger, h * (held ? 0.8f : 1.0f)) : White(0.12f * h + (held ? 0.06f : 0.0f));
        if (h > 0.01f || held) dl->AddRectFilled(ba, bb, fill, S(Tok::RadiusControl));
        Icon(dl, capIcons[i], ImVec2((ba.x + bb.x) * 0.5f, cy), S(10.0f), Mix(Pal().textDim, White(1.0f), h));
    }
    x -= S(10.0f);
    dl->AddLine(ImVec2(x, cy - S(10.0f)), ImVec2(x, cy + S(10.0f)), White(0.16f));
    x -= S(10.0f);

    // "Restart to update" once a new version has been downloaded.
    if (ctx.updater && ctx.updater->State() == UpdateState::Ready) {
        ButtonOpts up;
        up.style = Btn::Primary;
        up.icon = ICON_DOWNLOAD;
        up.size = ImVec2(S(170.0f), S(32.0f));
        up.round = true;
        const std::string tip = ctx.updater->Message();
        up.tooltip = tip.c_str();
        const ImVec2 pa(x - up.size.x, cy - up.size.y * 0.5f);
        ImGui::SetCursorScreenPos(pa);
        if (Button("Restart to update", up)) ctx.Do(Action::ApplyUpdate);
        hit(pa, ImVec2(x, pa.y + up.size.y));
        x -= up.size.x + S(10.0f);
    }
    // Recording indicator (only while recording).
    const engine::EngineStatus& st = ctx.status;
    const float rec = Anim(ImGui::GetID("##recpill"), st.recording ? 1.0f : 0.0f, Motion::Slide);
    if (rec > 0.01f) {
        PushFont(GetFonts().medium, Type::Body);
        const std::string label = "REC  " + FormatDuration(st.recordingSeconds);
        const float pw = ImGui::CalcTextSize(label.c_str()).x + S(42.0f);
        const ImVec2 pa(x - pw, cy - S(15.0f)), pb(x, cy + S(15.0f));
        ImGui::SetCursorScreenPos(pa);
        if (ImGui::InvisibleButton("##recstop", ImVec2(pw, pb.y - pa.y))) ctx.Do(Action::ToggleRecording);
        const bool hv = ImGui::IsItemHovered();
        Tooltip("Stop recording");
        hit(pa, pb);
        dl->AddRectFilled(pa, pb, ColA(Pal().record, (hv ? 0.30f : 0.20f) * rec), S(15.0f));
        dl->AddRect(pa, pb, ColA(Pal().record, 0.55f * rec), S(15.0f));
        PulseDot(dl, ImVec2(pa.x + S(17.0f), cy), S(4.0f), ColA(Pal().record, rec));
        dl->AddText(ImVec2(pa.x + S(30.0f), cy - ImGui::GetFontSize() * 0.5f), White(rec), label.c_str());
        PopFont();
        x -= pw + S(10.0f);
    }

    // Replay buffer pill (while the buffer runs and nothing is being recorded): "● Buffer · Game · 2:00".
    {
        const bool bufOn = st.buffer == engine::BufferState::Filling || st.buffer == engine::BufferState::Ready;
        const float bv = Anim(ImGui::GetID("##bufpill"), bufOn && !st.recording ? 1.0f : 0.0f, Motion::Slide);
        if (bv > 0.01f && x - a.x > S(520.0f)) {
            const std::string& src = ctx.settings.video.source;
            const char* what = src == "game" ? "Game" : (src.rfind("both:", 0) == 0 ? "Game + Screen" : "Screen");
            const std::string label = std::string("Buffer  \xC2\xB7  ") + what + "  \xC2\xB7  " + FormatDuration(st.bufferTargetSeconds);
            PushFont(GetFonts().medium, Type::Caption);
            const float pw = ImGui::CalcTextSize(label.c_str()).x + S(36.0f);
            const ImVec2 pa(x - pw, cy - S(14.0f)), pb(x, cy + S(14.0f));
            ImGui::SetCursorScreenPos(pa);
            ImGui::InvisibleButton("##bufinfo", ImVec2(pw, pb.y - pa.y));
            const std::string tip = (st.buffer == engine::BufferState::Ready ? "Replay ready: " : "Filling the replay buffer: ") + st.sourceName;
            Tooltip(tip.c_str());
            hit(pa, pb);
            dl->AddRectFilled(pa, pb, White(0.07f * bv), S(14.0f));
            dl->AddRect(pa, pb, White(0.16f * bv), S(14.0f));
            const ImU32 dot = st.buffer == engine::BufferState::Ready ? ColA(Pal().success, bv) : ColA(Pal().warning, bv);
            PulseDot(dl, ImVec2(pa.x + S(15.0f), cy), S(3.5f), dot);
            dl->AddText(ImVec2(pa.x + S(26.0f), cy - ImGui::GetFontSize() * 0.5f), White(0.9f * bv), label.c_str());
            PopFont();
            x -= pw + S(10.0f);
        }
    }

    // Search, centred in the bar (or squeezed left of the right cluster).
    const float sw = std::min(S(480.0f), x - a.x - S(32.0f));
    if (sw > S(160.0f)) {
        float sx = a.x + (W - sw) * 0.5f;
        sx = std::min(sx, x - sw - S(12.0f));
        sx = std::max(sx, a.x + S(16.0f));
        ImGui::SetCursorScreenPos(ImVec2(sx, cy - ch * 0.5f));
        LibraryState& lib = ui.library;
        const char* hint = ui.page == Page::Screenshots ? "Search screenshots, games\xE2\x80\xA6" : "Search clips, games\xE2\x80\xA6";
        // Ctrl+F focuses the search.
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) ImGui::SetKeyboardFocusHere();
        if (TextField("##search", lib.search, sizeof(lib.search), hint, sw, ImGuiInputTextFlags_EscapeClearsAll, ICON_SEARCH, lib.search[0] ? ch : 0.0f)) {
            if (lib.search[0] && ui.page != Page::Clips && ui.page != Page::Screenshots) GoTo(ctx, Page::Clips);
        }
        hit(ImVec2(sx, cy - ch * 0.5f), ImVec2(sx + sw, cy + ch * 0.5f));
        if (lib.search[0]) {
            const ImVec2 xa(sx + sw - ch, cy - ch * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(xa.x + S(4.0f), xa.y + S(4.0f)));
            ButtonOpts o;
            o.style = Btn::Ghost;
            o.icon = ICON_CLOSE;
            o.size = ImVec2(ch - S(8.0f), ch - S(8.0f));
            o.tooltip = "Clear search";
            o.round = true;
            if (Button("##clearsearch", o)) lib.search[0] = 0;
        }
    }
}

} // namespace cs::ui
