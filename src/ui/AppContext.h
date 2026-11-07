#pragma once
#include "core/Bookmarks.h"
#include "core/ClipLibrary.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/SystemStats.h"
#include "core/Updater.h"
#include "account/Account.h"
#include "core/InputOverlay.h"
#include "engine/AudioCapture.h"
#include "engine/ClipEngine.h"
#include "imgui.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cs {
class Window;
class Hotkeys;
class Thumbnailer;
class MediaPlayer;
class ClipAudio;
class Filmstrips;
class ImageCache;
} // namespace cs

namespace cs::ui {

// Sidebar pages. (Saved in settings as the last page.)
enum class Page { Clips = 0, Screenshots, Editor, Uploads, Settings, Count };

// Things views ask the app to do after the frame (never in the middle of ImGui rendering).
enum class Action {
    SaveReplay,
    ToggleRecording,
    ToggleBuffer,
    ScreenshotFull,
    ScreenshotWindow,
    ScreenshotRegion,
    OpenClipsFolder,
    BrowseClipsFolder,
    OpenLogFolder,
    TestToast,
    StartKeyCapture,      // ui.settings.capturing says which slot
    CheckUpdates,
    ApplyUpdate,
    FinishSetup,
    Minimize,
    ToggleMaximize,
    Close,
    Quit,
};

struct LibraryState {
    char search[128] = {};      // the top bar search box
    std::string game;           // "" = all games
    bool favourites = false;    // the "Favourites" entry of the game filter
    int clipSort = 0;           // 0 newest, 1 oldest, 2 longest
    int shotSort = 0;           // 0 newest, 1 oldest
    std::shared_ptr<const ClipList> snapshot;
    uint64_t version = ~0ull;
    // Context menu and dialogs
    int dateFilter = 0;         // 0 any time, 1 today, 2 this week, 3 this month
    int lengthFilter = 0;       // 0 any length, 1 under 30 s, 2 30 s - 2 min, 3 over 2 min
    std::string tag;            // "" = any tag
    ClipInfo target;
    bool menuRequested = false, openRename = false, openDelete = false, openTags = false, openCleanup = false;
    char renameBuf[256] = {};
    char tagBuf[48] = {};
    std::vector<std::string> tagEdit;
    struct CleanupItem {
        ClipInfo clip;
        std::string reason;
        bool selected = true;
    };
    std::vector<CleanupItem> cleanup;
};

// The editor: one clip, trim, rename, export.
// A kept part of the clip (source time). Everything between sections is cut out.
struct EditSection {
    double start = 0.0, end = 0.0;
    float speed = 1.0f;
};
// Text on the video, in source time (moves with the footage when sections change).
struct EditCaption {
    char text[160] = {};
    double start = 0.0, end = 0.0;
    int size = 1;                    // 0 small, 1 medium, 2 large
    int position = 2;                // 0 top, 1 middle, 2 bottom
};

// One audio lane in the editor (Game, Mic, ...): live in the preview, applied on export.
struct EditLane {
    float gain = 1.0f;               // 0 .. 2
    bool mute = false, solo = false;
    bool defaultMute = false;        // "Mic (raw)" starts muted
    bool denoise = false;            // mic lanes: noise suppression applied in the editor / export
};

struct EditorState {
    bool hasClip = false;            // shows "Editor" in the sidebar
    ClipInfo clip;
    std::vector<EditSection> sections;   // sorted, non-overlapping
    std::vector<EditCaption> captions;
    int selSection = -1, selCaption = -1;
    double in = 0.0, out = 0.0;      // first section start / last section end (kept in sync)
    bool rangeSet = false;
    struct Snapshot {
        std::vector<EditSection> sections;
        std::vector<EditCaption> captions;
    };
    std::vector<Snapshot> undo;
    int dragging = -1;               // 0 section start, 1 section end, 2 playhead, 3 caption
    int dragIndex = -1;
    double dragGrab = 0.0;           // caption drags: where in the caption it was grabbed
    bool wasPlaying = false;
    bool fullscreen = false;
    // Free "look" edits for the whole export.
    int aspect = 0;                  // 0 original, 1 vertical 9:16, 2 square
    float cropX = 0.5f;
    int filter = 0;                  // 0 none, 1 vivid, 2 warm, 3 cool, 4 B&W
    float gain = 1.0f;               // 0 .. 2
    bool fadeIn = false, fadeOut = false;
    std::vector<EditLane> lanes;     // sized from the clip's audio once it's loaded
    std::vector<Bookmark> marks;     // the clip's bookmarks (sidecar file), in source time
    std::wstring marksFor;           // path the marks were loaded for
    int selMark = -1;
    bool openMarkRename = false;
    char markLabel[64] = {};
    bool keepTracks = false;         // export: the mix plus each lane as its own track
    double lastMouseMove = 0.0;
    char title[256] = {};
    bool holdPlayer = false;         // a rename is in flight: keep the file closed until it finishes
    // Export popup
    bool openExport = false;
    int quality = 0;                 // 0 Original, 1 High, 2 Small file
    int sizeLimit = 0;               // 0 none, 1 10 MB (Discord), 2 25 MB, 3 50 MB, 4 100 MB
    bool banner = true;
    char exportName[200] = {};
    uint64_t exportJob = 0;
    bool exporting = false;
    float exportProgress = 0.0f;
    bool exportFinished = false;     // set by the app on ExportDone: the popup closes itself
    std::string exportError;
};

// Clips queued up to be joined into one video.
struct MontageState {
    std::vector<ClipInfo> items;
    bool openExport = false;
    int quality = 1;
    int sizeLimit = 0;
    bool banner = true;
    char name[200] = {};
    uint64_t exportJob = 0;
    bool exporting = false;
    float exportProgress = 0.0f;
    bool exportFinished = false;
    std::string exportError;
};

// Full-window screenshot viewer (zoom, pan, previous/next).
struct ImageViewerState {
    bool open = false;
    std::vector<uint64_t> sequence;
    uint64_t current = 0;
    ClipInfo clip;
    float zoom = 1.0f;               // 1 = fit
    bool actualSize = false;
    ImVec2 pan = ImVec2(0, 0);
};

struct SettingsState {
    int category = 0;
    int capturing = -1;             // hotkey index being bound, kPushToTalkSlot for push-to-talk
    double savedAt = -100.0;        // "Saved" flash
    bool advanced[5] = {};
    double bannerPreviewAt = -100.0;
};
constexpr int kPushToTalkSlot = 100;

struct Snackbar {
    std::string text;
    std::wstring revealPath;        // non-empty: shows "Show in folder"
    double at = -100.0;
    bool error = false;
};

struct UiState {
    Page page = Page::Clips;
    double pageChangedAt = 0.0;
    LibraryState library;
    EditorState editor;
    MontageState montage;
    ImageViewerState viewer;
    SettingsState settings;
    Snackbar snack;
    int welcomeStep = 0;
    std::vector<Action> actions;

    bool suggestionsChanged = true;  // the Suggested highlights row rescans its folder
    bool settingsDirty = false, themeDirty = false, hotkeysDirty = false, engineDirty = false, audioDirty = false, storageDirty = false,
         startupDirty = false;
};

// Everything a view needs, gathered once per frame.
struct AppContext {
    Settings& settings;
    Window& window;
    engine::IClipEngine& engine;
    engine::AudioCapture& audio;
    ClipLibrary& library;
    Thumbnailer& thumbs;
    Filmstrips& strips;
    ImageCache& images;
    MediaPlayer& player;
    Hotkeys& hotkeys;
    UiState& ui;

    engine::EngineStatus status;
    engine::AudioLevels levels;
    SystemSnapshot system;
    std::vector<platform::MonitorDesc> monitors;
    Updater* updater = nullptr;
    Account* account = nullptr;
    InputOverlay* inputOverlay = nullptr;     // null when accounts are off (CS_ACCOUNT_SERVER empty)

    void Do(Action a) { ui.actions.push_back(a); }
    void Changed() { ui.settingsDirty = true; }
    void ChangedEngine() { ui.settingsDirty = ui.engineDirty = true; }
    void ChangedAudio() { ui.settingsDirty = ui.engineDirty = ui.audioDirty = true; }
    void ChangedTheme() { ui.settingsDirty = ui.themeDirty = true; }
    void ChangedHotkeys() { ui.settingsDirty = ui.hotkeysDirty = true; }
    void ChangedStorage() { ui.settingsDirty = ui.storageDirty = ui.engineDirty = true; }
    void ChangedStartup() { ui.settingsDirty = ui.startupDirty = true; }
};

// ---- views (one file each) ----------------------------------------------------------------
void DrawTopBar(AppContext& ctx, float height);
void DrawSidebar(AppContext& ctx, float width, float height);
void DrawLibrary(AppContext& ctx, bool screenshots);    // LibraryView.cpp
void DrawEditor(AppContext& ctx);                        // Editor.cpp
void DrawSettings(AppContext& ctx);                      // SettingsView.cpp
void DrawWelcome(AppContext& ctx);
void DrawUploads(AppContext& ctx);  // the signed-in user's cloud clips
void UploadClip(AppContext& ctx, const ClipInfo& clip);   // queue an upload (or say how to sign in)
   // first run: hotkey, quality, folder
void DrawImageViewer(AppContext& ctx);                   // ImageViewer.cpp (overlay; no-op when closed)

// ---- shared helpers (Library.cpp) ----------------------------------------------------------
void GoTo(AppContext& ctx, Page page);
std::shared_ptr<const ClipList> LibrarySnapshot(AppContext& ctx);
const ClipInfo* FindClip(AppContext& ctx, uint64_t id);
std::string RelativeTime(int64_t unixSeconds);
std::string GameName(const ClipInfo& c);
std::string HotkeyText(const AppContext& ctx, HotkeyAction a);
BannerStyle BannerStyleFor(const Settings& s);
// The export banner drawn over a video shown at `scale` with its top-left at imageMin, t seconds into the clip (Editor.cpp).
void DrawBannerPreview(AppContext& ctx, ImVec2 imageMin, float scale, int videoW, int videoH, double t);

void OpenEditor(AppContext& ctx, const ClipInfo& clip);
ClipAudio& EditorAudio();                                 // the editor clip's audio tracks (waveforms + live mix)
void LeaveEditor(AppContext& ctx);                       // back to Clips; frees the player
void OpenImage(AppContext& ctx, const ClipInfo& clip, const std::vector<int>& sequence);
void CloseImage(AppContext& ctx);

// Responsive grid of cards for `items` (indices into the snapshot): hover preview, click to open,
// Edit and "..." on hover, right-click menu.
void ClipGrid(AppContext& ctx, const char* id, const std::vector<int>& items, bool compact);
void LibraryOverlays(AppContext& ctx);                   // context menu + rename/delete dialogs
void EmptyState(AppContext& ctx, const char* title, const char* body, bool showActions);

void Snack(UiState& ui, const std::string& text, const std::wstring& revealPath = L"", bool error = false);
void DrawSnack(AppContext& ctx);

} // namespace cs::ui
