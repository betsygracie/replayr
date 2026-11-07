// Uploads: the signed-in user's cloud clips: copy link, who can see it, views, delete.
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Platform.h"
#include "core/Window.h"
#include "core/Util.h"

#include <algorithm>
#include <cstdio>

namespace cs::ui {

namespace {
const char* kPrivacy[] = { "Public", "Unlisted", "Private" };
const char* kPrivacyKeys[] = { "public", "unlisted", "private" };
const char* kPrivacyHelp[] = { "Anyone can find and watch it", "Only people with the link", "Only you" };

int PrivacyIndex(const std::string& p)
{
    for (int i = 0; i < 3; ++i)
        if (p == kPrivacyKeys[i]) return i;
    return 1;
}
} // namespace

void UploadClip(AppContext& ctx, const ClipInfo& clip)
{
    if (!ctx.account) return;
    if (ctx.account->State() != AccountState::SignedIn) {
        Snack(ctx.ui, "Sign in with Discord or Steam first (bottom left) to upload.", L"", true);
        return;
    }
    const AccountUser u = ctx.account->User();
    if (u.quotaBytes > 0 && u.usedBytes + clip.sizeBytes > u.quotaBytes) {
        Snack(ctx.ui, "Not enough cloud space. Delete an upload in Uploads, or export a smaller version.", L"", true);
        return;
    }
    if (clip.sizeBytes > 95ull * 1024 * 1024) {
        Snack(ctx.ui, "Uploads can be up to 95 MB. Open it in the editor and export with \"Fit under a size\".", L"", true);
        return;
    }
    ctx.account->Upload(clip.path, clip.title, clip.duration, "unlisted");
    Snack(ctx.ui, "Uploading \"" + clip.title + "\"\xE2\x80\xA6 the link is copied when it's done");
}

void DrawUploads(AppContext& ctx)
{
    Account* acc = ctx.account;
    if (!acc || acc->State() != AccountState::SignedIn) {
        PageHeader("Uploads");
        EmptyState(ctx, "Sign in to upload", "Use the Discord or Steam button at the bottom left. Then right-click any clip > Upload & copy link.", false);
        return;
    }
    const AccountUser u = acc->User();
    const float W = ImGui::GetContentRegionAvail().x;
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    PageHeader("Uploads");
    // Cloud space, top right.
    {
        const float bw = S(260.0f);
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 a(p.x + x0 + W - bw, p.y + y0 + S(4.0f) - ImGui::GetScrollY());
        char line[96];
        snprintf(line, sizeof(line), "%s of %s cloud space", FormatBytes(u.usedBytes).c_str(), FormatBytes(u.quotaBytes).c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        PushFont(GetFonts().medium, Type::Caption);
        dl->AddText(a, Pal().textDim, line);
        PopFont();
        const float frac = u.quotaBytes ? (float)((double)u.usedBytes / (double)u.quotaBytes) : 0.0f;
        ProgressBar(dl, ImVec2(a.x, a.y + S(22.0f)), ImVec2(a.x + bw, a.y + S(28.0f)), frac);
    }
    Gap(6.0f);

    // In-progress uploads.
    for (const UploadItem& up : acc->Uploads()) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 b(p.x + W, p.y + S(52.0f));
        Glass(dl, p, b, S(Tok::RadiusCard), 0.0f, Tok::GlassBorderSoft, 0, 2);
        Icon(dl, ICON_UPLOAD, ImVec2(p.x + S(26.0f), p.y + S(26.0f)), S(16.0f), Accent2());
        PushFont(GetFonts().medium, Type::Body);
        dl->AddText(ImVec2(p.x + S(50.0f), p.y + S(9.0f)), Pal().text, up.title.c_str());
        PopFont();
        char pct[48];
        snprintf(pct, sizeof(pct), up.active ? "Uploading %d%%" : "Waiting", (int)(up.progress * 100.0f));
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(b.x - S(16.0f) - ImGui::CalcTextSize(pct).x, p.y + S(11.0f)), Pal().textDim, pct);
        PopFont();
        ProgressBar(dl, ImVec2(p.x + S(50.0f), p.y + S(34.0f)), ImVec2(b.x - S(16.0f), p.y + S(39.0f)), up.progress);
        ImGui::Dummy(ImVec2(W, S(52.0f)));
        Gap(4.0f);
    }

    const std::vector<CloudClip> clips = acc->Clips();
    if (clips.empty()) {
        if (acc->ClipsLoading()) {
            Gap(30.0f);
            Spinner(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetCursorScreenPos().x + W * 0.5f, ImGui::GetCursorScreenPos().y), S(12.0f), Accent2());
            ImGui::Dummy(ImVec2(W, S(20.0f)));
        } else if (acc->Uploads().empty()) {
            EmptyState(ctx, "Nothing uploaded yet", "Right-click a clip and choose \"Upload & copy link\". The link goes on your clipboard.", false);
        }
        return;
    }
    // One glass row per upload.
    for (size_t i = 0; i < clips.size(); ++i) {
        const CloudClip& c = clips[i];
        ImGui::PushID(c.id.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = S(64.0f);
        const ImVec2 b(p.x + W, p.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool hov = ImGui::IsMouseHoveringRect(p, b);
        const float hv = AnimHover(ImGui::GetID("##row"), hov);
        Glass(dl, p, b, S(Tok::RadiusCard), hv * 0.5f, Tok::GlassBorderSoft, 0, 2);
        const int pi = PrivacyIndex(c.privacy);
        Icon(dl, pi == 0 ? ICON_GLOBE : pi == 1 ? ICON_LINK : ICON_LOCK, ImVec2(p.x + S(28.0f), p.y + h * 0.5f), S(17.0f), Accent2());
        const float actionsW = S(420.0f);
        PushFont(GetFonts().medium, Type::Body);
        const std::string title = Ellipsize(c.title.c_str(), W - actionsW - S(70.0f));
        dl->AddText(ImVec2(p.x + S(54.0f), p.y + S(12.0f)), Pal().text, title.c_str());
        PopFont();
        char meta[160];
        snprintf(meta, sizeof(meta), "%s  \xC2\xB7  %lld views  \xC2\xB7  %s  \xC2\xB7  %s", FormatDuration(c.duration).c_str(), (long long)c.views,
                 FormatBytes(c.sizeBytes).c_str(), RelativeTime(c.createdAt).c_str());
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(p.x + S(54.0f), p.y + S(36.0f)), Pal().textFaint, meta);
        PopFont();

        // Actions: who can see it, copy link, open, delete.
        float x = b.x - S(14.0f);
        const float bs = S(34.0f), cy = p.y + (h - bs) * 0.5f;
        auto iconBtn = [&](const char* id, const char* icon, const char* tip, Btn style) {
            x -= bs;
            ImGui::SetCursorScreenPos(ImVec2(x, cy));
            ButtonOpts o;
            o.style = style;
            o.icon = icon;
            o.size = ImVec2(bs, bs);
            o.round = true;
            o.tooltip = tip;
            const bool r = Button(id, o);
            x -= S(6.0f);
            return r;
        };
        if (iconBtn("##del", ICON_DELETE, "Delete from the cloud (your local copy stays)", Btn::Ghost)) acc->DeleteClip(c.id);
        if (iconBtn("##open", ICON_OPEN_EXTERNAL, "Open in your browser", Btn::Ghost)) platform::OpenUrl(ToWide(c.url));
        x -= S(126.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, cy));
        ButtonOpts copy;
        copy.style = Btn::Secondary;
        copy.icon = ICON_LINK;
        copy.size = ImVec2(S(126.0f), bs);
        if (Button("Copy link", copy)) {
            platform::CopyTextToClipboard(ctx.window.Hwnd(), c.url);
            Snack(ctx.ui, "Link copied");
        }
        x -= S(10.0f) + S(130.0f);
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - S(Tok::ControlHeight)) * 0.5f));
        int sel = pi;
        if (Dropdown("##privacy", &sel, kPrivacy, 3, S(130.0f)) && sel != pi) acc->SetPrivacy(c.id, kPrivacyKeys[sel]);
        Tooltip(kPrivacyHelp[pi]);
        ImGui::SetCursorScreenPos(ImVec2(p.x, b.y + S(8.0f)));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    Gap(20.0f);
}

} // namespace cs::ui
