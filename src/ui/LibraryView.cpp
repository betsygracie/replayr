// The library: one grid grouped by Today / Yesterday / Earlier, a game filter and a sort control.
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Util.h"
#include "core/Bookmarks.h"
#include "core/Platform.h"
#include "core/Thumbnailer.h"
#include "media/MediaCache.h"

#include <algorithm>
#include <climits>
#include <ctime>
#include <map>

namespace cs::ui {

namespace {

int64_t StartOfToday()
{
    time_t now = (time_t)NowUnix();
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    return (int64_t)mktime(&lt);
}

// "All games ▾": all games, favourites, then each game with its avatar.
void GameFilter(AppContext& ctx, bool screenshots, float width)
{
    LibraryState& lib = ctx.ui.library;
    auto snap = LibrarySnapshot(ctx);
    std::map<std::string, int> games;
    if (snap)
        for (const ClipInfo& c : *snap)
            if ((c.type == ClipType::Image) == screenshots) games[GameName(c)]++;
    const std::string label = lib.favourites ? "Favourites" : (lib.game.empty() ? "All games" : lib.game);
    if (BeginDropdown("##game", label.c_str(), width, lib.favourites ? ICON_STAR_FILL : ICON_GAME)) {
        if (DropdownItem("All games", lib.game.empty() && !lib.favourites, ICON_GAME)) {
            lib.game.clear();
            lib.favourites = false;
        }
        if (DropdownItem("Favourites", lib.favourites, ICON_STAR)) {
            lib.game.clear();
            lib.favourites = true;
        }
        for (const auto& kv : games) {
            char item[160];
            snprintf(item, sizeof(item), "%s  (%d)##%s", kv.first.c_str(), kv.second, kv.first.c_str());
            if (DropdownItem(item, !lib.favourites && lib.game == kv.first, ICON_DOT)) {
                lib.game = kv.first;
                lib.favourites = false;
            }
        }
        EndDropdown();
    }
}

// "Filters ▾": when it was recorded, how long it is, and tags.
void Filters(AppContext& ctx, bool screenshots, float width)
{
    LibraryState& lib = ctx.ui.library;
    auto snap = LibrarySnapshot(ctx);
    std::map<std::string, int> tags;
    if (snap)
        for (const ClipInfo& c : *snap)
            if ((c.type == ClipType::Image) == screenshots)
                for (const std::string& t : c.tags) tags[t]++;
    const int active = (lib.dateFilter != 0) + (!screenshots && lib.lengthFilter != 0) + !lib.tag.empty();
    char label[32];
    if (active) snprintf(label, sizeof(label), "Filters (%d)", active);
    else snprintf(label, sizeof(label), "Filters");
    if (BeginDropdown("##filters", label, width, ICON_FILTER)) {
        MenuLabel("Recorded");
        static const char* dates[] = { "Any time", "Today", "This week", "This month" };
        for (int i = 0; i < 4; ++i) {
            char id[48];
            snprintf(id, sizeof(id), "%s##d%d", dates[i], i);
            if (DropdownItem(id, lib.dateFilter == i)) lib.dateFilter = i;
        }
        if (!screenshots) {
            MenuSeparator();
            MenuLabel("Length");
            static const char* lens[] = { "Any length", "Under 30 seconds", "30 seconds to 2 minutes", "Over 2 minutes" };
            for (int i = 0; i < 4; ++i) {
                char id[48];
                snprintf(id, sizeof(id), "%s##l%d", lens[i], i);
                if (DropdownItem(id, lib.lengthFilter == i)) lib.lengthFilter = i;
            }
        }
        MenuSeparator();
        MenuLabel("Tag");
        if (DropdownItem("Any tag##t", lib.tag.empty(), ICON_TAG)) lib.tag.clear();
        for (const auto& kv : tags) {
            char item[96];
            snprintf(item, sizeof(item), "%s  (%d)##tag%s", kv.first.c_str(), kv.second, kv.first.c_str());
            if (DropdownItem(item, lib.tag == kv.first, ICON_TAG)) lib.tag = kv.first;
        }
        if (tags.empty()) MenuLabel("No tags yet: right-click a clip > Tags");
        if (active) {
            MenuSeparator();
            if (DropdownItem("Clear filters", false, ICON_CLOSE)) {
                lib.dateFilter = lib.lengthFilter = 0;
                lib.tag.clear();
            }
        }
        EndDropdown();
    }
}

} // namespace

// ---- Suggested highlights -----------------------------------------------------------------
// Short clips the highlight detector saved into the Suggestions folder. Save moves one into the
// library; Dismiss deletes it. Old ones (over a day) and extras (past 12) are cleared out.
namespace {
struct Suggestion {
    std::wstring path;
    int64_t modified = 0;
    int score = 0;
    std::string game, sources;
};
std::vector<Suggestion> g_suggestions;
double g_suggestScan = -100.0;

void ScanSuggestions(AppContext& ctx)
{
    const double now = NowSeconds();
    if (!ctx.ui.suggestionsChanged && now - g_suggestScan < 5.0) return;
    ctx.ui.suggestionsChanged = false;
    g_suggestScan = now;
    std::vector<Suggestion> list;
    for (const std::wstring& f : ListFiles(SuggestionsDir(), L"*.mp4")) {
        Suggestion sg;
        sg.path = f;
        sg.modified = FileModifiedUnix(f);
        for (const Bookmark& b : LoadBookmarks(f)) {
            if (b.source == "auto") sg.score = std::max(sg.score, b.score);
            if (sg.game.empty()) sg.game = b.game;
        }
        list.push_back(sg);
    }
    std::sort(list.begin(), list.end(), [](const Suggestion& a, const Suggestion& b) { return a.modified > b.modified; });
    const int64_t cutoff = (int64_t)std::time(nullptr) - 24 * 3600;
    for (size_t i = 0; i < list.size();) {
        if (i >= 12 || list[i].modified < cutoff) {
            platform::DeleteFileNow(list[i].path);
            SaveBookmarks(list[i].path, {});
            list.erase(list.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
    g_suggestions = std::move(list);
}

void SuggestedRow(AppContext& ctx, float W)
{
    ScanSuggestions(ctx);
    if (g_suggestions.empty()) return;
    Settings& st = ctx.settings;
    // Header: title + count, and the auto-save switch on the right.
    const ImVec2 hp = ImGui::GetCursorPos();
    PushFont(GetFonts().semibold, Type::Heading);
    ImGui::TextUnformatted("Suggested highlights");
    PopFont();
    ImGui::SameLine(0, S(10.0f));
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d new", (int)g_suggestions.size());
    Badge(ImGui::GetWindowDrawList(), ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + S(4.0f)), cnt, Accent(0.9f), White(1.0f));
    ImGui::NewLine();
    {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "Auto-save %d+", st.highlights.minScore);
        const float tw = ImGui::CalcTextSize(lbl).x;
        ImGui::SetCursorPos(ImVec2(hp.x + W - S(44.0f) - tw - S(10.0f), hp.y + S(4.0f)));
        Text(lbl, Pal().textDim, GetFonts().medium, Type::Caption);
        ImGui::SetCursorPos(ImVec2(hp.x + W - S(44.0f), hp.y));
        if (Toggle("##autosavehl", &st.highlights.autoSave)) ctx.Changed();
        Tooltip("Save highlights at or above this score straight into your clips (Settings > Highlights)");
        ImGui::SetCursorPos(ImVec2(hp.x, hp.y + S(40.0f)));
    }
    const float gap = S(14.0f);
    const int cols = std::max(2, std::min(5, (int)((W + gap) / (S(250.0f) + gap))));
    const float cardW = std::floor((W - gap * (cols - 1)) / cols);
    const float cardH = (cardW - S(12.0f)) * 9.0f / 16.0f + S(12.0f) + CardTextHeight();
    int drop = -1, keep = -1;
    for (int i = 0; i < (int)g_suggestions.size() && i < cols; ++i) {
        const Suggestion& sg = g_suggestions[(size_t)i];
        if (i > 0) ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        const ThumbInfo th = ctx.thumbs.Get(sg.path, sg.modified, (int)(cardW * 1.2f));
        CardData d;
        const std::string title = (sg.game.empty() ? std::string("Highlight") : sg.game) + "", when = RelativeTime(sg.modified);
        char badge[32];
        snprintf(badge, sizeof(badge), "%s %d", ICON_STAR, sg.score);
        d.title = title.c_str();
        d.game = "Suggested highlight";
        d.when = when.c_str();
        d.badge = badge;
        d.thumb = (ImTextureID)th.texture;
        d.thumbW = th.width;
        d.thumbH = th.height;
        d.thumbLoading = !th.texture && !th.failed;
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        if (ImGui::IsMouseHoveringRect(cp, ImVec2(cp.x + cardW, cp.y + cardH))) {   // hover plays the moment
            const StripInfo si = ctx.strips.Get(sg.path, sg.modified);
            d.strip = (ImTextureID)si.texture;
            d.stripFrames = si.frames;
        }
        char cid[32];
        snprintf(cid, sizeof(cid), "sg%d", i);
        const CardResult r = ClipCard(cid, d, ImVec2(cardW, cardH));
        if (r.clicked || r.edit) {
            ClipInfo ci;
            ci.path = sg.path;
            ci.title = ToUtf8(FileStem(sg.path));
            ci.game = sg.game;
            ci.modified = sg.modified;
            OpenEditor(ctx, ci);
        }
        const float bw = (cardW - S(8.0f)) * 0.5f;
        ButtonOpts sv;
        sv.style = Btn::Primary;
        sv.icon = ICON_SAVE;
        sv.size = ImVec2(bw, S(32.0f));
        sv.tooltip = "Keep it: moves the clip into your library";
        ImGui::PushID(i);
        if (Button("Save clip", sv)) keep = i;
        ImGui::SameLine(0, S(8.0f));
        ButtonOpts ds = sv;
        ds.style = Btn::Ghost;
        ds.icon = ICON_CLOSE;
        ds.tooltip = "Not interested: deletes it";
        if (Button("Dismiss", ds)) drop = i;
        ImGui::PopID();
        ImGui::EndGroup();
    }
    if (keep >= 0) {
        const Suggestion sg = g_suggestions[(size_t)keep];
        std::wstring dest = st.ClipsFolder();
        if (st.storage.perGameFolders && !sg.game.empty()) dest = JoinPath(dest, SanitizeFileName(ToWide(sg.game)));
        ctx.library.Move(sg.path, dest);
        g_suggestions.erase(g_suggestions.begin() + keep);
        Snack(ctx.ui, "Saved to your clips");
    }
    if (drop >= 0) {
        platform::DeleteFileNow(g_suggestions[(size_t)drop].path);
        SaveBookmarks(g_suggestions[(size_t)drop].path, {});
        g_suggestions.erase(g_suggestions.begin() + drop);
    }
    Gap(22.0f);
}
} // namespace

void DrawLibrary(AppContext& ctx, bool screenshots)
{
    LibraryState& lib = ctx.ui.library;
    auto snap = LibrarySnapshot(ctx);
    int& sort = screenshots ? lib.shotSort : lib.clipSort;

    // Filtered, sorted items.
    std::vector<int> items;
    const std::string q = lib.search;
    if (snap)
        for (int i = 0; i < (int)snap->size(); ++i) {
            const ClipInfo& c = (*snap)[(size_t)i];
            if ((c.type == ClipType::Image) != screenshots) continue;
            if (lib.favourites && !c.favourite) continue;
            if (!lib.game.empty() && GameName(c) != lib.game) continue;
            if (!q.empty() && !ContainsNoCase(c.title, q) && !ContainsNoCase(GameName(c), q) &&
                std::none_of(c.tags.begin(), c.tags.end(), [&](const std::string& t) { return ContainsNoCase(t, q); }))
                continue;
            if (!lib.tag.empty() && std::find(c.tags.begin(), c.tags.end(), lib.tag) == c.tags.end()) continue;
            if (lib.dateFilter) {
                static const int64_t spans[] = { 0, 0, 7 * 86400, 30 * 86400 };
                const int64_t from = lib.dateFilter == 1 ? StartOfToday() : NowUnix() - spans[lib.dateFilter];
                if (c.modified < from) continue;
            }
            if (!screenshots && lib.lengthFilter) {
                const double d = c.duration;
                if (lib.lengthFilter == 1 && !(d < 30.0)) continue;
                if (lib.lengthFilter == 2 && !(d >= 30.0 && d <= 120.0)) continue;
                if (lib.lengthFilter == 3 && !(d > 120.0)) continue;
            }
            items.push_back(i);
        }
    if (snap)
        std::stable_sort(items.begin(), items.end(), [&](int a, int b) {
            const ClipInfo &x = (*snap)[(size_t)a], &y = (*snap)[(size_t)b];
            if (sort == 1) return x.modified < y.modified;
            if (sort == 2) return x.duration > y.duration;
            return x.modified > y.modified;
        });

    // Header: title on the left, filter + sort on the right.
    const float x0 = ImGui::GetCursorPosX(), y0 = ImGui::GetCursorPosY();
    const float W = ImGui::GetContentRegionAvail().x;
    PageHeader(screenshots ? "Screenshots" : "Clips");
    const float fw = S(190.0f), sw = S(130.0f), flw = S(140.0f), cw = S(Tok::ControlHeight);
    float hx = x0 + W - sw - fw - flw - cw - S(30.0f);
    ImGui::SetCursorPos(ImVec2(hx, y0));
    ButtonOpts clean;
    clean.style = Btn::Ghost;
    clean.icon = ICON_DELETE;
    clean.size = ImVec2(cw, cw);
    clean.round = true;
    clean.tooltip = "Clean up: find old, unwatched and duplicate clips to free space";
    if (Button("##cleanup", clean)) lib.openCleanup = true;
    hx += cw + S(10.0f);
    ImGui::SetCursorPos(ImVec2(hx, y0));
    Filters(ctx, screenshots, flw);
    hx += flw + S(10.0f);
    ImGui::SetCursorPos(ImVec2(hx, y0));
    GameFilter(ctx, screenshots, fw);
    ImGui::SetCursorPos(ImVec2(x0 + W - sw, y0));
    static const char* clipSorts[] = { "Newest", "Oldest", "Longest" };
    if (Dropdown("##sort", &sort, clipSorts, screenshots ? 2 : 3, sw)) ctx.Changed();
    ImGui::SetCursorPos(ImVec2(x0, y0 + S(Tok::ControlHeight) + S(18.0f)));
    if (!screenshots && q.empty()) SuggestedRow(ctx, W);

    if (items.empty()) {
        if (!q.empty()) EmptyState(ctx, "Nothing found", "Try a different word, or clear the search.", false);
        else if (lib.dateFilter || lib.lengthFilter || !lib.tag.empty())
            EmptyState(ctx, "Nothing matches these filters", "Open Filters and choose \"Clear filters\".", false);
        else if (lib.favourites) EmptyState(ctx, "No favourites yet", "Right-click a clip and choose \"Add to favourites\".", false);
        else if (!lib.game.empty()) EmptyState(ctx, "Nothing for this game", "Pick \"All games\" to see everything.", false);
        else if (screenshots) EmptyState(ctx, "No screenshots yet", "Press your screenshot hotkey in any game.", true);
        else {
            const std::string key = HotkeyText(ctx, HotkeyAction::SaveReplay);
            char body[200];
            snprintf(body, sizeof(body), "Press %s in-game to save the last %s.", key.empty() ? "the Save clip hotkey" : key.c_str(),
                     FormatDuration(ctx.settings.replay.seconds).c_str());
            EmptyState(ctx, "No clips yet", body, true);
        }
        return;
    }

    if (sort == 2) {   // by length: one grid
        ClipGrid(ctx, "all", items, screenshots);
        Gap(24.0f);
        return;
    }
    const int64_t today = StartOfToday();
    struct Section {
        const char* name;
        int64_t from, to;
    };
    Section sections[] = { { "Today", today, INT64_MAX }, { "Yesterday", today - 86400, today }, { "Earlier", INT64_MIN, today - 86400 } };
    if (sort == 1) std::swap(sections[0], sections[2]);
    for (const Section& s : sections) {
        std::vector<int> group;
        for (int i : items) {
            const int64_t m = (*snap)[(size_t)i].modified;
            if (m >= s.from && m < s.to) group.push_back(i);
        }
        if (group.empty()) continue;
        SectionHeader(s.name);
        Gap(6.0f);
        ClipGrid(ctx, s.name, group, screenshots);
        Gap(20.0f);
    }
}

} // namespace cs::ui
