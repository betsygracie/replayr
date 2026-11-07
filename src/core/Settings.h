#pragma once
#include <array>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cs {

// Win32 modifier bits (same values as MOD_ALT etc.) so this header stays Windows-free.
enum : uint32_t { kModAlt = 0x0001, kModCtrl = 0x0002, kModShift = 0x0004, kModWin = 0x0008 };

struct Hotkey {
    uint32_t vk = 0;
    uint32_t mods = 0;
    bool IsSet() const { return vk != 0; }
    bool operator==(const Hotkey& o) const { return vk == o.vk && mods == o.mods; }
    bool operator!=(const Hotkey& o) const { return !(*this == o); }
};

enum class HotkeyAction : int {
    SaveReplay = 0,
    ToggleRecording,
    ToggleReplayBuffer,
    Screenshot,
    ScreenshotRegion,
    ScreenshotWindow,
    Count
};
constexpr int kHotkeyCount = (int)HotkeyAction::Count;
const char* HotkeyActionLabel(HotkeyAction a);
const char* HotkeyActionKey(HotkeyAction a);   // JSON key

struct VideoSettings {
    // "game" (game window only), "monitor:<index>" / "window:foreground" (whole screen),
    // or "both:<index>" (game + that screen, saved as two files).
    std::string source = "monitor:0";
    bool hideBorder = true;             // game capture without the yellow border (Windows 11)
    bool hideApp = true;                // keep Replayr's own window out of recordings and screenshots
    int resolution = 3;                 // 0 native, 1 2160p, 2 1440p, 3 1080p, 4 720p
    int fps = 60;                       // 30 / 60 / 120 / 144
    int bitrateMbps = 25;
    std::string encoder = "auto";       // auto / nvenc / amf / qsv / x264
    std::string codec = "h264";         // h264 / hevc / av1
    int quality = 1;                    // 0 performance, 1 balanced, 2 quality
    bool captureCursor = true;
};

struct ReplaySettings {
    bool enabled = true;
    int seconds = 30;                   // 15 s .. 10 min
};

struct AppTrack {                       // per-app audio track (Windows 10 2004+)
    std::string name;
    bool include = true;
    float volume = 1.0f;
};

struct AudioSettings {
    std::wstring outputDeviceId;        // empty = system default
    std::wstring inputDeviceId;
    int sampleRate = 48000;
    bool separateTracks = true;         // system / mic / apps on their own tracks
    bool captureSystem = true;
    float systemVolume = 1.0f;
    bool systemMuted = false;
    bool captureMic = false;
    float micVolume = 1.0f;
    bool micMuted = false;
    bool pushToTalk = false;
    uint32_t pushToTalkKey = 0x56;      // V
    std::vector<AppTrack> apps;
};

struct StorageSettings {
    std::wstring outputFolder;          // empty = Videos\Replayr
    bool perGameFolders = true;
    int maxLibraryGB = 0;               // 0 = no limit
    bool autoDelete = false;
    int autoDeleteDays = 30;
    bool screenshotToClipboard = true;
    int screenshotFormat = 0;           // 0 PNG, 1 JPG
};

struct InputDisplaySettings {          // keyboard & mouse overlay
    bool enabled = false;
    int mode = 0;                     // 0 screen + clips, 1 screen only, 2 clips only
    int corner = 2;                   // 0 TL, 1 TR, 2 BL, 3 BR
    int size = 1;                     // 0 S, 1 M, 2 L
    int show = 0;                     // 0 keyboard + mouse, 1 keyboard, 2 mouse
    float opacity = 0.9f;
    bool gamesOnly = true;            // hide while a normal (non-fullscreen) app is in front
};

struct GeneralSettings {
    bool startWithWindows = false;
    bool startMinimized = false;
    bool minimizeToTray = false;
    bool closeToTray = true;
    bool sounds = true;
    bool toasts = true;
    int toastCorner = 1;                // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    float accent[3] = { 0.545f, 0.247f, 0.988f };   // #8B3FFC violet
    float accent2[3] = { 0.761f, 0.122f, 0.878f };  // #C21FE0 magenta (gradient end)
    float glass = 0.6f;                 // glass panel strength 0..1
    int backdrop = 0;                   // 0 Replayr background, 1 Mica, 2 Acrylic (Win 10: blur behind)
    int designVersion = 3;              // bumped when defaults change, to migrate old settings
    bool animations = true;             // off = "Reduce motion": fades instead of springs, tilts and drift
    int background = 0;                 // 0 animated gradient, 1 static, 2 solid (low-end PCs)
    bool uiSounds = false;              // quiet hover/click sounds
    bool autoUpdate = true;             // check GitHub releases at startup (when CS_UPDATE_REPO is set)
    bool setupDone = false;             // the first-run welcome has been completed
    std::string accountServer;          // overrides CS_ACCOUNT_SERVER (not shown in the UI)
    float uiScale = 1.0f;
    std::wstring ffmpegPath;            // empty = auto-detect
    bool trayHintShown = false;
};

struct EditorSettings {
    int exportQuality = 0;              // 0 Original, 1 High, 2 Small file
    bool loopSelection = true;
    float volume = 0.8f;
};

// The animated "Clipped using Replayr" banner on exported clips (and optionally screenshots).
struct BannerSettings {
    bool enabled = true;                // default for the export popup's toggle
    int corner = 3;                     // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
    int mode = 0;                       // 0 5 s at the start, 1 always visible
    bool screenshots = false;
};

struct UiPrefs {
    int lastPage = 0;                   // ui::Page
    int clipSort = 0;
    int shotSort = 0;
};

struct WindowState {
    int x = INT_MIN, y = INT_MIN, w = 0, h = 0;
    bool maximized = false;
};

struct Settings {
    VideoSettings video;
    ReplaySettings replay;
    AudioSettings audio;
    StorageSettings storage;
    GeneralSettings general;
    EditorSettings editor;
    BannerSettings banner;
    InputDisplaySettings inputDisplay;
    WindowState window;
    UiPrefs ui;
    std::array<Hotkey, kHotkeyCount> hotkeys{};

    Settings();                                   // fills default hotkeys
    static std::array<Hotkey, kHotkeyCount> DefaultHotkeys();
    std::wstring ClipsFolder() const;             // resolved output folder
};

bool LoadSettings(const std::wstring& path, Settings& out, std::string* error = nullptr);
std::string SerializeSettings(const Settings& s);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);

// Writes settings on a background thread so a slow disk or antivirus scan never stalls the UI.
// Save() serializes on the caller's thread (microseconds) and hands the string over.
class SettingsSaver {
public:
    ~SettingsSaver();
    void Start(const std::wstring& path);
    void Stop();                                  // flushes pending data
    void Save(const Settings& s);

private:
    void Run();
    std::wstring path_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string pending_;
    bool hasPending_ = false;
    bool quit_ = false;
};

} // namespace cs
