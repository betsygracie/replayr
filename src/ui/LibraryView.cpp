// The library: one grid grouped by Today / Yesterday / Earlier, a game filter and a sort control.
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/Util.h"

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
