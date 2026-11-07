// Settings: five categories on the left, rows on the right (title + short description on the left
// of each row, the control on the right). Technical options sit in a collapsed "Advanced" section.
#include "ui/AppContext.h"
#include "engine/MicTest.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Hotkeys.h"
#include "core/Log.h"
#include "core/Util.h"
#include "core/Window.h"
#include "engine/Encoder.h"
#include "AppInfo.h"
#include "core/InputOverlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>
#include <vector>

namespace cs::ui {

constexpr int VK_LBUTTON_ = 0x01, VK_RBUTTON_ = 0x02, VK_MBUTTON_ = 0x04;


namespace {

struct Category {
    const char* icon;
    const char* name;
};
const Category kCategories[] = {
    { ICON_GENERAL, "General" }, { ICON_RECORDING, "Recording" }, { ICON_AUDIO, "Audio" }, { ICON_KEYBOARD, "Hotkeys" }, { ICON_FOLDER, "Storage" },
};
constexpr int kCategoryCount = (int)(sizeof(kCategories) / sizeof(kCategories[0]));

// ---- row layout ---------------------------------------------------------------------------

struct Rows {
    float x0 = 0, width = 0, controlW = 0;
    float rowStart = 0, textEnd = 0;
    int count = 0;
};
Rows g_rows;

void Group(const char* title)
{
    Gap(4.0f);
    Text(title, Pal().textFaint, GetFonts().medium, Type::Caption);
    Gap(2.0f);
}

bool BeginRows(const char* id)
{
    const bool open = BeginPanel(id, ImVec2(0, 0), Tok::Space5, false, Tok::RadiusCard, Tok::GlassBorderSoft + 0.04f);
    g_rows.x0 = ImGui::GetCursorPosX();
    g_rows.width = ImGui::GetContentRegionAvail().x;
    g_rows.controlW = std::min(S(320.0f), g_rows.width * 0.45f);
    g_rows.count = 0;
    return open;
}

void EndRows()
{
    EndPanel();
    Gap(16.0f);
}

// Draws the label column, then leaves the cursor where the control goes.
void Row(const char* title, const char* desc = nullptr)
{
    if (g_rows.count++ > 0) {
        Gap(4.0f);
        Divider(0.08f);
        Gap(4.0f);
    }
    g_rows.rowStart = ImGui::GetCursorPosY();
    const float textW = g_rows.width - g_rows.controlW - S(24.0f);
    PushFont(GetFonts().medium, Type::Body);
    ImGui::PushTextWrapPos(g_rows.x0 + textW);
    ImGui::TextUnformatted(title);
    ImGui::PopTextWrapPos();
    PopFont();
    if (desc) {
        PushFont(GetFonts().regular, Type::Caption);
        ImGui::PushStyleColor(ImGuiCol_Text, Pal().textDim);
        ImGui::PushTextWrapPos(g_rows.x0 + textW);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        PopFont();
    }
    g_rows.textEnd = ImGui::GetCursorPosY();
    const float textH = g_rows.textEnd - g_rows.rowStart;
    const float off = std::max(0.0f, (textH - S(Tok::ControlHeight)) * 0.5f - S(2.0f));
    ImGui::SetCursorPos(ImVec2(g_rows.x0 + g_rows.width - g_rows.controlW, g_rows.rowStart + off));
    ImGui::BeginGroup();
}

void RowEnd()
{
    ImGui::EndGroup();
    const float y = std::max(g_rows.textEnd, ImGui::GetCursorPosY());
    ImGui::SetCursorPos(ImVec2(g_rows.x0, y));
    ImGui::Dummy(ImVec2(0, 0));
}

// Right-aligns a control of width w within the control column.
void Right(float w) { ImGui::SetCursorPosX(g_rows.x0 + g_rows.width - w); }
float CW() { return g_rows.controlW; }

void ToggleRow(const char* title, const char* desc, bool* v, AppContext& ctx, void (AppContext::*changed)())
{
    Row(title, desc);
    Right(S(40.0f));
    if (Toggle(title, v)) (ctx.*changed)();
    RowEnd();
}

// Text button with an optional icon (the one Button, with options filled in).
bool Act(const char* label, const char* icon = nullptr, Btn style = Btn::Secondary, ImVec2 size = ImVec2(0, 0))
{
    ButtonOpts o;
    o.style = style;
    o.icon = icon;
    o.size = size;
    return Button(label, o);
}

// "Advanced" disclosure: a quiet header that expands the technical rows below it.
bool Advanced(AppContext& ctx, int index)
{
    bool& open = ctx.ui.settings.advanced[index];
    ImGui::PushID(index);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = S(40.0f);
    if (ImGui::InvisibleButton("##adv", ImVec2(w, h))) open = !open;
    const bool hov = ImGui::IsItemHovered();
    const ImGuiID id = ImGui::GetItemID();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float hv = Anim(id, hov ? 1.0f : 0.0f, Motion::Hover);
    const float rot = Anim(AnimId(id, 2), open ? 1.0f : 0.0f, Motion::Slide);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 b(p.x + w, p.y + h);
    Glass(dl, p, b, S(Tok::RadiusCard), hv * 0.6f, Tok::GlassBorderSoft);
    WaveFx(dl, id, p, b, S(Tok::RadiusCard), hov, false, Accent2());
    const ImU32 col = Mix(Pal().textDim, White(1.0f), hv);
    Icon(dl, ICON_SETTINGS, ImVec2(p.x + S(20.0f), p.y + h * 0.5f), S(13.0f), col);
    PushFont(GetFonts().medium, Type::Body);
    dl->AddText(ImVec2(p.x + S(38.0f), p.y + (h - ImGui::GetFontSize()) * 0.5f), col, "Advanced");
    PopFont();
    // Chevron turns from right to down.
    const ImVec2 c(b.x - S(22.0f), p.y + h * 0.5f);
    const float k = S(4.5f), ang = rot * 1.5708f;
    auto R = [&](float x, float y) { return ImVec2(c.x + x * std::cos(ang) - y * std::sin(ang), c.y + x * std::sin(ang) + y * std::cos(ang)); };
    dl->AddPolyline(std::vector<ImVec2>{ R(-k * 0.5f, -k), R(k * 0.5f, 0.0f), R(-k * 0.5f, k) }.data(), 3, col, 0, S(1.6f));
    Gap(12.0f);
    return open;
}

void StartCapture(AppContext& ctx, int slot)
{
    ctx.ui.settings.capturing = slot;
    ctx.Do(Action::StartKeyCapture);
}

bool DeviceDropdown(const char* id, std::wstring& current, const std::vector<engine::AudioDevice>& list, float width)
{
    std::string defName = "Default";
    for (const auto& d : list)
        if (d.isDefault) defName = "Default (" + d.name + ")";
    std::string label = defName;
    if (!current.empty()) {
        label = "Unavailable device";
        for (const auto& d : list)
            if (d.id == current) label = d.name;
    }
    bool changed = false;
    if (BeginDropdown(id, label.c_str(), width)) {
        if (DropdownItem(defName.c_str(), current.empty())) {
            current.clear();
            changed = true;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            ImGui::PushID((int)i);
            if (DropdownItem(list[i].name.c_str(), current == list[i].id)) {
                current = list[i].id;
                changed = true;
            }
            ImGui::PopID();
        }
        EndDropdown();
    }
    return changed;
}

// ---- categories ---------------------------------------------------------------------------

struct Preset {
    const char* name;
    float a[3], b[3];
};
const Preset kPresets[] = {
    { "Violet", { 0.545f, 0.247f, 0.988f }, { 0.761f, 0.122f, 0.878f } },
    { "Blue", { 0.231f, 0.655f, 1.0f }, { 0.361f, 0.792f, 1.0f } },
    { "Rose", { 0.925f, 0.282f, 0.600f }, { 0.976f, 0.451f, 0.627f } },
    { "Mint", { 0.063f, 0.725f, 0.506f }, { 0.078f, 0.722f, 0.651f } },
    { "Sunset", { 0.976f, 0.451f, 0.086f }, { 0.937f, 0.267f, 0.267f } },
    { "Ocean", { 0.024f, 0.588f, 0.784f }, { 0.231f, 0.341f, 0.957f } },
    { "Gold", { 0.961f, 0.682f, 0.075f }, { 0.976f, 0.451f, 0.086f } },
    { "Crimson", { 0.863f, 0.149f, 0.267f }, { 0.616f, 0.090f, 0.427f } },
    { "Ice", { 0.580f, 0.769f, 0.961f }, { 0.722f, 0.600f, 0.988f } },
};

void AccentRow(AppContext& ctx)
{
    Settings& s = ctx.settings;
    Row("Accent colour", "Used for highlights, buttons and progress.");
    const float sw = S(30.0f), gap = S(10.0f);
    const int n = (int)(sizeof(kPresets) / sizeof(kPresets[0]));
    Right(sw * n + gap * (n - 1));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(3.0f));
    for (int i = 0; i < n; ++i) {
        const Preset& p = kPresets[i];
        if (i) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##sw", ImVec2(sw, sw));
        const bool hov = ImGui::IsItemHovered();
        const ImGuiID id = ImGui::GetItemID();
        Tooltip(p.name);
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool sel = std::fabs(p.a[0] - s.general.accent[0]) < 0.01f && std::fabs(p.a[1] - s.general.accent[1]) < 0.01f;
        const float h = Anim(id, hov || sel ? 1.0f : 0.0f, Motion::Hover);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(pos.x + sw * 0.5f, pos.y + sw * 0.5f);
        const ImU32 ca = ImGui::ColorConvertFloat4ToU32(ImVec4(p.a[0], p.a[1], p.a[2], 1)), cb = ImGui::ColorConvertFloat4ToU32(ImVec4(p.b[0], p.b[1], p.b[2], 1));
        const float rr = sw * (0.40f + 0.03f * h);
        GradientRectH(dl, ImVec2(c.x - rr, c.y - rr), ImVec2(c.x + rr, c.y + rr), ca, cb, rr);
        WaveFx(dl, id, ImVec2(c.x - rr, c.y - rr), ImVec2(c.x + rr, c.y + rr), rr, hov, clicked, White(1.0f));
        if (sel) dl->AddCircle(c, sw * 0.5f + S(1.0f), White(0.95f), 32, S(2.0f));
        else if (h > 0.01f) dl->AddCircle(c, sw * 0.5f + S(1.0f), White(0.4f * h), 32, S(1.5f));
        ImGui::PopID();
        if (clicked) {
            for (int k = 0; k < 3; ++k) {
                s.general.accent[k] = p.a[k];
                s.general.accent2[k] = p.b[k];
            }
            ctx.ChangedTheme();
        }
    }
    RowEnd();
}

// A small 16:9 "game frame" with the banner playing on a loop, exactly as an export would show it.
void BannerDemo(AppContext& ctx, float width)
{
    SettingsState& st = ctx.ui.settings;
    const Settings& s = ctx.settings;
    const float h = width * 9.0f / 16.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(10.0f);
    // A stand-in scene: dusk sky, hills, a sun.
    GradientRect(dl, a, b, IM_COL32(38, 22, 92, 255), IM_COL32(196, 84, 140, 255), r);
    RadialGlow(dl, ImVec2(a.x + width * 0.30f, a.y + h * 0.62f), h * 0.45f, IM_COL32(255, 200, 140, 120));
    dl->AddCircleFilled(ImVec2(a.x + width * 0.30f, a.y + h * 0.62f), h * 0.11f, IM_COL32(255, 222, 170, 255), 48);
    const ImVec2 hill[] = { ImVec2(a.x, b.y - h * 0.30f), ImVec2(a.x + width * 0.25f, b.y - h * 0.42f), ImVec2(a.x + width * 0.55f, b.y - h * 0.28f),
                            ImVec2(a.x + width * 0.80f, b.y - h * 0.38f), ImVec2(b.x, b.y - h * 0.26f) };
    for (int i = 0; i < 4; ++i)
        dl->AddQuadFilled(hill[i], hill[i + 1], ImVec2(hill[i + 1].x, b.y), ImVec2(hill[i].x, b.y), IM_COL32(28, 10, 56, 255));
    dl->AddRect(a, b, White(Tok::GlassBorder), r);
    // Loop: 6.5 s for "first 5 seconds" (in at 1 s, out by 5.4 s), 4 s for "always" (shine each loop).
    const double now = ImGui::GetTime();
    if (now - st.bannerPreviewAt > 1e6) st.bannerPreviewAt = now;
    const double cycle = s.banner.mode == 1 ? 4.0 : 6.5;
    const double t = std::fmod(std::max(0.0, now - st.bannerPreviewAt), cycle);
    dl->PushClipRect(a, b, true);
    const int vw = 1920, vh = 1080;
    DrawBannerPreview(ctx, a, width / (float)vw, vw, vh, t);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(width, h));
}

void General(AppContext& ctx)
{
    Settings& s = ctx.settings;
    Group("Startup");
    if (BeginRows("##start")) {
        ToggleRow("Start with Windows", "Replayr starts quietly in the tray when you sign in.", &s.general.startWithWindows, ctx, &AppContext::ChangedStartup);
        ToggleRow("Close to tray", "The close button hides the window; hotkeys keep working. Quit from the tray icon.", &s.general.closeToTray, ctx,
                  &AppContext::Changed);
    }
    EndRows();

    Group("Notifications and sounds");
    if (BeginRows("##notif")) {
        ToggleRow("Show notifications", "A small card in the corner when a clip or screenshot is saved. It never takes focus from your game.",
                  &s.general.toasts, ctx, &AppContext::Changed);
        Row("Position");
        static const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        Right(CW());
        if (Dropdown("##corner", &s.general.toastCorner, corners, 4, CW())) ctx.ChangedTheme();
        RowEnd();
        ToggleRow("Sounds", "A short chime when a clip or screenshot is saved.", &s.general.sounds, ctx, &AppContext::Changed);
        ToggleRow("Interface sounds", "A very quiet tick when you hover and click buttons.", &s.general.uiSounds, ctx, &AppContext::Changed);
        Row("Try it");
        Right(CW());
        if (Act("Show a test notification", ICON_BELL, Btn::Secondary, ImVec2(CW(), 0))) ctx.Do(Action::TestToast);
        RowEnd();
    }
    EndRows();

    Group("Replayr banner");
    if (BeginRows("##banner")) {
        ToggleRow("Banner on exported clips", "\"Clipped using Replayr\" in a corner. This is the default; you can switch it off in each export.",
                  &s.banner.enabled, ctx, &AppContext::Changed);
        Row("When it shows", "Slides in at 1 s, stays for 4 s and slides away. Or stays for the whole clip.");
        static const char* modes[] = { "First 5 seconds", "Always" };
        Right(CW());
        if (Segmented("##bmode", &s.banner.mode, modes, 2, CW())) {
            ctx.ui.settings.bannerPreviewAt = ImGui::GetTime();
            ctx.Changed();
        }
        RowEnd();
        Row("Corner");
        static const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        Right(CW());
        if (Dropdown("##bcorner", &s.banner.corner, corners, 4, CW())) {
            ctx.ui.settings.bannerPreviewAt = ImGui::GetTime();
            ctx.Changed();
        }
        RowEnd();
        ToggleRow("Also on screenshots", "Stamps the banner (without animation) on every new screenshot.", &s.banner.screenshots, ctx, &AppContext::Changed);
        Row("Preview", "Exactly how it looks and moves in an export.");
        Right(CW());
        ImGui::BeginGroup();
        BannerDemo(ctx, CW());
        Gap(6.0f);
        if (Act("Replay", ICON_REFRESH, Btn::Ghost, ImVec2(CW(), 0))) ctx.ui.settings.bannerPreviewAt = ImGui::GetTime();
        ImGui::EndGroup();
        RowEnd();
    }
    EndRows();

    Group("Appearance");
    if (BeginRows("##look")) {
        AccentRow(ctx);
        Row("Background", "Animated drifts slowly (one loop every 45 s). Solid is lightest on older PCs.");
        static const char* bgStyles[] = { "Animated", "Static", "Solid" };
        Right(CW());
        if (Segmented("##bgstyle", &s.general.background, bgStyles, 3, CW())) ctx.ChangedTheme();
        RowEnd();
        Row("Reduce motion", "Simple fades instead of springs, tilts and the moving background.");
        bool reduce = !s.general.animations;
        Right(S(40.0f));
        if (Toggle("##reducemotion", &reduce)) {
            s.general.animations = !reduce;
            ctx.ChangedTheme();
        }
        RowEnd();
    }
    EndRows();

    if (Advanced(ctx, 0)) {
        if (BeginRows("##genadv")) {
            ToggleRow("Start hidden", "Open straight to the tray instead of showing the window.", &s.general.startMinimized, ctx, &AppContext::Changed);
            ToggleRow("Minimise to tray", "Minimising hides the window instead of putting it on the taskbar.", &s.general.minimizeToTray, ctx,
                      &AppContext::Changed);
            Row("Background", "Replayr's own backdrop, or let Windows blur your wallpaper behind the window.");
            static const char* bgs[] = { "Replayr", "Mica", "Acrylic" };
            int bg = Clamp(s.general.backdrop, 0, 2);
            const bool supported = ctx.window.SupportsBackdrop();
            ImGui::BeginDisabled(!supported);
            Right(CW());
            if (Segmented("##bg", &bg, bgs, 3, CW())) {
                s.general.backdrop = bg;
                ctx.ChangedTheme();
            }
            ImGui::EndDisabled();
            RowEnd();
            Row("Interface size", "Scales everything on top of Windows' display scaling.");
            char v[16];
            snprintf(v, sizeof(v), "%d%%", (int)std::lround(s.general.uiScale * 100.0f));
            Right(CW());
            if (Slider("##uiscale", &s.general.uiScale, 0.8f, 1.5f, v, CW(), 0.05f)) ctx.ChangedTheme();
            RowEnd();
            Row("Log files", "Send these if something doesn't work.");
            Right(CW());
            if (Act("Open log folder", ICON_FOLDER_OPEN, Btn::Secondary, ImVec2(CW(), 0))) ctx.Do(Action::OpenLogFolder);
            RowEnd();
        }
        EndRows();
    }
    if (Updater::Enabled() && ctx.updater) {
        Group("Updates");
        if (BeginRows("##updates")) {
            ToggleRow("Update automatically", "Checks for a new version when Replayr starts and downloads it in the background.", &s.general.autoUpdate,
                      ctx, &AppContext::Changed);
            const UpdateState us = ctx.updater->State();
            std::string status = ctx.updater->Message();
            if (us == UpdateState::Checking) status = "Checking\xE2\x80\xA6";
            else if (us == UpdateState::Downloading) status = "Downloading " + ctx.updater->Version() + "\xE2\x80\xA6";
            else if (status.empty()) status = "You're on " CS_APP_VERSION ".";
            Row("Version " CS_APP_VERSION, status.c_str());
            Right(CW());
            if (us == UpdateState::Ready) {
                if (Act("Restart to update", ICON_DOWNLOAD, Btn::Primary, ImVec2(CW(), 0))) ctx.Do(Action::ApplyUpdate);
            } else {
                ImGui::BeginDisabled(us == UpdateState::Checking || us == UpdateState::Downloading);
                if (Act("Check now", ICON_REFRESH, Btn::Secondary, ImVec2(CW(), 0))) ctx.Do(Action::CheckUpdates);
                ImGui::EndDisabled();
            }
            RowEnd();
        }
        EndRows();
    }
    TextWrapped(CS_APP_NAME " " CS_APP_VERSION "  \xC2\xB7  clips and screenshots for Windows", Pal().textFaint, ImGui::GetContentRegionAvail().x,
                GetFonts().regular, Type::Caption);
    TextWrapped("Mic noise suppression uses RNNoise, (c) 2017 Mozilla, Jean-Marc Valin, Xiph.Org Foundation and Mark Borgerding, "
                "under the BSD 3-clause licence (included with Replayr's source in third_party/rnnoise/COPYING).",
                Pal().textFaint, ImGui::GetContentRegionAvail().x, GetFonts().regular, Type::Caption);
}

int RecommendedMbps(const AppContext& ctx)
{
    const Settings& s = ctx.settings;
    static const int heights[] = { 0, 2160, 1440, 1080, 720 };
    int h = heights[Clamp(s.video.resolution, 0, 4)], w = h * 16 / 9;
    if (h == 0) {
        w = ctx.monitors.empty() ? 1920 : ctx.monitors[0].width;
        h = ctx.monitors.empty() ? 1080 : ctx.monitors[0].height;
    }
    const double bpp = s.video.codec == "hevc" ? 0.10 : 0.15;
    return std::max(5, (int)(std::round((double)w * h * s.video.fps * bpp / 1e6 / 5.0) * 5.0));
}

// Live preview of the keyboard & mouse overlay: press keys while looking at it.
void InputPreview(AppContext& ctx, float width)
{
    const InputDisplaySettings& d = ctx.settings.inputDisplay;
    float lw = 0, lh = 0;
    const std::vector<OverlayKey>& keys = OverlayLayout(d.show, lw, lh);
    const float u = std::min(width / (lw + 0.7f), S(26.0f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = u * 0.35f, W = lw * u + pad * 2.0f, H = lh * u + pad * 2.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(p.x + pad, p.y + pad);
    auto down = [&](int vk) { return ctx.inputOverlay && ctx.inputOverlay->IsDown(vk); };
    const float op = d.opacity;
    for (const OverlayKey& k : keys) {
        const ImVec2 a(o.x + k.x * u, o.y + k.y * u), b(a.x + k.w * u, a.y + k.h * u);
        if (k.vk == -1) {   // mouse body
            dl->AddRectFilled(a, b, IM_COL32(14, 8, 32, (int)(165 * op)), u * 1.45f);
            dl->AddRect(a, b, White(0.35f * op), u * 1.45f, 0, std::max(1.0f, u * 0.04f));
            continue;
        }
        const bool on = down(k.vk);
        const bool button = k.vk == VK_LBUTTON_ || k.vk == VK_RBUTTON_;
        const float r = button ? 0.0f : (k.vk == VK_MBUTTON_ ? k.w * u * 0.5f : u * 0.2f);
        if (button) {
            // Clipped to the body: draw as the top half of the rounded body.
            const ImDrawFlags f = k.vk == VK_LBUTTON_ ? ImDrawFlags_RoundCornersTopLeft : ImDrawFlags_RoundCornersTopRight;
            if (on) GradientRectH(dl, a, b, Accent(op), Accent2(op), u * 1.45f, f);
            dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), White(0.3f * op));
            if (k.vk == VK_LBUTTON_) dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, b.y), White(0.3f * op));
            continue;
        }
        if (on) {
            for (int g = 1; g <= 3; ++g)
                dl->AddRect(ImVec2(a.x - g * 1.6f, a.y - g * 1.6f), ImVec2(b.x + g * 1.6f, b.y + g * 1.6f), Accent2(0.22f * op / g), r + g * 1.6f, 0, 1.8f);
            GradientRectH(dl, a, b, Accent(op), Accent2(op), r);
        } else {
            dl->AddRectFilled(a, b, IM_COL32(14, 8, 32, (int)(165 * op)), r);
            GradientRect(dl, a, ImVec2(b.x, (a.y + b.y) * 0.5f), White(0.13f * op), White(0.0f), r, ImDrawFlags_RoundCornersTop);
        }
        dl->AddRect(a, b, White((on ? 0.85f : 0.28f) * op), r, 0, std::max(1.0f, u * 0.035f));
        if (k.label && *k.label) {
            const float fs = std::strlen(k.label) > 1 ? u * 0.30f : u * 0.42f;
            ImFont* f = GetFonts().semibold;
            const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, k.label);
            dl->AddText(f, fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), White((on ? 1.0f : 0.85f) * op), k.label);
        }
    }
    ImGui::Dummy(ImVec2(W, H));
}

void Recording(AppContext& ctx)
{
    Settings& s = ctx.settings;
    const engine::EngineStatus& st = ctx.status;
    // Only say something when capture can't run.
    if (st.buffer == engine::BufferState::Error) {
        if (BeginPanel("##engerr", ImVec2(0, 0), Tok::Space4, false, Tok::RadiusCard, Tok::GlassBorderSoft + 0.04f)) {
            Text("Capture can't start", Pal().danger, GetFonts().medium);
            TextWrapped(st.lastError.c_str(), Pal().textDim, ImGui::GetContentRegionAvail().x, GetFonts().regular, Type::Caption);
        }
        EndPanel();
        Gap(16.0f);
    }

    Group("Replay buffer");
    if (BeginRows("##replay")) {
        Row("Replay buffer", "Always records in the background so you can save what just happened.");
        Right(S(40.0f));
        if (Toggle("##replayon", &s.replay.enabled)) ctx.Changed();
        RowEnd();
        Row("Clip length", "How much is saved when you press Save clip.");
        char v[32];
        snprintf(v, sizeof(v), "%s", FormatDuration(s.replay.seconds).c_str());
        Right(CW());
        if (SliderInt("##replaylen", &s.replay.seconds, 15, 600, v, 15, CW())) ctx.ChangedEngine();
        RowEnd();
        {   // The buffer lives on disk as short segments: roughly bitrate x length (x2 in Both mode).
            const int copies = s.video.source.rfind("both:", 0) == 0 ? 2 : 1;
            const double mb = (s.video.bitrateMbps + 0.2) * s.replay.seconds / 8.0 * copies;
            char est[128];
            if (mb >= 1024.0) snprintf(est, sizeof(est), "The buffer uses about %.1f GB of disk while it runs, and very little RAM.", mb / 1024.0);
            else snprintf(est, sizeof(est), "The buffer uses about %.0f MB of disk while it runs, and very little RAM.", mb);
            TextWrapped(est, Pal().textFaint, ImGui::GetContentRegionAvail().x, GetFonts().regular, Type::Caption);
        }
    }
    EndRows();

    Group("Video");
    if (BeginRows("##video")) {
        // Capture source: Game only / Whole screen / Both.
        const bool isGame = s.video.source == "game", isBoth = s.video.source.rfind("both:", 0) == 0;
        int mode = isGame ? 0 : (isBoth ? 2 : 1);
        static const char* modes[] = { "Game only", "Whole screen", "Both" };
        static const char* modeHelp[] = {
            "Records just the game's window. Notifications, Discord pop-ups and other windows never show up.",
            "Records everything on the screen you pick, including other windows and pop-ups.",
            "Records the game and the whole screen at the same time, and saves both as separate files.",
        };
        Row("Capture source", modeHelp[mode]);
        Right(CW());
        if (Segmented("##capmode", &mode, modes, 3, CW())) {
            // Remember which screen was picked, so switching modes doesn't lose it.
            int mon = 0;
            if (s.video.source.rfind("monitor:", 0) == 0) mon = atoi(s.video.source.c_str() + 8);
            else if (isBoth) mon = atoi(s.video.source.c_str() + 5);
            s.video.source = mode == 0 ? std::string("game") : (mode == 1 ? "monitor:" + std::to_string(mon) : "both:" + std::to_string(mon));
            ctx.ChangedEngine();
        }
        RowEnd();

        if (mode != 0) {
            Row(mode == 2 ? "Screen" : "Which screen", mode == 1 ? "\"Game's display\" follows the window in front." : nullptr);
            std::string current = "Game's display";
            int idx = -1;
            if (s.video.source.rfind("monitor:", 0) == 0) idx = atoi(s.video.source.c_str() + 8);
            else if (mode == 2) idx = atoi(s.video.source.c_str() + 5);
            if (idx >= 0) current = idx < (int)ctx.monitors.size() ? ctx.monitors[(size_t)idx].name : "Display " + std::to_string(idx + 1);
            Right(CW());
            if (BeginDropdown("##source", current.c_str(), CW(), ICON_MONITOR)) {
                for (size_t i = 0; i < ctx.monitors.size(); ++i) {
                    const std::string key = (mode == 2 ? "both:" : "monitor:") + std::to_string(i);
                    if (DropdownItem(ctx.monitors[i].name.c_str(), s.video.source == key, ICON_MONITOR)) {
                        s.video.source = key;
                        ctx.ChangedEngine();
                    }
                }
                if (mode == 1 && DropdownItem("Game's display", s.video.source == "window:foreground", ICON_GAME)) {
                    s.video.source = "window:foreground";
                    ctx.ChangedEngine();
                }
                EndDropdown();
            }
            RowEnd();
        }
        if (mode == 2) {
            Row("Uses more of your PC",
                "Two recordings at once: about twice the encoder work and twice the buffer on disk (roughly 2-4% more GPU load "
                "in games). If your GPU can't run two at once, Replayr records the game only and tells you.");
            Right(S(24.0f));
            Icon(ImGui::GetWindowDrawList(), ICON_WARNING, ImVec2(ImGui::GetCursorScreenPos().x + S(12.0f), ImGui::GetCursorScreenPos().y + S(14.0f)),
                 S(16.0f), Pal().warning);
            ImGui::Dummy(ImVec2(S(24.0f), S(28.0f)));
            RowEnd();
        }
        if (mode != 1)
            ToggleRow("Hide the capture border", "Asks Windows not to draw a yellow border around the game while it's recorded (Windows 11).",
                      &s.video.hideBorder, ctx, &AppContext::ChangedEngine);
        ToggleRow("Hide Replayr from recordings", "Keeps Replayr's own window out of your clips and screenshots.", &s.video.hideApp, ctx,
                  &AppContext::Changed);

        Row("Resolution", "Lower resolutions make smaller files. Never upscales.");
        static const char* resolutions[] = { "Native", "2160p", "1440p", "1080p", "720p" };
        Right(CW());
        if (Dropdown("##res", &s.video.resolution, resolutions, 5, CW())) ctx.ChangedEngine();
        RowEnd();

        Row("Frame rate");
        static const char* fpsItems[] = { "30", "60", "120", "144" };
        static const int fpsValues[] = { 30, 60, 120, 144 };
        int fi = 1;
        for (int i = 0; i < 4; ++i)
            if (s.video.fps == fpsValues[i]) fi = i;
        Right(CW());
        if (Segmented("##fps", &fi, fpsItems, 4, CW())) {
            s.video.fps = fpsValues[fi];
            ctx.ChangedEngine();
        }
        RowEnd();
    }
    EndRows();

    // How capture is doing, so people can trust it isn't costing them frames.
    Group("Performance");
    if (BeginRows("##perf")) {
        const bool running = st.buffer == engine::BufferState::Ready || st.buffer == engine::BufferState::Filling || st.recording;
        char fpsLine[96];
        if (running && st.captureFps > 0.0f)
            snprintf(fpsLine, sizeof(fpsLine), "%.0f of %d fps, %u dropped", st.captureFps, s.video.fps, st.droppedFrames);
        else snprintf(fpsLine, sizeof(fpsLine), "Not capturing right now");
        Row("Capture", running ? "Recording frames on your graphics card, so your game keeps its frame rate." : nullptr);
        Right(CW());
        Text(fpsLine, running && st.droppedFrames == 0 ? Pal().success : Pal().textDim, GetFonts().medium, Type::Body);
        RowEnd();
        Row("Encoder", st.encoderName.empty() ? "Starts with the replay buffer." : nullptr);
        Right(CW());
        Text(st.encoderName.empty() ? "-" : st.encoderName.c_str(), Pal().text, GetFonts().medium, Type::Body);
        RowEnd();
        Row("Your PC right now", "Everything running, not just Replayr.");
        char load[64];
        if (ctx.system.gpu >= 0.0f) snprintf(load, sizeof(load), "CPU %.0f%%   GPU %.0f%%", std::max(0.0f, ctx.system.cpu), ctx.system.gpu);
        else snprintf(load, sizeof(load), "CPU %.0f%%", std::max(0.0f, ctx.system.cpu));
        Right(CW());
        Text(load, Pal().text, GetFonts().medium, Type::Body);
        RowEnd();
    }
    EndRows();

    // Keyboard & mouse overlay (key viewer).
    Group("Keyboard & mouse overlay");
    if (BeginRows("##inputs")) {
        InputDisplaySettings& d = s.inputDisplay;
        ToggleRow("Show my keys and mouse", "A small keyboard and mouse that light up when you press them. Nothing is saved or sent anywhere.", &d.enabled,
                  ctx, &AppContext::Changed);
        if (d.enabled) {
            Row("Where it shows", d.mode == 0 ? "On your screen, so it's in your clips too." : d.mode == 1 ? "Only you see it; clips stay clean."
                                                                                               : "Only in recordings; nothing on your screen.");
            static const char* modes[] = { "Screen + clips", "Screen only", "Clips only" };
            Right(CW());
            if (Segmented("##idmode", &d.mode, modes, 3, CW())) ctx.Changed();
            RowEnd();
            Row("Show");
            static const char* shows[] = { "Both", "Keyboard", "Mouse" };
            Right(CW());
            if (Segmented("##idshow", &d.show, shows, 3, CW())) ctx.Changed();
            RowEnd();
            Row("Corner");
            static const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
            Right(CW());
            if (Dropdown("##idcorner", &d.corner, corners, 4, CW())) ctx.Changed();
            RowEnd();
            Row("Size");
            static const char* sizes[] = { "Small", "Medium", "Large" };
            Right(CW());
            if (Segmented("##idsize", &d.size, sizes, 3, CW())) ctx.Changed();
            RowEnd();
            Row("Opacity");
            char v[16];
            snprintf(v, sizeof(v), "%d%%", (int)std::lround(d.opacity * 100.0f));
            Right(CW());
            if (Slider("##idop", &d.opacity, 0.2f, 1.0f, v, CW(), 0.05f)) ctx.Changed();
            RowEnd();
            ToggleRow("Only during games", "Hides it while a normal window is in front, so it never shows what you type in chats or browsers.", &d.gamesOnly,
                      ctx, &AppContext::Changed);
            Row("Preview", "Press some keys or click to try it.");
            Right(CW());
            InputPreview(ctx, CW());
            RowEnd();
        }
    }
    EndRows();

    if (Advanced(ctx, 1)) {
        if (BeginRows("##recadv")) {
            Row("Encoder", "Your GPU's encoder is fast and barely affects your game. Software is a fallback.");
            static const char* encs[] = { "GPU (auto)", "Software" };
            int e = s.video.encoder == "x264" ? 1 : 0;
            Right(CW());
            if (Segmented("##enc", &e, encs, 2, CW())) {
                s.video.encoder = e == 1 ? "x264" : "auto";
                ctx.ChangedEngine();
            }
            RowEnd();

            Row("Format", "HEVC makes files about 40% smaller; older devices may not play it.");
            static const char* codecs[] = { "H.264", "HEVC" };
            int ci = s.video.codec == "hevc" ? 1 : 0;
            Right(CW());
            if (Segmented("##codec", &ci, codecs, 2, CW())) {
                s.video.codec = ci == 1 ? "hevc" : "h264";
                ctx.ChangedEngine();
            }
            RowEnd();

            char desc[96];
            snprintf(desc, sizeof(desc), "Higher looks better and makes bigger files. Recommended here: %d Mbps.", RecommendedMbps(ctx));
            Row("Bitrate", desc);
            char v[32];
            snprintf(v, sizeof(v), "%d Mbps", s.video.bitrateMbps);
            Right(CW());
            if (SliderInt("##bitrate", &s.video.bitrateMbps, 5, 150, v, 5, CW())) ctx.ChangedEngine();
            RowEnd();

            ToggleRow("Show the mouse cursor", nullptr, &s.video.captureCursor, ctx, &AppContext::ChangedEngine);
        }
        EndRows();
    }
}

void MicCleanup(AppContext& ctx);

void Audio(AppContext& ctx)
{
    Settings& s = ctx.settings;
    const auto outs = ctx.audio.OutputDevices();
    const auto ins = ctx.audio.InputDevices();

    Group("System audio");
    if (BeginRows("##sys")) {
        ToggleRow("Record system audio", "Everything you hear: the game, voice chat, music.", &s.audio.captureSystem, ctx, &AppContext::ChangedAudio);
        Row("Device", "The output whose sound ends up in your clips.");
        Right(CW());
        if (DeviceDropdown("##outdev", s.audio.outputDeviceId, outs, CW())) ctx.ChangedAudio();
        RowEnd();
        Row("Volume");
        char v[16];
        snprintf(v, sizeof(v), "%d%%", (int)std::lround(s.audio.systemVolume * 100.0f));
        Right(CW());
        ImGui::BeginGroup();
        if (Slider("##sysvol", &s.audio.systemVolume, 0.0f, 2.0f, v, CW(), 0.05f)) ctx.ChangedAudio();
        LevelMeter("##sysmeter", s.audio.captureSystem ? ctx.levels.system * s.audio.systemVolume : 0.0f, ImVec2(CW(), S(6.0f)), !s.audio.captureSystem);
        ImGui::EndGroup();
        RowEnd();
    }
    EndRows();

    Group("Microphone");
    if (BeginRows("##mic")) {
        ToggleRow("Record microphone", "Off by default. Your voice goes into your clips when this is on.", &s.audio.captureMic, ctx, &AppContext::ChangedAudio);
        if (s.audio.captureMic) {
            Row("Device");
            Right(CW());
            if (DeviceDropdown("##indev", s.audio.inputDeviceId, ins, CW())) ctx.ChangedAudio();
            RowEnd();
            Row("Volume");
            char v[16];
            snprintf(v, sizeof(v), "%d%%", (int)std::lround(s.audio.micVolume * 100.0f));
            Right(CW());
            ImGui::BeginGroup();
            if (Slider("##micvol", &s.audio.micVolume, 0.0f, 2.0f, v, CW(), 0.05f)) ctx.ChangedAudio();
            LevelMeter("##micmeter", ctx.levels.mic * s.audio.micVolume, ImVec2(CW(), S(6.0f)), ctx.levels.micGated);
            ImGui::EndGroup();
            RowEnd();
            ToggleRow("Push-to-talk", "Your mic is only recorded while you hold a key.", &s.audio.pushToTalk, ctx, &AppContext::ChangedAudio);
            if (s.audio.pushToTalk) {
                Row("Push-to-talk key", ctx.levels.micGated ? "Key up: mic muted." : "Key down: mic live.");
                const bool capturing = ctx.ui.settings.capturing == kPushToTalkSlot;
                Right(CW());
                if (HotkeyBinder("##pttkey", Hotkeys::KeyName(s.audio.pushToTalkKey), capturing ? BinderState::Capturing : BinderState::Normal, CW()))
                    StartCapture(ctx, kPushToTalkSlot);
                RowEnd();
            }
        }
    }
    EndRows();

    if (s.audio.captureMic) MicCleanup(ctx);

    if (Advanced(ctx, 2)) {
        if (BeginRows("##audadv")) {
            ToggleRow("Separate tracks", "Puts your mic on its own audio track so you can mix it later. Off: mixed into one track.",
                      &s.audio.separateTracks, ctx, &AppContext::ChangedAudio);
        }
        EndRows();
    }
}

// Mic clean-up: RNNoise noise suppression, noise gate (with its threshold on a live meter),
// high-pass, raw backup track, and a 5-second before/after test.
void MicCleanup(AppContext& ctx)
{
    Settings& s = ctx.settings;
    static engine::MicTest test;
    Group("Mic clean-up");
    if (BeginRows("##micfx")) {
        ToggleRow("Noise suppression", "Removes keyboard clatter, fans and background hum from your voice, live while you record. Runs on your PC.",
                  &s.audio.noiseSuppression, ctx, &AppContext::ChangedEngine);
        if (s.audio.noiseSuppression) {
            Row("Strength", "Low keeps a little of the room sound; High removes everything that isn't your voice.");
            static const char* str[] = { "Low", "Medium", "High" };
            Right(CW());
            if (Segmented("##nsstr", &s.audio.suppressionStrength, str, 3, CW())) ctx.ChangedEngine();
            RowEnd();
        }
        ToggleRow("Noise gate", "Silences the mic completely while you aren't talking.", &s.audio.noiseGate, ctx, &AppContext::ChangedEngine);
        if (s.audio.noiseGate) {
            Row("Gate opens above", "Talk normally: the meter should go past the white line, and stay under it while you're quiet.");
            char v[24];
            snprintf(v, sizeof(v), "%.0f dB", s.audio.gateDb);
            Right(CW());
            ImGui::BeginGroup();
            if (Slider("##gatedb", &s.audio.gateDb, -70.0f, -20.0f, v, CW(), 1.0f)) ctx.ChangedEngine();
            const ImVec2 mp = ImGui::GetCursorScreenPos();
            LevelMeter("##gatemeter", ctx.levels.mic * s.audio.micVolume, ImVec2(CW(), S(6.0f)), ctx.levels.micGated);
            const float x = mp.x + CW() * std::clamp((s.audio.gateDb + 60.0f) / 60.0f, 0.0f, 1.0f);   // meter: -60..0 dB
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, mp.y - S(3.0f)), ImVec2(x, mp.y + S(9.0f)), White(0.95f), S(2.0f));
            ImGui::EndGroup();
            RowEnd();
        }
        ToggleRow("Cut low rumble", "Filters out sound below 80 Hz: desk bumps, fans, traffic. Voices aren't affected.", &s.audio.highPass, ctx,
                  &AppContext::ChangedEngine);
        ToggleRow("Also keep the raw mic", "Records your mic a second time without clean-up, so you can switch back in the editor if it sounds wrong.",
                  &s.audio.keepRawMic, ctx, &AppContext::ChangedEngine);

        // Test: 5 s recording, played back as recorded, then cleaned.
        const engine::MicTestState ts = test.State();
        const bool busy = ts == engine::MicTestState::Recording || ts == engine::MicTestState::PlayingRaw || ts == engine::MicTestState::PlayingClean;
        const char* desc = "Records 5 seconds, then plays it back as recorded and then cleaned up.";
        std::string status;
        switch (ts) {
        case engine::MicTestState::Recording: status = "Recording\xE2\x80\xA6 say something, or tap your desk"; break;
        case engine::MicTestState::PlayingRaw: status = "Playing: as recorded"; break;
        case engine::MicTestState::PlayingClean: status = "Playing: cleaned up"; break;
        case engine::MicTestState::Done: status = "Done. Hear a difference? Try another strength."; break;
        case engine::MicTestState::Failed: status = test.Error(); break;
        default: break;
        }
        Row("Test mic", status.empty() ? desc : status.c_str());
        Right(CW());
        ImGui::BeginGroup();
        ButtonOpts o;
        o.style = busy ? Btn::Secondary : Btn::Primary;
        o.icon = busy ? ICON_STOP : ICON_MIC;
        o.size = ImVec2(CW(), S(Tok::ControlHeight));
        if (Button(busy ? "Stop test##mictest" : "Test mic##mictest", o)) {
            if (busy) test.Stop();
            else {
                engine::MicFx fx;
                fx.suppress = s.audio.noiseSuppression;
                fx.strength = s.audio.suppressionStrength;
                fx.gate = s.audio.noiseGate;
                fx.gateDb = s.audio.gateDb;
                fx.highPass = s.audio.highPass;
                test.Start(s.audio.inputDeviceId, fx);
            }
        }
        if (busy) {
            const ImVec2 pp = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(CW(), S(8.0f)));
            ProgressBar(ImGui::GetWindowDrawList(), ImVec2(pp.x, pp.y + S(4.0f)), ImVec2(pp.x + CW(), pp.y + S(8.0f)), test.Progress());
        }
        ImGui::EndGroup();
        RowEnd();
    }
    EndRows();
}

const char* HotkeyHelp(HotkeyAction a)
{
    switch (a) {
    case HotkeyAction::SaveReplay: return "Saves the replay buffer as a clip.";
    case HotkeyAction::ToggleRecording: return "Starts or stops a normal recording.";
    case HotkeyAction::ToggleReplayBuffer: return "Turns the replay buffer on or off.";
    case HotkeyAction::Screenshot: return "Captures the display your game is on.";
    case HotkeyAction::ScreenshotRegion: return "Freezes the screen so you can drag out an area.";
    case HotkeyAction::ScreenshotWindow: return "Captures just the active window.";
    default: return "";
    }
}

void HotkeysPage(AppContext& ctx)
{
    Settings& s = ctx.settings;
    Group("Global hotkeys (they work while a game has focus)");
    if (BeginRows("##hk")) {
        for (int i = 0; i < kHotkeyCount; ++i) {
            const HotkeyAction a = (HotkeyAction)i;
            ImGui::PushID(i);
            Row(HotkeyActionLabel(a), HotkeyHelp(a));
            const bool capturing = ctx.ui.settings.capturing == i;
            const HotkeyStatus status = ctx.hotkeys.Suspended() ? HotkeyStatus::Ok : ctx.hotkeys.Status(a);
            const BinderState state = capturing ? BinderState::Capturing
                                      : status == HotkeyStatus::Duplicate ? BinderState::Duplicate
                                      : status == HotkeyStatus::InUse ? BinderState::InUse
                                                                       : BinderState::Normal;
            Right(CW());
            if (HotkeyBinder("##bind", Hotkeys::ToString(s.hotkeys[(size_t)i]), state, CW())) StartCapture(ctx, i);
            RowEnd();
            ImGui::PopID();
        }
    }
    EndRows();
    if (Act("Reset to defaults", ICON_UNDO)) {
        s.hotkeys = Settings::DefaultHotkeys();
        ctx.ChangedHotkeys();
    }
    Gap(8.0f);
    TextWrapped("Tip: include Alt, Ctrl or Shift. Windows gives a registered hotkey to Replayr only, so a plain key like F9 would stop working in "
                "your game.",
                Pal().textFaint, ImGui::GetContentRegionAvail().x, GetFonts().regular, Type::Caption);
}

void Storage(AppContext& ctx)
{
    Settings& s = ctx.settings;
    Group("Where clips go");
    if (BeginRows("##where")) {
        Row("Clips folder", ToUtf8(s.ClipsFolder()).c_str());
        const float bw = (CW() - S(8.0f)) * 0.5f;
        Right(CW());
        if (Act("Change\xE2\x80\xA6", ICON_FOLDER, Btn::Secondary, ImVec2(bw, 0))) ctx.Do(Action::BrowseClipsFolder);
        ImGui::SameLine(0, S(8.0f));
        if (Act("Open", ICON_FOLDER_OPEN, Btn::Secondary, ImVec2(bw, 0))) ctx.Do(Action::OpenClipsFolder);
        RowEnd();
        {
            char used[96];
            snprintf(used, sizeof(used), "%s used  \xC2\xB7  %s free on this drive", FormatBytes(ctx.library.TotalBytes()).c_str(),
                     FormatBytes(ctx.system.diskFree).c_str());
            Row("Space", "Clips and screenshots on this PC.");
            Right(CW());
            Text(used, Pal().text, GetFonts().medium, Type::Body);
            RowEnd();
        }
        ToggleRow("A folder per game", "Clips go into a sub-folder named after the game.", &s.storage.perGameFolders, ctx, &AppContext::ChangedStorage);
        ToggleRow("Copy screenshots to the clipboard", "So you can paste a screenshot straight into a chat.", &s.storage.screenshotToClipboard, ctx,
                  &AppContext::Changed);
    }
    EndRows();

    if (Advanced(ctx, 4)) {
        if (BeginRows("##storadv")) {
            Row("Library size limit", "When the library is bigger, the oldest clips go to the Recycle Bin. Favourites are kept.");
            char v[32];
            if (s.storage.maxLibraryGB <= 0) snprintf(v, sizeof(v), "No limit");
            else snprintf(v, sizeof(v), "%d GB", s.storage.maxLibraryGB);
            Right(CW());
            if (SliderInt("##maxgb", &s.storage.maxLibraryGB, 0, 1000, v, 10, CW())) ctx.ChangedStorage();
            RowEnd();
            ToggleRow("Delete old clips", "Old clips go to the Recycle Bin. Favourites are kept.", &s.storage.autoDelete, ctx, &AppContext::ChangedStorage);
            if (s.storage.autoDelete) {
                Row("Older than");
                char d[32];
                snprintf(d, sizeof(d), "%d days", s.storage.autoDeleteDays);
                Right(CW());
                if (SliderInt("##days", &s.storage.autoDeleteDays, 1, 365, d, 1, CW())) ctx.ChangedStorage();
                RowEnd();
            }
            Row("Screenshot format", "PNG is lossless; JPG is much smaller.");
            static const char* fmts[] = { "PNG", "JPG" };
            Right(CW());
            if (Segmented("##fmt", &s.storage.screenshotFormat, fmts, 2, CW())) ctx.Changed();
            RowEnd();
        }
        EndRows();
    }
}

} // namespace

void DrawSettings(AppContext& ctx)
{
    SettingsState& st = ctx.ui.settings;
    st.category = Clamp(st.category, 0, kCategoryCount - 1);
    const float y0 = ImGui::GetCursorPosY(), x0 = ImGui::GetCursorPosX();
    PageHeader("Settings", "Changes save automatically.");
    // "Saved" confirmation that fades out.
    const float since = (float)(ImGui::GetTime() - st.savedAt);
    if (since >= 0.0f && since < Motion::SavedFlashSeconds) {
        const float a = since < 0.15f ? since / 0.15f : 1.0f - Clamp((since - 1.1f) / 0.5f, 0.0f, 1.0f);
        PushFont(GetFonts().medium, Type::Body);
        const char* t = "Saved";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        const ImVec2 p = ImGui::GetWindowPos();
        const float x = p.x + x0 + ImGui::GetContentRegionAvail().x - ts.x - S(4.0f), y = p.y + y0 + S(10.0f) - ImGui::GetScrollY();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Icon(dl, ICON_CHECK, ImVec2(x - S(14.0f), y + ts.y * 0.5f), S(12.0f), ColA(Pal().success, a));
        dl->AddText(ImVec2(x, y), ColA(Pal().success, a), t);
        PopFont();
    }
    Gap(14.0f);

    // Category column.
    const float catW = S(200.0f);
    const float top = ImGui::GetCursorPosY();
    ImGui::BeginGroup();
    for (int i = 0; i < kCategoryCount; ++i) {
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = S(40.0f);
        if (ImGui::InvisibleButton("##cat", ImVec2(catW, h))) st.category = i;
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImGuiID cid = ImGui::GetItemID();
        const bool clicked = ImGui::IsItemClicked();
        const float hv = Anim(cid, hov && st.category != i ? 1.0f : 0.0f, Motion::Hover);
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool sel = st.category == i;
        WaveFx(dl, cid, p, ImVec2(p.x + catW, p.y + h), S(10.0f), hov, clicked, Accent2());
        if (hv > 0.01f) dl->AddRectFilled(p, ImVec2(p.x + catW, p.y + h), White(0.07f * hv), S(10.0f));
        if (sel) {
            dl->AddRectFilled(p, ImVec2(p.x + catW, p.y + h), White(0.13f), S(10.0f));
            dl->AddRect(p, ImVec2(p.x + catW, p.y + h), White(0.16f), S(10.0f));
            dl->AddRectFilled(ImVec2(p.x + S(1.0f), p.y + h * 0.28f), ImVec2(p.x + S(4.0f), p.y + h * 0.72f), Accent2(), S(2.0f));
        }
        const ImU32 col = sel ? White(1.0f) : Mix(IM_COL32(255, 255, 255, 165), White(1.0f), hv);
        Icon(dl, kCategories[i].icon, ImVec2(p.x + S(22.0f), p.y + h * 0.5f), S(15.0f), col);
        PushFont(sel ? GetFonts().medium : GetFonts().regular, Type::Body);
        dl->AddText(ImVec2(p.x + S(44.0f), p.y + (h - ImGui::GetFontSize()) * 0.5f), col, kCategories[i].name);
        PopFont();
    }
    ImGui::EndGroup();

    // Content.
    ImGui::SetCursorPos(ImVec2(x0 + catW + S(24.0f), top));
    const float cw = std::min(S(860.0f), ImGui::GetContentRegionAvail().x);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(cw);
    ImGui::BeginChild("##setcontent", ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    switch (st.category) {
    case 0: General(ctx); break;
    case 1: Recording(ctx); break;
    case 2: Audio(ctx); break;
    case 3: HotkeysPage(ctx); break;
    default: Storage(ctx); break;
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    Gap(24.0f);
}

} // namespace cs::ui
