// First run: three short steps on a big glass card. 1) the Save clip hotkey and clip length,
// 2) quality, 3) where clips go (+ mic and start-with-Windows). Everything can be changed later in
// Settings; closing early keeps the defaults.
#include "ui/AppContext.h"
#include "imgui_internal.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Hotkeys.h"
#include "core/Util.h"

#include <algorithm>
#include <cmath>

namespace cs::ui {

namespace {

struct QualityChoice {
    const char* name;
    const char* detail;
    const char* note;
    int resolution, mbps;
};
const QualityChoice kChoices[] = {
    { "Light", "720p, 60 fps", "Small files, easy on older PCs", 4, 12 },
    { "Balanced", "1080p, 60 fps", "Recommended for most games", 3, 25 },
    { "Sharp", "1440p, 60 fps", "Needs a recent graphics card", 2, 40 },
};

// A big selectable card (quality step).
bool ChoiceCard(const char* id, const QualityChoice& q, bool selected, ImVec2 size)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hov = ImGui::IsItemHovered();
    const ImGuiID gid = ImGui::GetItemID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float h = AnimHover(gid, hov), sel = Anim(AnimId(gid, 1), selected ? 1.0f : 0.0f, Motion::Hover);
    const float lift = Animations() ? -S(3.0f) * Spring(AnimId(gid, 2), hov ? 1.0f : 0.0f, Springs::Hover) : 0.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + lift), b(p.x + size.x, p.y + size.y + lift);
    const float r = S(Tok::RadiusCard);
    Shadow(dl, a, b, r, 0.8f + 0.6f * h);
    Glass(dl, a, b, r, h, Tok::GlassBorderSoft + 0.1f * sel, 0, 2);
    if (sel > 0.01f) {
        GradientRect(dl, a, b, Accent(0.28f * sel), Accent2(0.10f * sel), r);
        dl->AddRect(a, b, Accent2(sel), r, 0, S(2.0f));
    }
    WaveFx(dl, gid, a, b, r, hov, clicked, Accent2());
    PushFont(GetFonts().semibold, Type::Heading);
    dl->AddText(ImVec2(a.x + S(18.0f), a.y + S(18.0f)), Pal().text, q.name);
    PopFont();
    PushFont(GetFonts().medium, Type::Body);
    dl->AddText(ImVec2(a.x + S(18.0f), a.y + S(48.0f)), Mix(Pal().textDim, Pal().text, sel), q.detail);
    PopFont();
    PushFont(GetFonts().regular, Type::Caption);
    dl->AddText(ImVec2(a.x + S(18.0f), a.y + S(74.0f)), Pal().textFaint, q.note);
    PopFont();
    if (sel > 0.01f) {
        const ImVec2 c(b.x - S(22.0f), a.y + S(24.0f));
        dl->AddCircleFilled(c, S(10.0f), Accent2(sel));
        Icon(dl, ICON_CHECK, c, S(11.0f), White(sel));
    }
    return clicked;
}

void Dots(ImDrawList* dl, ImVec2 c, int step, int count)
{
    const float gap = S(14.0f);
    for (int i = 0; i < count; ++i) {
        const float x = c.x + (i - (count - 1) * 0.5f) * gap;
        const float on = Anim(AnimId(ImGui::GetID("##wdot"), (unsigned)i), i == step ? 1.0f : 0.0f, Motion::Slide);
        const float w = S(6.0f) + S(14.0f) * on;
        dl->AddRectFilled(ImVec2(x - w * 0.5f, c.y - S(3.0f)), ImVec2(x + w * 0.5f, c.y + S(3.0f)), Mix(White(0.25f), Accent2(1.0f), on), S(3.0f));
    }
}

} // namespace

void DrawWelcome(AppContext& ctx)
{
    Settings& s = ctx.settings;
    UiState& ui = ctx.ui;
    const float shown = Anim(ImGui::GetID("##welcomeA"), s.general.setupDone ? 0.0f : 1.0f, Motion::Page);
    if (shown < 0.01f) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, shown);
    ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 va = vp->Pos, vb(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    dl->AddRectFilled(va, vb, IM_COL32(6, 2, 18, (int)(150 * shown)));

    const float W = std::min(S(680.0f), vp->Size.x - S(40.0f)), H = std::min(S(520.0f), vp->Size.y - S(40.0f));
    const float rise = S(16.0f) * (1.0f - EaseOutCubic(shown));
    const ImVec2 a(va.x + (vp->Size.x - W) * 0.5f, va.y + (vp->Size.y - H) * 0.5f + rise), b(a.x + W, a.y + H);
    const float r = S(Tok::RadiusPanel + 4.0f);
    Shadow(dl, a, b, r, 2.0f);
    Glass(dl, a, b, r, 0.0f, Tok::GlassBorder, 0, 3);
    // Soft accent light in the corner.
    dl->PushClipRect(a, b, true);
    RadialGlow(dl, ImVec2(a.x + W * 0.15f, a.y), W * 0.6f, Accent(0.20f), 48);
    dl->PopClipRect();

    const int step = Clamp(ui.welcomeStep, 0, 2);
    const float pad = S(40.0f), cw = W - pad * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + S(34.0f)));
    ImGui::BeginGroup();
    static const char* kTitles[] = { "Welcome to Replayr", "Pick your quality", "Where should clips go?" };
    static const char* kBodies[] = {
        "Replayr quietly keeps the last moments of your game. When something great happens, press your hotkey and it's saved as a clip.",
        "You can change this any time in Settings. Higher quality needs a stronger graphics card and more disk space.",
        "Clips and screenshots are saved here. Last step, then you're ready to play.",
    };
    PushFont(GetFonts().semibold, Type::Title + 4.0f);
    ImGui::TextUnformatted(kTitles[step]);
    PopFont();
    Gap(4.0f);
    TextWrapped(kBodies[step], Pal().textDim, cw, GetFonts().regular, Type::Body);
    Gap(22.0f);

    if (step == 0) {
        Text("Save clip hotkey", Pal().text, GetFonts().medium, Type::Body);
        Gap(2.0f);
        const int slot = (int)HotkeyAction::SaveReplay;
        const bool capturing = ui.settings.capturing == slot;
        if (HotkeyBinder("##whk", Hotkeys::ToString(s.hotkeys[(size_t)slot]), capturing ? BinderState::Capturing : BinderState::Normal, cw)) {
            ui.settings.capturing = slot;
            ctx.Do(Action::StartKeyCapture);
        }
        TextWrapped("Click it and press the keys you want. Include Alt, Ctrl or Shift so it doesn't clash with your game.", Pal().textFaint, cw,
                    nullptr, Type::Caption);
        Gap(14.0f);
        Text("How much to save", Pal().text, GetFonts().medium, Type::Body);
        Gap(2.0f);
        static const char* lens[] = { "15 seconds", "30 seconds", "1 minute", "2 minutes" };
        static const int secs[] = { 15, 30, 60, 120 };
        int li = 1;
        for (int i = 0; i < 4; ++i)
            if (s.replay.seconds == secs[i]) li = i;
        if (Segmented("##wlen", &li, lens, 4, cw)) {
            s.replay.seconds = secs[li];
            ctx.ChangedEngine();
        }
    } else if (step == 1) {
        const float gap = S(14.0f), cardW = (cw - gap * 2.0f) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, gap);
            char id[16];
            snprintf(id, sizeof(id), "##wq%d", i);
            if (ChoiceCard(id, kChoices[i], s.video.resolution == kChoices[i].resolution, ImVec2(cardW, S(110.0f)))) {
                s.video.resolution = kChoices[i].resolution;
                s.video.fps = 60;
                s.video.bitrateMbps = kChoices[i].mbps;
                ctx.ChangedEngine();
            }
        }
    } else {
        Text("Clips folder", Pal().text, GetFonts().medium, Type::Body);
        Gap(2.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = S(110.0f), fh = S(Tok::ControlHeight);
        Glass(dl, p, ImVec2(p.x + cw - bw - S(10.0f), p.y + fh), S(Tok::RadiusControl), 0.0f, Tok::GlassBorderSoft, 0, 2);
        PushFont(GetFonts().regular, Type::Body);
        const std::string folder = Ellipsize(ToUtf8(s.ClipsFolder()).c_str(), cw - bw - S(36.0f));
        dl->AddText(ImVec2(p.x + S(12.0f), p.y + (fh - ImGui::GetFontSize()) * 0.5f), Pal().text, folder.c_str());
        PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x + cw - bw, p.y));
        ButtonOpts change;
        change.icon = ICON_FOLDER;
        change.size = ImVec2(bw, fh);
        if (Button("Change\xE2\x80\xA6##wf", change)) ctx.Do(Action::BrowseClipsFolder);
        Gap(16.0f);
        auto row = [&](const char* title, const char* sub, bool* v, void (AppContext::*changed)()) {
            const float y = ImGui::GetCursorPosY();
            const float x = ImGui::GetCursorPosX();
            Text(title, Pal().text, GetFonts().medium, Type::Body);
            Text(sub, Pal().textFaint, nullptr, Type::Caption);
            const float y2 = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(x + cw - S(44.0f), y + S(4.0f)));
            if (Toggle(title, v)) (ctx.*changed)();
            ImGui::SetCursorPos(ImVec2(x, y2 + S(8.0f)));
        };
        row("Record my microphone", "Off by default. Your voice goes into clips when it's on.", &s.audio.captureMic, &AppContext::ChangedAudio);
        row("Start with Windows", "Replayr waits in the tray so it's always ready.", &s.general.startWithWindows, &AppContext::ChangedStartup);
    }
    ImGui::EndGroup();

    // Footer: dots, Back, Next / Start.
    const float fy = b.y - S(30.0f) - S(42.0f);
    Dots(dl, ImVec2(a.x + pad + S(20.0f), fy + S(21.0f)), step, 3);
    const float bh = S(42.0f);
    ButtonOpts next;
    next.style = Btn::Primary;
    next.icon = step == 2 ? ICON_CHECK : ICON_CHEVRON_RIGHT;
    next.size = ImVec2(S(180.0f), bh);
    ImGui::SetCursorScreenPos(ImVec2(b.x - pad - next.size.x, fy));
    if (Button(step == 2 ? "Start clipping" : "Next", next)) {
        if (step == 2) {
            ctx.Do(Action::FinishSetup);
            s.general.setupDone = true;
        } else {
            ui.welcomeStep = step + 1;
        }
    }
    if (step > 0) {
        ButtonOpts back;
        back.style = Btn::Ghost;
        back.size = ImVec2(S(100.0f), bh);
        ImGui::SetCursorScreenPos(ImVec2(b.x - pad - next.size.x - S(10.0f) - back.size.x, fy));
        if (Button("Back##w", back)) ui.welcomeStep = step - 1;
    } else {
        ButtonOpts skip;
        skip.style = Btn::Ghost;
        skip.size = ImVec2(S(150.0f), bh);
        skip.tooltip = "Keep the defaults (30 s, 1080p60, Alt+F10). You can change everything in Settings.";
        ImGui::SetCursorScreenPos(ImVec2(b.x - pad - next.size.x - S(10.0f) - skip.size.x, fy));
        if (Button("Use defaults##w", skip)) {
            ctx.Do(Action::FinishSetup);
            s.general.setupDone = true;
        }
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace cs::ui
