// Shared library pieces: snapshot access, the card grid, the clip menu, rename/delete dialogs,
// the snackbar and the empty state.
#include "ui/AppContext.h"
#include "media/ClipAudio.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Hotkeys.h"
#include "core/MediaPlayer.h"
#include "core/Thumbnailer.h"
#include "core/Util.h"
#include "core/Window.h"
#include "media/MediaCache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace cs::ui {

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

void GoTo(AppContext& ctx, Page page)
{
    UiState& ui = ctx.ui;
    if (ui.page == page) return;
    if (ui.page == Page::Editor) ctx.player.Close();   // free the decoder and its texture
    ui.page = page;
    ui.pageChangedAt = ImGui::GetTime();
    if (page == Page::Uploads && ctx.account) ctx.account->RefreshClips();
    if (page == Page::Clips || page == Page::Screenshots) {
        ctx.settings.ui.lastPage = (int)page;
        ctx.Changed();
    }
}

std::shared_ptr<const ClipList> LibrarySnapshot(AppContext& ctx)
{
    LibraryState& lib = ctx.ui.library;
    const uint64_t v = ctx.library.Version();
    if (!lib.snapshot || v != lib.version) {
        lib.snapshot = ctx.library.Snapshot();
        lib.version = v;
    }
    return lib.snapshot;
}

const ClipInfo* FindClip(AppContext& ctx, uint64_t id)
{
    auto snap = LibrarySnapshot(ctx);
    if (!snap) return nullptr;
    for (const ClipInfo& c : *snap)
        if (c.id == id) return &c;
    return nullptr;
}

std::string RelativeTime(int64_t t)
{
    const int64_t d = NowUnix() - t;
    char buf[64];
    if (d < 60) return "Just now";
    if (d < 3600) {
        snprintf(buf, sizeof(buf), "%dm ago", (int)(d / 60));
        return buf;
    }
    if (d < 86400) {
        snprintf(buf, sizeof(buf), "%dh ago", (int)(d / 3600));
        return buf;
    }
    if (d < 2 * 86400) return "Yesterday";
    if (d < 7 * 86400) {
        snprintf(buf, sizeof(buf), "%dd ago", (int)(d / 86400));
        return buf;
    }
    time_t tt = (time_t)t;
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(buf, sizeof(buf), "%d %s", lt.tm_mday, months[lt.tm_mon % 12]);
    return buf;
}

std::string GameName(const ClipInfo& c) { return c.game.empty() ? "Desktop" : c.game; }

std::string HotkeyText(const AppContext& ctx, HotkeyAction a)
{
    const Hotkey& h = ctx.settings.hotkeys[(size_t)a];
    return h.IsSet() ? Hotkeys::ToString(h) : std::string();
}

BannerStyle BannerStyleFor(const Settings& s)
{
    BannerStyle b;
    b.corner = s.banner.corner;
    b.mode = s.banner.mode == 1 ? BannerMode::Always : BannerMode::StartFiveSeconds;
    for (int i = 0; i < 3; ++i) {
        b.accent[i] = s.general.accent[i];
        b.accent2[i] = s.general.accent2[i];
    }
    return b;
}

void Snack(UiState& ui, const std::string& text, const std::wstring& revealPath, bool error)
{
    ui.snack.text = text;
    ui.snack.revealPath = revealPath;
    ui.snack.error = error;
    ui.snack.at = ImGui::GetTime();
}

void DrawSnack(AppContext& ctx)
{
    Snackbar& sn = ctx.ui.snack;
    const float life = sn.revealPath.empty() ? 2.4f : 5.0f;
    const float t = (float)(ImGui::GetTime() - sn.at);
    if (t < 0.0f || t > life || sn.text.empty()) return;
    const float in = EaseOutCubic(t / 0.2f), out = 1.0f - Clamp((t - (life - 0.3f)) / 0.3f, 0.0f, 1.0f);
    const float a = std::min(in, out);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    PushFont(GetFonts().medium, Type::Body);
    const ImVec2 ts = ImGui::CalcTextSize(sn.text.c_str());
    const float linkW = sn.revealPath.empty() ? 0.0f : ImGui::CalcTextSize("Show in folder").x + S(28.0f);
    const float w = ts.x + S(56.0f) + linkW, h = S(44.0f);
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + vp->Size.y - h - S(28.0f) + S(14.0f) * (1.0f - in));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
    ImGui::Begin("##snack", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pa = ImGui::GetWindowPos(), pb(pa.x + w, pa.y + h);
    Shadow(dl, pa, pb, h * 0.5f, 1.2f);
    dl->AddRectFilled(pa, pb, Pal().surface, h * 0.5f);
    GradientRect(dl, pa, pb, White(0.08f), White(0.02f), h * 0.5f);
    dl->AddRect(pa, pb, White(Tok::GlassBorder), h * 0.5f);
    Icon(dl, sn.error ? ICON_WARNING : ICON_CHECK, ImVec2(pa.x + S(24.0f), pa.y + h * 0.5f), S(14.0f), sn.error ? Pal().danger : Pal().success);
    dl->AddText(ImVec2(pa.x + S(42.0f), pa.y + (h - ts.y) * 0.5f), Pal().text, sn.text.c_str());
    if (!sn.revealPath.empty()) {
        ImGui::SetCursorPos(ImVec2(w - linkW - S(6.0f), (h - S(32.0f)) * 0.5f));
        ButtonOpts o;
        o.style = Btn::Ghost;
        o.size = ImVec2(linkW, S(32.0f));
        o.round = true;
        if (Button("Show in folder##snack", o)) {
            platform::RevealInExplorer(sn.revealPath);
            sn.at = ImGui::GetTime() - life + 0.3f;   // fade out
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    PopFont();
}

// ---------------------------------------------------------------------------------------------
// Opening things
// ---------------------------------------------------------------------------------------------

void OpenEditor(AppContext& ctx, const ClipInfo& clip)
{
    EditorState& e = ctx.ui.editor;
    if (!e.hasClip || e.clip.id != clip.id) {
        e = EditorState{};
        e.hasClip = true;
        e.clip = clip;
        e.quality = ctx.settings.editor.exportQuality;
        e.banner = ctx.settings.banner.enabled;
        snprintf(e.title, sizeof(e.title), "%s", clip.title.c_str());
        snprintf(e.exportName, sizeof(e.exportName), "%s (edit)", clip.title.c_str());
        ctx.library.MarkOpened(clip.path);
    }
    CloseImage(ctx);
    GoTo(ctx, Page::Editor);
}

void LeaveEditor(AppContext& ctx)
{
    ctx.ui.editor.fullscreen = false;
    ctx.player.Close();
    EditorAudio().Clear();
    GoTo(ctx, Page::Clips);
}

void OpenImage(AppContext& ctx, const ClipInfo& clip, const std::vector<int>& sequence)
{
    ImageViewerState& v = ctx.ui.viewer;
    auto snap = LibrarySnapshot(ctx);
    v.sequence.clear();
    if (snap)
        for (int i : sequence)
            if (i >= 0 && i < (int)snap->size() && (*snap)[(size_t)i].type == ClipType::Image) v.sequence.push_back((*snap)[(size_t)i].id);
    if (std::find(v.sequence.begin(), v.sequence.end(), clip.id) == v.sequence.end()) v.sequence.push_back(clip.id);
    v.open = true;
    ctx.library.MarkOpened(clip.path);
    v.current = clip.id;
    v.clip = clip;
    v.zoom = 1.0f;
    v.actualSize = false;
    v.pan = ImVec2(0, 0);
}

void CloseImage(AppContext& ctx) { ctx.ui.viewer.open = false; }

// ---------------------------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------------------------

void ClipGrid(AppContext& ctx, const char* id, const std::vector<int>& items, bool compact)
{
    auto snap = LibrarySnapshot(ctx);
    if (!snap || items.empty()) return;
    ImGui::PushID(id);
    const float W = ImGui::GetContentRegionAvail().x;
    const float gap = S(compact ? 12.0f : 18.0f);
    const float minW = S(compact ? 210.0f : Tok::CardMinWidth);
    const int cols = Clamp((int)std::floor((W + gap) / (minW + gap)), 2, compact ? 6 : Tok::CardMaxColumns);
    const float cardW = std::floor((W - gap * (cols - 1)) / cols);
    const float thumbH = (cardW - S(12.0f)) * 9.0f / 16.0f;
    const float cardH = thumbH + S(12.0f) + (compact ? 0.0f : CardTextHeight());
    const int rows = ((int)items.size() + cols - 1) / cols;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ImGuiListClipper clipper;
    clipper.Begin(rows, cardH + gap);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int col = 0; col < cols; ++col) {
                const int k = row * cols + col;
                if (k >= (int)items.size()) break;
                const ClipInfo& c = (*snap)[(size_t)items[(size_t)k]];
                if (col > 0) ImGui::SameLine();
                const bool isImage = c.type == ClipType::Image;
                const ThumbInfo th = ctx.thumbs.Get(c.path, c.modified, (int)(cardW * 1.2f));
                const std::string game = GameName(c), when = RelativeTime(c.modified);
                char badge[48] = {};
                if (!isImage && c.duration > 0.0) snprintf(badge, sizeof(badge), "%s", FormatDuration(c.duration).c_str());
                else if (isImage && c.width > 0) snprintf(badge, sizeof(badge), "%d\xC3\x97%d", c.width, c.height);
                CardData d;
                d.title = c.title.c_str();
                d.game = game.c_str();
                d.when = when.c_str();
                d.badge = badge;
                d.bookmarks = c.bookmarks;
                d.isImage = isImage;
                d.compact = compact;
                d.thumb = (ImTextureID)th.texture;
                d.thumbW = th.width;
                d.thumbH = th.height;
                d.thumbLoading = !th.texture && !th.failed;
                // The hover preview needs the clip's filmstrip: only ask for it while hovered.
                const ImVec2 cp = ImGui::GetCursorScreenPos();
                if (!isImage && ImGui::IsMouseHoveringRect(cp, ImVec2(cp.x + cardW, cp.y + cardH)) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
                    const StripInfo si = ctx.strips.Get(c.path, c.modified);
                    d.strip = (ImTextureID)si.texture;
                    d.stripFrames = si.frames;
                }
                char cid[32];
                snprintf(cid, sizeof(cid), "c%llu", (unsigned long long)c.id);
                const CardResult r = ClipCard(cid, d, ImVec2(cardW, cardH));
                if (r.clicked || r.edit) {
                    if (isImage) OpenImage(ctx, c, items);
                    else OpenEditor(ctx, c);
                }
                if (r.rightClicked || r.more) {
                    ctx.ui.library.target = c;
                    ctx.ui.library.menuRequested = true;
                }
            }
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------
// Menu and dialogs
// ---------------------------------------------------------------------------------------------

static void CopyClip(AppContext& ctx, const ClipInfo& c)
{
    const bool ok = c.type == ClipType::Image ? platform::CopyImageToClipboard(ctx.window.Hwnd(), c.path)
                                              : platform::CopyFilesToClipboard(ctx.window.Hwnd(), { c.path });
    Snack(ctx.ui, ok ? (c.type == ClipType::Image ? "Image copied" : "Clip copied \xE2\x80\x94 paste it anywhere") : "Couldn't copy to the clipboard",
          L"", !ok);
}

// Tags: chips you can remove, a field to add more, and your other tags as one-click suggestions.
static void TagsModal(AppContext& ctx)
{
    LibraryState& lib = ctx.ui.library;
    if (lib.openTags) {
        ImGui::OpenPopup("##tags");
        lib.openTags = false;
    }
    if (!BeginModal("##tags", "Tags", S(440.0f))) return;
    const float W = ImGui::GetContentRegionAvail().x;
    TextWrapped(lib.target.title.c_str(), Pal().textDim, W, GetFonts().regular, Type::Caption);
    Gap(4.0f);
    auto chips = [&](const std::vector<std::string>& list, bool removable, std::vector<std::string>* clicked) {
        float x = 0.0f;
        const float x0 = ImGui::GetCursorPosX();
        for (size_t i = 0; i < list.size(); ++i) {
            PushFont(GetFonts().medium, Type::Caption);
            const float tw = ImGui::CalcTextSize(list[i].c_str()).x + S(removable ? 34.0f : 22.0f);
            PopFont();
            if (x > 0.0f && x + tw > W) {
                x = 0.0f;
                Gap(4.0f);
            } else if (x > 0.0f) {
                ImGui::SameLine(0, 0);
            }
            ImGui::SetCursorPosX(x0 + x);
            ButtonOpts o;
            o.style = removable ? Btn::Secondary : Btn::Ghost;
            o.icon = removable ? ICON_CLOSE : ICON_ADD;
            o.size = ImVec2(tw, S(28.0f));
            o.round = true;
            char id[80];
            snprintf(id, sizeof(id), "%s##chip%d%d", list[i].c_str(), removable, (int)i);
            if (Button(id, o) && clicked) clicked->push_back(list[i]);
            x += tw + S(6.0f);
        }
    };
    std::vector<std::string> removed, added;
    if (lib.tagEdit.empty()) Text("No tags yet.", Pal().textFaint, nullptr, Type::Caption);
    else chips(lib.tagEdit, true, &removed);
    Gap(6.0f);
    const bool enter = TextField("##newtag", lib.tagBuf, sizeof(lib.tagBuf), "Add a tag and press Enter", W,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter && lib.tagBuf[0]) {
        std::string t = lib.tagBuf;
        while (!t.empty() && (t.back() == ' ' || t.front() == ' ')) t = t.back() == ' ' ? t.substr(0, t.size() - 1) : t.substr(1);
        added.push_back(t);
        lib.tagBuf[0] = 0;
        ImGui::SetKeyboardFocusHere(-1);
    }
    // Suggestions: the tags you've used on other clips.
    std::vector<std::string> others;
    if (auto snap = LibrarySnapshot(ctx))
        for (const ClipInfo& c : *snap)
            for (const std::string& t : c.tags)
                if (std::find(lib.tagEdit.begin(), lib.tagEdit.end(), t) == lib.tagEdit.end() && std::find(others.begin(), others.end(), t) == others.end())
                    others.push_back(t);
    if (!others.empty()) {
        Gap(4.0f);
        Text("Your tags", Pal().textFaint, GetFonts().medium, Type::Caption);
        if (others.size() > 12) others.resize(12);
        chips(others, false, &added);
    }
    for (const std::string& r : removed) lib.tagEdit.erase(std::remove(lib.tagEdit.begin(), lib.tagEdit.end(), r), lib.tagEdit.end());
    for (const std::string& a : added)
        if (!a.empty() && std::find(lib.tagEdit.begin(), lib.tagEdit.end(), a) == lib.tagEdit.end() && lib.tagEdit.size() < 12) lib.tagEdit.push_back(a);
    Gap(8.0f);
    ButtonOpts done;
    done.style = Btn::Primary;
    done.size = ImVec2(W, S(40.0f));
    if (Button("Done", done)) {
        ctx.library.SetTags(lib.target.path, lib.tagEdit);
        ImGui::CloseCurrentPopup();
    }
    EndModal();
}

// Clean up: clips that are probably safe to remove, ticked, with how much space they free.
static void BuildCleanup(AppContext& ctx)
{
    LibraryState& lib = ctx.ui.library;
    lib.cleanup.clear();
    auto snap = LibrarySnapshot(ctx);
    if (!snap) return;
    const int64_t now = NowUnix(), month = 30 * 86400;
    std::vector<const ClipInfo*> videos;
    for (const ClipInfo& c : *snap)
        if (c.type == ClipType::Video && !c.favourite) videos.push_back(&c);
    // Near-duplicates: same game, saved within two minutes of each other with almost the same length
    // (pressing Save clip twice). Keep the first, suggest the later ones.
    std::vector<bool> dup(videos.size(), false);
    for (size_t i = 0; i < videos.size(); ++i)
        for (size_t j = 0; j < videos.size(); ++j) {
            if (i == j || dup[j]) continue;
            const ClipInfo &a = *videos[i], &b = *videos[j];
            if (a.game == b.game && b.modified > a.modified && b.modified - a.modified < 120 && std::fabs(a.duration - b.duration) < 0.75 &&
                a.duration > 0) {
                dup[j] = true;
                lib.cleanup.push_back({ b, "Saved twice: almost the same as \"" + a.title + "\"", true });
            }
        }
    // Old and never opened.
    for (size_t i = 0; i < videos.size(); ++i) {
        const ClipInfo& c = *videos[i];
        if (dup[i] || now - c.modified < month) continue;
        if (c.lastOpened != 0 && now - c.lastOpened < month) continue;
        char why[96];
        snprintf(why, sizeof(why), "%d days old, %s", (int)((now - c.modified) / 86400), c.lastOpened ? "not opened in a month" : "never opened");
        lib.cleanup.push_back({ c, why, c.sizeBytes > 200ull * 1024 * 1024 });
    }
    std::sort(lib.cleanup.begin(), lib.cleanup.end(), [](const auto& a, const auto& b) { return a.clip.sizeBytes > b.clip.sizeBytes; });
    if (lib.cleanup.size() > 200) lib.cleanup.resize(200);
}

static void CleanupModal(AppContext& ctx)
{
    LibraryState& lib = ctx.ui.library;
    if (lib.openCleanup) {
        BuildCleanup(ctx);
        ImGui::OpenPopup("##cleanup");
        lib.openCleanup = false;
    }
    if (!BeginModal("##cleanup", "Clean up", S(560.0f))) return;
    const float W = ImGui::GetContentRegionAvail().x;
    if (lib.cleanup.empty()) {
        TextWrapped("Nothing to suggest. No near-duplicates, and every clip older than a month has been opened recently or is a favourite.",
                    Pal().textDim, W, nullptr, Type::Body);
        Gap(8.0f);
        ButtonOpts ok;
        ok.size = ImVec2(W, S(40.0f));
        if (Button("Close", ok)) ImGui::CloseCurrentPopup();
        EndModal();
        return;
    }
    TextWrapped("These look safe to remove. Untick anything you want to keep; ticked clips go to the Recycle Bin, so you can still get them back. "
                "Favourites are never suggested.",
                Pal().textDim, W, nullptr, Type::Caption);
    Gap(6.0f);
    ImGui::BeginChild("##cleanlist", ImVec2(W, std::min(S(360.0f), (S(52.0f) + ImGui::GetStyle().ItemSpacing.y) * lib.cleanup.size())), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    uint64_t freed = 0;
    int count = 0;
    for (size_t i = 0; i < lib.cleanup.size(); ++i) {
        auto& it = lib.cleanup[i];
        ImGui::PushID((int)i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rw = ImGui::GetContentRegionAvail().x;
        Toggle("##keep", &it.selected);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        PushFont(GetFonts().medium, Type::Body);
        const std::string title = Ellipsize(it.clip.title.c_str(), rw - S(170.0f));
        dl->AddText(ImVec2(p.x + S(54.0f), p.y + S(4.0f)), Pal().text, title.c_str());
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        dl->AddText(ImVec2(p.x + S(54.0f), p.y + S(24.0f)), Pal().textFaint, it.reason.c_str());
        const std::string size = FormatBytes(it.clip.sizeBytes);
        dl->AddText(ImVec2(p.x + rw - ImGui::CalcTextSize(size.c_str()).x - S(6.0f), p.y + S(12.0f)), Pal().textDim, size.c_str());
        PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(52.0f)));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
        if (it.selected) {
            freed += it.clip.sizeBytes;
            ++count;
        }
    }
    ImGui::EndChild();
    Gap(8.0f);
    const float bw = (W - S(10.0f)) * 0.5f;
    ButtonOpts cancel;
    cancel.size = ImVec2(bw, S(40.0f));
    if (Button("Cancel##clean", cancel)) ImGui::CloseCurrentPopup();
    ImGui::SameLine(0, S(10.0f));
    ButtonOpts del;
    del.style = Btn::Danger;
    del.icon = ICON_DELETE;
    del.size = ImVec2(bw, S(40.0f));
    del.enabled = count > 0;
    char label[96];
    snprintf(label, sizeof(label), "Remove %d (frees %s)##go", count, FormatBytes(freed).c_str());
    if (Button(label, del)) {
        for (auto& it : lib.cleanup)
            if (it.selected) ctx.library.Delete(it.clip.path);
        Snack(ctx.ui, std::to_string(count) + (count == 1 ? " clip" : " clips") + " moved to the Recycle Bin");
        ImGui::CloseCurrentPopup();
    }
    EndModal();
}

// Montage tray: a glass pill at the bottom while clips are queued.
static void MontageTray(AppContext& ctx)
{
    MontageState& m = ctx.ui.montage;
    const Page page = ctx.ui.page;
    const float shown = Anim(ImGui::GetID("##montagetrayA"), !m.items.empty() && (page == Page::Clips) ? 1.0f : 0.0f, Motion::Page);
    if (shown < 0.01f) return;
    double total = 0.0;
    for (const ClipInfo& c : m.items) total += c.duration;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = S(520.0f), h = S(58.0f);
    const ImVec2 pos(vp->Pos.x + vp->Size.x * 0.5f + S(130.0f) - w * 0.5f, vp->Pos.y + vp->Size.y - h - S(28.0f) + S(30.0f) * (1.0f - EaseOutCubic(shown)));
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, shown);
    ImGui::Begin("##montagetray", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = pos, b(pos.x + w, pos.y + h);
    dl->PushClipRectFullScreen();
    Shadow(dl, a, b, h * 0.5f, 1.8f);
    dl->PopClipRect();
    Glass(dl, a, b, h * 0.5f, 0.0f, Tok::GlassBorder, 0, 3);
    Icon(dl, ICON_LIST, ImVec2(a.x + S(30.0f), a.y + h * 0.5f), S(18.0f), Accent2());
    char line[96];
    snprintf(line, sizeof(line), "Montage: %d %s, %s", (int)m.items.size(), m.items.size() == 1 ? "clip" : "clips", FormatDuration(total).c_str());
    PushFont(GetFonts().semibold, Type::Body);
    dl->AddText(ImVec2(a.x + S(52.0f), a.y + (h - ImGui::GetFontSize()) * 0.5f), Pal().text, line);
    PopFont();
    ImGui::SetCursorScreenPos(ImVec2(b.x - S(250.0f), a.y + (h - S(38.0f)) * 0.5f));
    ButtonOpts clear;
    clear.style = Btn::Ghost;
    clear.size = ImVec2(S(80.0f), S(38.0f));
    if (Button("Clear##montage", clear)) m.items.clear();
    ImGui::SameLine(0, S(8.0f));
    ButtonOpts make;
    make.style = Btn::Primary;
    make.icon = ICON_DOWNLOAD;
    make.size = ImVec2(S(150.0f), S(38.0f));
    make.enabled = m.items.size() >= 1;
    if (Button("Make video", make)) {
        if (!m.name[0]) {
            time_t now = (time_t)NowUnix();
            tm lt{};
#ifdef _WIN32
            localtime_s(&lt, &now);
#else
            localtime_r(&now, &lt);
#endif
            char d[32];
            strftime(d, sizeof(d), "%d %b %H.%M", &lt);
            snprintf(m.name, sizeof(m.name), "Montage %s", d);
        }
        m.openExport = true;
    }
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void MontageModal(AppContext& ctx)
{
    MontageState& m = ctx.ui.montage;
    if (m.openExport) {
        ImGui::OpenPopup("##montage");
        m.openExport = false;
        m.exportError.clear();
        m.exportFinished = false;
    }
    if (!BeginModal("##montage", "Make a montage", S(520.0f))) return;
    if (m.exportFinished) {
        m.exportFinished = false;
        m.items.clear();
        ImGui::CloseCurrentPopup();
    }
    const float W = ImGui::GetContentRegionAvail().x;
    const bool busy = m.exporting;
    ImGui::BeginDisabled(busy);
    Text("Order (top plays first)", Pal().textDim, GetFonts().medium, Type::Caption);
    ImGui::BeginChild("##morder", ImVec2(W, std::min(S(300.0f), (S(46.0f) + ImGui::GetStyle().ItemSpacing.y) * m.items.size())), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    int moveFrom = -1, moveTo = -1, removeAt = -1;
    for (size_t i = 0; i < m.items.size(); ++i) {
        const ClipInfo& c = m.items[i];
        ImGui::PushID((int)i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float rw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        Glass(dl, p, ImVec2(p.x + rw, p.y + S(40.0f)), S(10.0f), 0.0f, Tok::GlassBorderSoft, 0, 2);
        char num[8];
        snprintf(num, sizeof(num), "%d", (int)i + 1);
        PushFont(GetFonts().semibold, Type::Body);
        dl->AddText(ImVec2(p.x + S(12.0f), p.y + (S(40.0f) - ImGui::GetFontSize()) * 0.5f), Accent2(), num);
        const std::string t = Ellipsize(c.title.c_str(), rw - S(200.0f));
        dl->AddText(ImVec2(p.x + S(36.0f), p.y + (S(40.0f) - ImGui::GetFontSize()) * 0.5f), Pal().text, t.c_str());
        PopFont();
        PushFont(GetFonts().regular, Type::Caption);
        const std::string d = FormatDuration(c.duration);
        dl->AddText(ImVec2(p.x + rw - S(126.0f) - ImGui::CalcTextSize(d.c_str()).x, p.y + (S(40.0f) - ImGui::GetFontSize()) * 0.5f), Pal().textFaint, d.c_str());
        PopFont();
        const float bs = S(30.0f);
        auto small = [&](const char* id, const char* icon, const char* tip, bool en) {
            ButtonOpts o;
            o.style = Btn::Ghost;
            o.icon = icon;
            o.size = ImVec2(bs, bs);
            o.round = true;
            o.tooltip = tip;
            o.enabled = en;
            return Button(id, o);
        };
        ImGui::SetCursorScreenPos(ImVec2(p.x + rw - bs * 3 - S(12.0f), p.y + S(5.0f)));
        if (small("##up", ICON_CHEVRON_UP, "Move up", i > 0)) moveFrom = (int)i, moveTo = (int)i - 1;
        ImGui::SameLine(0, S(2.0f));
        if (small("##down", ICON_CHEVRON_DOWN, "Move down", i + 1 < m.items.size())) moveFrom = (int)i, moveTo = (int)i + 1;
        ImGui::SameLine(0, S(2.0f));
        if (small("##rm", ICON_CLOSE, "Remove", true)) removeAt = (int)i;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(46.0f)));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (moveFrom >= 0) std::swap(m.items[(size_t)moveFrom], m.items[(size_t)moveTo]);
    if (removeAt >= 0) m.items.erase(m.items.begin() + removeAt);
    Gap(4.0f);
    Text("Quality", Pal().textDim, GetFonts().medium, Type::Caption);
    static const char* kQualities[] = { "Original", "High", "Small file" };
    Segmented("##mq", &m.quality, kQualities, 3, W);
    Text("Fit under a size", Pal().textDim, GetFonts().medium, Type::Caption);
    static const char* kSizes[] = { "No limit", "10 MB (Discord)", "25 MB", "50 MB", "100 MB" };
    Dropdown("##msize", &m.sizeLimit, kSizes, 5, W);
    Gap(4.0f);
    const float rowY = ImGui::GetCursorPosY();
    Text("Show Replayr banner", Pal().text, GetFonts().medium, Type::Body);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + W - S(44.0f), rowY - S(3.0f)));
    Toggle("##mbanner", &m.banner);
    Gap(8.0f);
    const bool enter = TextField("##mname", m.name, sizeof(m.name), "File name", W, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled();
    if (busy) {
        Gap(4.0f);
        char pr[48];
        snprintf(pr, sizeof(pr), "Making the montage\xE2\x80\xA6 %d%%", (int)std::lround(m.exportProgress * 100.0f));
        Text(pr, Pal().text, GetFonts().medium, Type::Body);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ProgressBar(ImGui::GetWindowDrawList(), p, ImVec2(p.x + W, p.y + S(6.0f)), m.exportProgress);
        ImGui::Dummy(ImVec2(W, S(6.0f)));
    }
    if (!m.exportError.empty()) TextWrapped(m.exportError.c_str(), Pal().danger, W, GetFonts().medium, Type::Caption);
    Gap(8.0f);
    const float bw = (W - S(10.0f)) * 0.5f;
    ButtonOpts cancel;
    cancel.size = ImVec2(bw, S(40.0f));
    if (Button(busy ? "Stop##m" : "Cancel##m", cancel)) {
        if (busy) ctx.engine.CancelExport(m.exportJob);
        else ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, S(10.0f));
    ButtonOpts save;
    save.style = Btn::Primary;
    save.icon = ICON_DOWNLOAD;
    save.size = ImVec2(bw, S(40.0f));
    save.enabled = !busy && !m.items.empty() && m.name[0];
    if (Button("Save##m", save) || (enter && save.enabled)) {
        static const double sizes[] = { 0.0, 10.0, 25.0, 50.0, 100.0 };
        engine::ExportRequest req;
        for (const ClipInfo& c : m.items) req.segments.push_back(engine::Segment{ c.path, 0.0, c.duration > 0 ? c.duration : 3600.0, 1.0 });
        req.source = m.items[0].path;
        req.quality = m.quality;
        req.targetMB = sizes[Clamp(m.sizeLimit, 0, 4)];
        req.banner = m.banner;
        req.bannerStyle = BannerStyleFor(ctx.settings);
        req.hardware = ctx.settings.video.encoder != "x264";
        req.destination = UniquePath(JoinPath(ctx.settings.ClipsFolder(), SanitizeFileName(ToWide(m.name)) + L".mp4"));
        m.exportError.clear();
        m.exportProgress = 0.0f;
        m.exporting = true;
        m.exportJob = ctx.engine.Export(req);
    }
    EndModal();
}

void LibraryOverlays(AppContext& ctx)
{
    LibraryState& lib = ctx.ui.library;
    if (lib.menuRequested) {
        lib.menuRequested = false;
        ImGui::OpenPopup("##clipmenu");
    }
    if (BeginMenu("##clipmenu")) {
        const ClipInfo& c = lib.target;
        const bool video = c.type == ClipType::Video;
        const ClipInfo* live = FindClip(ctx, c.id);
        const bool fav = live ? live->favourite : c.favourite;
        MenuLabel(c.title.c_str());
        if (MenuItem(video ? ICON_TRIM : ICON_ZOOM_IN, video ? "Edit" : "View")) {
            if (video) OpenEditor(ctx, c);
            else OpenImage(ctx, c, {});
        }
        if (MenuItem(ICON_RENAME, "Rename\xE2\x80\xA6", "F2")) {
            snprintf(lib.renameBuf, sizeof(lib.renameBuf), "%s", c.title.c_str());
            lib.openRename = true;
        }
        if (MenuItem(fav ? ICON_STAR_FILL : ICON_STAR, fav ? "Remove from favourites" : "Add to favourites")) ctx.library.SetFavourite(c.path, !fav);
        if (MenuItem(ICON_TAG, "Tags\xE2\x80\xA6")) {
            lib.tagEdit = live ? live->tags : c.tags;
            lib.tagBuf[0] = 0;
            lib.openTags = true;
        }
        if (video && ctx.account && !(ctx.account->State() == AccountState::SignedIn && !ctx.account->User().cloudClips)) {
            if (MenuItem(ICON_UPLOAD, "Upload & copy link")) UploadClip(ctx, live ? *live : c);
        }
        if (video) {
            MontageState& m = ctx.ui.montage;
            const bool inMontage = std::any_of(m.items.begin(), m.items.end(), [&](const ClipInfo& x) { return x.path == c.path; });
            if (MenuItem(ICON_LIST, inMontage ? "Remove from montage" : "Add to montage")) {
                if (inMontage) m.items.erase(std::remove_if(m.items.begin(), m.items.end(), [&](const ClipInfo& x) { return x.path == c.path; }), m.items.end());
                else m.items.push_back(live ? *live : c);
            }
        }
        MenuSeparator();
        if (MenuItem(ICON_COPY, video ? "Copy file" : "Copy image")) CopyClip(ctx, c);
        if (MenuItem(ICON_FOLDER_OPEN, "Show in folder")) platform::RevealInExplorer(c.path);
        if (MenuItem(ICON_OPEN_EXTERNAL, "Open externally")) platform::OpenPath(c.path);
        MenuSeparator();
        if (MenuItem(ICON_DELETE, "Delete\xE2\x80\xA6", "Del", true)) lib.openDelete = true;
        EndMenu();
    }

    if (lib.openRename) {
        ImGui::OpenPopup("##rename");
        lib.openRename = false;
    }
    if (BeginModal("##rename", "Rename", S(420.0f))) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = TextField("##renamefield", lib.renameBuf, sizeof(lib.renameBuf), "Name", -1.0f,
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool valid = lib.renameBuf[0] != 0;
        Gap(6.0f);
        const float bw = (ImGui::GetContentRegionAvail().x - S(8.0f)) * 0.5f;
        ButtonOpts cancel;
        cancel.size = ImVec2(bw, 0);
        if (Button("Cancel", cancel) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ButtonOpts ok;
        ok.style = Btn::Primary;
        ok.size = ImVec2(bw, 0);
        ok.enabled = valid;
        if (Button("Rename##ok", ok) || (enter && valid)) {
            EditorState& e = ctx.ui.editor;
            if (e.hasClip && e.clip.path == lib.target.path) {
                ctx.player.Close();   // the player holds the file open
                e.holdPlayer = true;
            }
            ctx.library.Rename(lib.target.path, ToWide(lib.renameBuf));
            ImGui::CloseCurrentPopup();
        }
        EndModal();
    }

    if (lib.openDelete) {
        ImGui::OpenPopup("##delete");
        lib.openDelete = false;
    }
    if (BeginModal("##delete", "Delete this?", S(420.0f))) {
        char msg[400];
        snprintf(msg, sizeof(msg), "\"%s\" goes to the Recycle Bin. You can restore it from there.", lib.target.title.c_str());
        TextWrapped(msg, Pal().textDim, ImGui::GetContentRegionAvail().x);
        Gap(6.0f);
        const float bw = (ImGui::GetContentRegionAvail().x - S(8.0f)) * 0.5f;
        ButtonOpts cancel;
        cancel.size = ImVec2(bw, 0);
        if (Button("Cancel", cancel) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ButtonOpts del;
        del.style = Btn::Danger;
        del.icon = ICON_DELETE;
        del.size = ImVec2(bw, 0);
        if (Button("Delete##ok", del) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            EditorState& e = ctx.ui.editor;
            if (e.hasClip && e.clip.id == lib.target.id) {
                ctx.player.Close();
                const bool inEditor = ctx.ui.page == Page::Editor;
                e = EditorState{};
                if (inEditor) GoTo(ctx, Page::Clips);
            }
            if (ctx.ui.viewer.open && ctx.ui.viewer.current == lib.target.id) CloseImage(ctx);
            ctx.library.Delete(lib.target.path);
            ImGui::CloseCurrentPopup();
        }
        EndModal();
    }

    TagsModal(ctx);
    CleanupModal(ctx);
    MontageTray(ctx);
    MontageModal(ctx);
}

// ---------------------------------------------------------------------------------------------
// Empty state
// ---------------------------------------------------------------------------------------------

void EmptyState(AppContext& ctx, const char* title, const char* body, bool showActions)
{
    const float W = ImGui::GetContentRegionAvail().x;
    Gap(40.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + W * 0.5f, p.y + S(56.0f));
    const float t = (float)ImGui::GetTime();
    RadialGlow(dl, c, S(110.0f), Accent2(0.28f));
    dl->AddCircleFilled(c, S(52.0f), White(0.07f), 64);
    dl->AddCircle(c, S(52.0f), White(0.22f), 64, 1.0f);
    dl->AddCircle(c, S(64.0f + 2.0f * std::sin(t * 1.5f)), White(0.08f), 64, 1.0f);
    GradientRectH(dl, ImVec2(c.x - S(22.0f), c.y - S(16.0f)), ImVec2(c.x + S(22.0f), c.y + S(16.0f)), Accent(), Accent2(), S(8.0f));
    Icon(dl, ICON_PLAY, ImVec2(c.x + S(2.0f), c.y), S(16.0f), White(1.0f));
    for (int i = 0; i < 3; ++i) {
        const float ang = t * 0.6f + i * 2.094f;
        dl->AddCircleFilled(ImVec2(c.x + std::cos(ang) * S(78.0f), c.y + std::sin(ang) * S(78.0f)), S(2.5f), Accent2(0.8f));
    }
    Gap(128.0f);
    PushFont(GetFonts().semibold, Type::Heading + 2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (W - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    PopFont();
    Gap(2.0f);
    PushFont(GetFonts().regular, Type::Body);
    const float bw = std::min(W, S(440.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (W - std::min(bw, ImGui::CalcTextSize(body).x)) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, Pal().textDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bw);
    ImGui::TextUnformatted(body);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    PopFont();
    if (!showActions) return;
    Gap(14.0f);
    const float bwid = S(180.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (W - bwid) * 0.5f);
    ButtonOpts o;
    o.style = Btn::Primary;
    o.size = ImVec2(bwid, 0);
    if (!ctx.settings.replay.enabled) {
        o.icon = ICON_HISTORY;
        if (Button("Turn on replay", o)) ctx.Do(Action::ToggleBuffer);
    } else {
        o.icon = ICON_CAMERA;
        if (Button("Take a screenshot", o)) ctx.Do(Action::ScreenshotFull);
    }
}

} // namespace cs::ui
