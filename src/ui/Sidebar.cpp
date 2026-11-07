// Sidebar: logo, four pages (Editor only while a clip is open), storage and the Save clip button.
// Collapses to icons only when the window is narrow.
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Hotkeys.h"
#include "core/Util.h"
#include "media/MediaCache.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cs::ui {

namespace {

// The logo: a gradient ring with a gap and a white dot.
void Logo(ImDrawList* dl, ImVec2 c, float r)
{
    const int n = 40;
    const float a0 = 0.9f, a1 = 0.9f + 5.3f;
    for (int i = 0; i < n; ++i) {
        const float t0 = (float)i / n, t1 = (float)(i + 1) / n;
        dl->PathArcTo(c, r, a0 + (a1 - a0) * t0, a0 + (a1 - a0) * t1, 3);
        dl->PathStroke(Mix(Accent(), Accent2(), t0), 0, r * 0.42f);
    }
    dl->AddCircleFilled(c, r * 0.28f, White(1.0f));
}

// The white "Save clip  Alt F10" button at the bottom of the card.
void SaveClipButton(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float bw, float bh)
{
    const std::string saveKey = HotkeyText(ctx, HotkeyAction::SaveReplay);
    ImGui::SetCursorScreenPos(pos);
    ButtonOpts o;
    o.style = Btn::White;
    o.size = ImVec2(bw, bh);
    o.tooltip = "Saves the last part of the replay buffer as a clip";
    if (Button("##saveclip", o)) ctx.Do(Action::SaveReplay);
    PushFont(GetFonts().semibold, Type::Body);
    const char* label = "Save clip";
    const float lw = ImGui::CalcTextSize(label).x;
    const float kh = S(20.0f), kw = KeyCapsWidth(saveKey, kh);
    const float total = lw + (kw > 0 ? kw + S(10.0f) : 0.0f);
    const float lx = pos.x + (bw - total) * 0.5f;
    dl->AddText(ImVec2(lx, pos.y + (bh - ImGui::GetFontSize()) * 0.5f), IM_COL32(26, 12, 48, 255), label);
    PopFont();
    if (kw > 0) KeyCaps(dl, ImVec2(lx + lw + S(10.0f), pos.y + (bh - kh) * 0.5f), saveKey, kh, true);
}

// Round avatar: the Discord/Steam picture once it's downloaded, else the first letter.
void Avatar(AppContext& ctx, ImDrawList* dl, ImVec2 c, float r, const AccountUser& u)
{
    dl->AddCircleFilled(ImVec2(c.x, c.y + S(1.5f)), r + S(1.0f), IM_COL32(20, 0, 40, 90), 40);
    const std::wstring path = ctx.account->AvatarPath();
    const ImageInfo img = path.empty() ? ImageInfo{} : ctx.images.Get(path);
    if (img.texture && !img.failed) {
        dl->AddImageRounded((ImTextureID)img.texture, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r);
    } else {
        GradientRect(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(255, 255, 255, 235), IM_COL32(225, 215, 255, 235), r);
        char ch[2] = { u.name.empty() ? '?' : (char)toupper((unsigned char)u.name[0]), 0 };
        PushFont(GetFonts().semibold, Type::Body);
        const ImVec2 ts = ImGui::CalcTextSize(ch);
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(60, 20, 120, 255), ch);
        PopFont();
    }
    dl->AddCircle(c, r, White(0.85f), 40, S(1.5f));
}

void AccountMenu(AppContext& ctx)
{
    if (BeginMenu("##accountmenu")) {
        Account* acc = ctx.account;
        if (acc->State() == AccountState::SignedIn) {
            const AccountUser u = acc->User();
            MenuLabel(u.name.c_str());
            if (MenuItem(ICON_CLOUD, "My uploads")) GoTo(ctx, Page::Uploads);
            MenuSeparator();
            if (MenuItem(ICON_SIGNOUT, "Sign out")) acc->SignOut();
        } else {
            MenuLabel("Sign in to share clips");
            if (MenuItem(ICON_PERSON, "Continue with Discord")) acc->SignIn("discord");
            if (MenuItem(ICON_PERSON, "Continue with Steam")) acc->SignIn("steam");
        }
        EndMenu();
    }
}

void AccountCard(AppContext& ctx, ImDrawList* dl, ImVec2 a, ImVec2 b, float pad, bool collapsed)
{
    Account* acc = ctx.account;
    const AccountState st = acc->State();
    const AccountUser u = acc->User();
    const std::vector<UploadItem> ups = acc->Uploads();
    const float cardH = collapsed ? S(132.0f) : (st == AccountState::SignedIn ? S(182.0f) : S(196.0f));
    const ImVec2 ca(a.x + pad, b.y - pad - cardH), cb(b.x - pad, b.y - pad);
    const float cr = S(Tok::RadiusCard + 2.0f);
    GradientRect(dl, ca, cb, Accent(1.0f), Accent2(1.0f), cr);
    GradientRectH(dl, ca, cb, Accent(0.9f), Accent2(0.0f), cr);
    GradientRect(dl, ca, ImVec2(cb.x, ca.y + cardH * 0.5f), White(0.14f), White(0.0f), cr, ImDrawFlags_RoundCornersTop);
    dl->AddRect(ca, cb, White(0.22f), cr);

    if (collapsed) {
        const ImVec2 c((ca.x + cb.x) * 0.5f, ca.y + S(38.0f));
        const float r = S(19.0f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
        if (ImGui::InvisibleButton("##acct", ImVec2(r * 2.0f, r * 2.0f))) ImGui::OpenPopup("##accountmenu");
        Tooltip(st == AccountState::SignedIn ? u.name.c_str() : "Sign in to share clips");
        if (st == AccountState::SignedIn) Avatar(ctx, dl, c, r, u);
        else if (st == AccountState::SigningIn) Spinner(dl, c, S(10.0f), White(1.0f));
        else {
            dl->AddCircleFilled(c, r, White(0.22f), 40);
            Icon(dl, ICON_PERSON, c, S(18.0f), White(1.0f));
        }
        const float bs = S(40.0f);
        ImGui::SetCursorScreenPos(ImVec2(c.x - bs * 0.5f, cb.y - bs - S(14.0f)));
        ButtonOpts o;
        o.style = Btn::White;
        o.icon = ICON_SAVE;
        o.size = ImVec2(bs, bs);
        o.round = true;
        const std::string tip = "Save clip  " + HotkeyText(ctx, HotkeyAction::SaveReplay);
        o.tooltip = tip.c_str();
        if (Button("##savecol", o)) ctx.Do(Action::SaveReplay);
        AccountMenu(ctx);
        return;
    }

    const float ix = ca.x + S(16.0f), iw = cb.x - S(16.0f) - ix;
    float y = ca.y + S(14.0f);
    if (st == AccountState::SignedIn) {
        // Avatar + name; the whole row opens the account menu.
        const float r = S(18.0f);
        ImGui::SetCursorScreenPos(ImVec2(ix - S(4.0f), y - S(4.0f)));
        const bool open = ImGui::InvisibleButton("##acctrow", ImVec2(iw + S(8.0f), r * 2.0f + S(8.0f)));
        const bool hov = ImGui::IsItemHovered();
        const ImGuiID rid = ImGui::GetItemID();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float hv = AnimHover(rid, hov);
        if (hv > 0.01f) dl->AddRectFilled(ImVec2(ix - S(6.0f), y - S(5.0f)), ImVec2(ix + iw + S(6.0f), y + r * 2.0f + S(5.0f)), White(0.14f * hv), S(10.0f));
        WaveFx(dl, rid, ImVec2(ix - S(6.0f), y - S(5.0f)), ImVec2(ix + iw + S(6.0f), y + r * 2.0f + S(5.0f)), S(10.0f), hov, open, White(1.0f));
        if (open) ImGui::OpenPopup("##accountmenu");
        Avatar(ctx, dl, ImVec2(ix + r, y + r), r, u);
        PushFont(GetFonts().semibold, Type::Body);
        const std::string name = Ellipsize(u.name.c_str(), iw - r * 2.0f - S(30.0f));
        dl->AddText(ImVec2(ix + r * 2.0f + S(10.0f), y + S(1.0f)), White(1.0f), name.c_str());
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(ix + r * 2.0f + S(10.0f), y + S(20.0f)), White(0.8f), u.provider == "steam" ? "Steam" : "Discord");
        PopFont();
        Icon(dl, ICON_CHEVRON_DOWN, ImVec2(ix + iw - S(6.0f), y + r), S(11.0f), White(0.8f));
        y += r * 2.0f + S(16.0f);
        // Cloud space, or the current upload.
        char line[96];
        float frac;
        if (!ups.empty()) {
            const UploadItem& up = ups.front();
            snprintf(line, sizeof(line), ups.size() > 1 ? "Uploading %d%% (%d more)" : "Uploading %d%%", (int)(up.progress * 100.0f), (int)ups.size() - 1);
            frac = up.progress;
        } else {
            snprintf(line, sizeof(line), "%s of %s in the cloud", FormatBytes(u.usedBytes).c_str(), FormatBytes(u.quotaBytes).c_str());
            frac = u.quotaBytes ? (float)((double)u.usedBytes / (double)u.quotaBytes) : 0.0f;
        }
        ProgressBar(dl, ImVec2(ix, y), ImVec2(ix + iw, y + S(6.0f)), frac, true);
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(ix, y + S(13.0f)), White(0.9f), line);
        PopFont();
    } else if (st == AccountState::SigningIn) {
        PushFont(GetFonts().semibold, Type::Heading);
        dl->AddText(ImVec2(ix, y), White(1.0f), "Check your browser");
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(ix, y + S(28.0f)), White(0.85f), "Finish signing in there.");
        PopFont();
        Spinner(dl, ImVec2(ix + iw - S(10.0f), y + S(12.0f)), S(8.0f), White(1.0f));
        ImGui::SetCursorScreenPos(ImVec2(ix, y + S(58.0f)));
        ButtonOpts cancel;
        cancel.style = Btn::Ghost;
        cancel.size = ImVec2(iw, S(34.0f));
        if (Button("Cancel##signin", cancel)) acc->CancelSignIn();
    } else {
        PushFont(GetFonts().semibold, Type::Heading);
        dl->AddText(ImVec2(ix, y), White(1.0f), "Share your clips");
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(ix, y + S(28.0f)), White(0.85f), "Sign in to upload and get links.");
        PopFont();
        const float bw = (iw - S(8.0f)) * 0.5f, bh = S(34.0f);
        ImGui::SetCursorScreenPos(ImVec2(ix, y + S(56.0f)));
        ButtonOpts d;
        d.style = Btn::Secondary;
        d.size = ImVec2(bw, bh);
        d.tooltip = "Sign in with your Discord account (opens your browser)";
        if (Button("Discord", d)) acc->SignIn("discord");
        ImGui::SetCursorScreenPos(ImVec2(ix + bw + S(8.0f), y + S(56.0f)));
        ButtonOpts st2 = d;
        st2.tooltip = "Sign in with your Steam account (opens your browser)";
        if (Button("Steam", st2)) acc->SignIn("steam");
    }
    const float bh = S(42.0f);
    SaveClipButton(ctx, dl, ImVec2(ix, cb.y - bh - S(16.0f)), iw, bh);
    AccountMenu(ctx);
}

} // namespace

void DrawSidebar(AppContext& ctx, float width, float height)
{
    UiState& ui = ctx.ui;
    const bool collapsed = width < S(Tok::SidebarWidth - 1.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 a = p, b(p.x + width, p.y + height);
    ImDrawList* bg = ImGui::GetWindowDrawList();
    Shadow(bg, a, b, S(Tok::RadiusPanel), 0.8f);
    Glass(bg, a, b, S(Tok::RadiusPanel), 0.0f, Tok::GlassBorder);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##sidebar", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = collapsed ? S(10.0f) : S(16.0f);

    // Header: logo + name, then a divider (as in the reference).
    const float headerH = S(72.0f);
    if (collapsed) {
        Logo(dl, ImVec2(a.x + width * 0.5f, a.y + headerH * 0.5f), S(12.0f));
    } else {
        Logo(dl, ImVec2(a.x + S(34.0f), a.y + headerH * 0.5f), S(11.0f));
        PushFont(GetFonts().semibold, 19.0f);
        dl->AddText(ImVec2(a.x + S(56.0f), a.y + headerH * 0.5f - ImGui::GetFontSize() * 0.55f), Pal().text, "Replayr");
        PopFont();
    }
    dl->AddLine(ImVec2(a.x, a.y + headerH), ImVec2(b.x, a.y + headerH), White(Tok::GlassBorder));

    // Navigation.
    struct Item {
        const char* icon;
        const char* label;
        Page page;
    };
    std::vector<Item> items = { { ICON_VIDEO, "Clips", Page::Clips }, { ICON_CAMERA, "Screenshots", Page::Screenshots } };
    if (ui.editor.hasClip) items.push_back({ ICON_TRIM, "Editor", Page::Editor });
    if (ctx.account && ctx.account->State() == AccountState::SignedIn) items.push_back({ ICON_CLOUD, "Uploads", Page::Uploads });
    items.push_back({ ICON_SETTINGS, "Settings", Page::Settings });

    const float itemH = S(Tok::NavItemHeight + 4.0f), step = itemH + S(4.0f);
    const float top = headerH + S(16.0f);
    int selected = -1;
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].page == ui.page) selected = (int)i;
    // The highlight slides between items.
    const ImGuiID pillId = ImGui::GetID("##navpill");
    // The highlight springs between items (a little overshoot), and glows.
    const float pillY = selected >= 0 ? Spring(pillId, top + step * selected, Springs::Pill) : Anim(pillId, 0.0f, 0.0f);
    const float pillA = Anim(ImGui::GetID("##navpillA"), selected >= 0 ? 1.0f : 0.0f, Motion::Slide);
    if (pillA > 0.01f) {
        const ImVec2 pa(a.x + pad, a.y + pillY), pb(b.x - pad, a.y + pillY + itemH);
        const float stretch = std::min(S(6.0f), std::fabs(SpringVelocity(pillId)) * 0.012f);   // stretches while moving
        const ImVec2 sa(pa.x, pa.y - stretch * 0.5f), sb(pb.x, pb.y + stretch * 0.5f);
        RadialGlow(dl, ImVec2(sa.x + S(20.0f), (sa.y + sb.y) * 0.5f), S(70.0f), Accent2(0.16f * pillA), 40);
        Glass(dl, sa, sb, S(14.0f), 0.6f * pillA, Tok::GlassBorder, 0, 2);
        GradientRectH(dl, sa, sb, Accent(0.22f * pillA), Accent2(0.0f), S(14.0f));
        dl->AddRectFilled(ImVec2(pa.x + S(1.0f), pa.y + itemH * 0.28f), ImVec2(pa.x + S(4.0f), pb.y - itemH * 0.28f), Accent2(pillA), S(2.0f));
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        const float iy = top + step * i;
        ImGui::SetCursorPos(ImVec2(pad, iy));
        ImGui::PushID((int)i);
        const bool clicked = ImGui::InvisibleButton("##nav", ImVec2(width - pad * 2.0f, itemH));
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiID nid = ImGui::GetItemID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (collapsed) Tooltip(it.label);
        const float hv = Anim(nid, hovered && (int)i != selected ? 1.0f : 0.0f, Motion::Hover);
        ImGui::PopID();
        const ImVec2 ia(a.x + pad, a.y + iy), ib(b.x - pad, a.y + iy + itemH);
        if (hv > 0.01f) dl->AddRectFilled(ia, ib, White(0.07f * hv), S(14.0f));
        WaveFx(dl, nid, ia, ib, S(14.0f), hovered, clicked, Accent2());
        const bool sel = (int)i == selected;
        const ImU32 col = sel ? White(1.0f) : Mix(IM_COL32(255, 255, 255, 165), White(1.0f), hv);
        if (collapsed) {
            Icon(dl, it.icon, ImVec2((ia.x + ib.x) * 0.5f, (ia.y + ib.y) * 0.5f), S(19.0f), col);
        } else {
            Icon(dl, it.icon, ImVec2(ia.x + S(24.0f), (ia.y + ib.y) * 0.5f), S(19.0f), col);
            PushFont(sel ? GetFonts().medium : GetFonts().regular, 15.5f);
            dl->AddText(ImVec2(ia.x + S(50.0f), (ia.y + ib.y) * 0.5f - ImGui::GetFontSize() * 0.5f), col, it.label);
            PopFont();
        }
        if (clicked) {
            if (ui.viewer.open) CloseImage(ctx);
            GoTo(ctx, it.page);
        }
    }

    if (ctx.account) {
        // Accounts are on: the card shows who's signed in (or the sign-in buttons) instead of disk space.
        AccountCard(ctx, dl, a, b, pad, collapsed);
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::EndChild();
        return;
    }
    // Storage + Save clip, on a gradient card like "Cloud Storage" in the reference.
    const uint64_t used = ctx.library.TotalBytes();
    const uint64_t limit = ctx.settings.storage.maxLibraryGB > 0 ? (uint64_t)ctx.settings.storage.maxLibraryGB << 30 : used + ctx.system.diskFree;
    const float frac = limit > 0 ? (float)((double)used / (double)limit) : 0.0f;
    const std::string saveKey = HotkeyText(ctx, HotkeyAction::SaveReplay);
    const float cardH = collapsed ? S(132.0f) : S(170.0f);
    const ImVec2 ca(a.x + pad, b.y - pad - cardH), cb(b.x - pad, b.y - pad);
    const float cr = S(Tok::RadiusCard + 2.0f);
    GradientRect(dl, ca, cb, Accent(1.0f), Accent2(1.0f), cr);
    GradientRectH(dl, ca, cb, Accent(0.9f), Accent2(0.0f), cr);
    GradientRect(dl, ca, ImVec2(cb.x, ca.y + cardH * 0.5f), White(0.14f), White(0.0f), cr, ImDrawFlags_RoundCornersTop);
    dl->AddRect(ca, cb, White(0.22f), cr);
    if (collapsed) {
        const ImVec2 rc((ca.x + cb.x) * 0.5f, ca.y + S(38.0f));
        const float rr = S(21.0f);
        dl->AddCircle(rc, rr, IM_COL32(40, 0, 80, 120), 48, S(3.5f));
        dl->PathArcTo(rc, rr, -1.5708f, -1.5708f + 6.2832f * Clamp(frac, 0.0f, 1.0f), 48);
        dl->PathStroke(White(1.0f), 0, S(3.5f));
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(frac * 100.0f));
        PushFont(GetFonts().semibold, Type::Caption);
        const ImVec2 ps = ImGui::CalcTextSize(pct);
        dl->AddText(ImVec2(rc.x - ps.x * 0.5f, rc.y - ps.y * 0.5f), White(1.0f), pct);
        PopFont();
        const float bs = S(40.0f);
        ImGui::SetCursorScreenPos(ImVec2(rc.x - bs * 0.5f, cb.y - bs - S(14.0f)));
        ButtonOpts o;
        o.style = Btn::White;
        o.icon = ICON_SAVE;
        o.size = ImVec2(bs, bs);
        o.round = true;
        const std::string tip = "Save clip  " + saveKey;
        o.tooltip = tip.c_str();
        if (Button("##savecol", o)) ctx.Do(Action::SaveReplay);
    } else {
        const float ix = ca.x + S(16.0f);
        PushFont(GetFonts().semibold, Type::Heading);
        dl->AddText(ImVec2(ix, ca.y + S(14.0f)), White(1.0f), "Storage");
        PopFont();
        ProgressBar(dl, ImVec2(ix, ca.y + S(46.0f)), ImVec2(cb.x - S(16.0f), ca.y + S(52.0f)), frac, true);
        char line[96];
        snprintf(line, sizeof(line), "%s of %s used", FormatBytes(used).c_str(), FormatBytes(limit).c_str());
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(ix, ca.y + S(60.0f)), White(0.9f), line);
        PopFont();
        const float bh = S(42.0f), bw = cb.x - S(16.0f) - ix;
        ImGui::SetCursorScreenPos(ImVec2(ix, cb.y - bh - S(16.0f)));
        const ImVec2 bp = ImGui::GetCursorScreenPos();
        ButtonOpts o;
        o.style = Btn::White;
        o.size = ImVec2(bw, bh);
        o.tooltip = "Saves the last part of the replay buffer as a clip";
        if (Button("##saveclip", o)) ctx.Do(Action::SaveReplay);
        PushFont(GetFonts().semibold, Type::Body);
        const char* label = "Save clip";
        const float lw = ImGui::CalcTextSize(label).x;
        const float kh = S(20.0f), kw = KeyCapsWidth(saveKey, kh);
        const float total = lw + (kw > 0 ? kw + S(10.0f) : 0.0f);
        const float lx = bp.x + (bw - total) * 0.5f;
        dl->AddText(ImVec2(lx, bp.y + (bh - ImGui::GetFontSize()) * 0.5f), IM_COL32(26, 12, 48, 255), label);
        PopFont();
        if (kw > 0) KeyCaps(dl, ImVec2(lx + lw + S(10.0f), bp.y + (bh - kh) * 0.5f), saveKey, kh, true);
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
}

} // namespace cs::ui
