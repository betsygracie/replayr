#include "core/Settings.h"
#include "AppInfo.h"

#include <algorithm>
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <cstdio>

using json = nlohmann::json;

namespace cs {

const char* HotkeyActionLabel(HotkeyAction a)
{
    switch (a) {
    case HotkeyAction::SaveReplay: return "Save replay";
    case HotkeyAction::ToggleRecording: return "Start / stop recording";
    case HotkeyAction::ToggleReplayBuffer: return "Replay buffer on / off";
    case HotkeyAction::Screenshot: return "Screenshot (full screen)";
    case HotkeyAction::ScreenshotRegion: return "Screenshot (region)";
    case HotkeyAction::ScreenshotWindow: return "Screenshot (active window)";
    default: return "?";
    }
}

const char* HotkeyActionKey(HotkeyAction a)
{
    switch (a) {
    case HotkeyAction::SaveReplay: return "saveReplay";
    case HotkeyAction::ToggleRecording: return "toggleRecording";
    case HotkeyAction::ToggleReplayBuffer: return "toggleReplayBuffer";
    case HotkeyAction::Screenshot: return "screenshot";
    case HotkeyAction::ScreenshotRegion: return "screenshotRegion";
    case HotkeyAction::ScreenshotWindow: return "screenshotWindow";
    default: return "unknown";
    }
}

std::array<Hotkey, kHotkeyCount> Settings::DefaultHotkeys()
{
    // ShadowPlay-style defaults: modifier combos, so games never lose a plain key (RegisterHotKey eats it).
    std::array<Hotkey, kHotkeyCount> h{};
    h[(int)HotkeyAction::SaveReplay] = { 0x79, kModAlt };                        // Alt+F10
    h[(int)HotkeyAction::ToggleRecording] = { 0x78, kModAlt };                   // Alt+F9
    h[(int)HotkeyAction::ToggleReplayBuffer] = { 0x79, kModAlt | kModShift };    // Alt+Shift+F10
    h[(int)HotkeyAction::Screenshot] = { 0x70, kModAlt };                        // Alt+F1
    h[(int)HotkeyAction::ScreenshotRegion] = { 0x2C, kModCtrl };                 // Ctrl+PrtSc
    h[(int)HotkeyAction::ScreenshotWindow] = { 0x70, kModAlt | kModShift };      // Alt+Shift+F1
    return h;
}

Settings::Settings() : hotkeys(DefaultHotkeys()) {}

std::wstring Settings::ClipsFolder() const
{
    return storage.outputFolder.empty() ? DefaultClipsDir() : storage.outputFolder;
}

// ---- JSON helpers -----------------------------------------------------------

template <typename T>
static void Get(const json& j, const char* key, T& v)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try { v = it->get<T>(); } catch (...) {}
}

static void GetW(const json& j, const char* key, std::wstring& v)
{
    std::string s;
    bool found = j.contains(key) && j[key].is_string();
    if (!found) return;
    Get(j, key, s);
    v = ToWide(s);
}

static void GetColor(const json& j, const char* key, float (&c)[3])
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return;
    for (int i = 0; i < 3; ++i)
        if ((*it)[i].is_number()) c[i] = Clamp((*it)[i].get<float>(), 0.0f, 1.0f);
}

bool LoadSettings(const std::wstring& path, Settings& s, std::string* error)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (error) *error = "no settings file yet";
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "settings.json is not valid JSON";
        // Keep the broken file for the user and start with defaults.
        CopyFileW(path.c_str(), (path + L".broken").c_str(), FALSE);
        return false;
    }

    if (j.contains("video")) {
        const json& v = j["video"];
        Get(v, "source", s.video.source);
        Get(v, "resolution", s.video.resolution);
        Get(v, "fps", s.video.fps);
        Get(v, "bitrateMbps", s.video.bitrateMbps);
        Get(v, "encoder", s.video.encoder);
        Get(v, "codec", s.video.codec);
        Get(v, "quality", s.video.quality);
        Get(v, "captureCursor", s.video.captureCursor);
        Get(v, "hideBorder", s.video.hideBorder);
        Get(v, "hideApp", s.video.hideApp);
    }
    if (j.contains("replay")) {
        const json& r = j["replay"];
        Get(r, "enabled", s.replay.enabled);
        Get(r, "seconds", s.replay.seconds);
    }
    if (j.contains("audio")) {
        const json& a = j["audio"];
        GetW(a, "outputDevice", s.audio.outputDeviceId);
        GetW(a, "inputDevice", s.audio.inputDeviceId);
        Get(a, "sampleRate", s.audio.sampleRate);
        Get(a, "separateTracks", s.audio.separateTracks);
        Get(a, "captureSystem", s.audio.captureSystem);
        Get(a, "systemVolume", s.audio.systemVolume);
        Get(a, "systemMuted", s.audio.systemMuted);
        Get(a, "captureMic", s.audio.captureMic);
        Get(a, "micVolume", s.audio.micVolume);
        Get(a, "noiseSuppression", s.audio.noiseSuppression);
        Get(a, "suppressionStrength", s.audio.suppressionStrength);
        Get(a, "noiseGate", s.audio.noiseGate);
        Get(a, "gateDb", s.audio.gateDb);
        Get(a, "highPass", s.audio.highPass);
        Get(a, "keepRawMic", s.audio.keepRawMic);
        Get(a, "micMuted", s.audio.micMuted);
        Get(a, "pushToTalk", s.audio.pushToTalk);
        Get(a, "pushToTalkKey", s.audio.pushToTalkKey);
        if (a.contains("apps") && a["apps"].is_array()) {
            s.audio.apps.clear();
            for (const json& t : a["apps"]) {
                AppTrack tr;
                Get(t, "name", tr.name);
                Get(t, "include", tr.include);
                Get(t, "volume", tr.volume);
                if (!tr.name.empty()) s.audio.apps.push_back(tr);
            }
        }
    }
    if (j.contains("storage")) {
        const json& st = j["storage"];
        GetW(st, "outputFolder", s.storage.outputFolder);
        // Renamed from Clipstream: follow the clips folder that was moved on first start.
        if (s.storage.outputFolder.size() > 11 && s.storage.outputFolder.substr(s.storage.outputFolder.size() - 11) == L"\\Clipstream" &&
            GetFileAttributesW(s.storage.outputFolder.c_str()) == INVALID_FILE_ATTRIBUTES)
            s.storage.outputFolder = s.storage.outputFolder.substr(0, s.storage.outputFolder.size() - 11) + L"\\" CS_APP_NAME_W;
        Get(st, "perGameFolders", s.storage.perGameFolders);
        Get(st, "maxLibraryGB", s.storage.maxLibraryGB);
        Get(st, "autoDelete", s.storage.autoDelete);
        Get(st, "autoDeleteDays", s.storage.autoDeleteDays);
        Get(st, "screenshotToClipboard", s.storage.screenshotToClipboard);
        Get(st, "screenshotFormat", s.storage.screenshotFormat);
    }
    if (j.contains("general")) {
        const json& g = j["general"];
        Get(g, "startWithWindows", s.general.startWithWindows);
        Get(g, "startMinimized", s.general.startMinimized);
        Get(g, "minimizeToTray", s.general.minimizeToTray);
        s.general.setupDone = true;   // a settings file from an earlier version: skip the welcome
        Get(g, "setupDone", s.general.setupDone);
        Get(g, "autoUpdate", s.general.autoUpdate);
        Get(g, "accountServer", s.general.accountServer);
        Get(g, "closeToTray", s.general.closeToTray);
        Get(g, "sounds", s.general.sounds);
        Get(g, "toasts", s.general.toasts);
        Get(g, "toastCorner", s.general.toastCorner);
        GetColor(g, "accent", s.general.accent);
        GetColor(g, "accent2", s.general.accent2);
        Get(g, "glass", s.general.glass);
        Get(g, "backdrop", s.general.backdrop);
        int dv = 1;
        Get(g, "designVersion", dv);
        if (dv < 3) {   // settings from earlier builds: move to the violet glass defaults
            const GeneralSettings d;
            for (int i = 0; i < 3; ++i) {
                s.general.accent[i] = d.accent[i];
                s.general.accent2[i] = d.accent2[i];
            }
            s.general.backdrop = d.backdrop;
        }
        Get(g, "animations", s.general.animations);
        Get(g, "background", s.general.background);
        Get(g, "uiSounds", s.general.uiSounds);
        s.general.background = std::clamp(s.general.background, 0, 2);
        Get(g, "uiScale", s.general.uiScale);
        GetW(g, "ffmpegPath", s.general.ffmpegPath);
        Get(g, "trayHintShown", s.general.trayHintShown);
    }
    if (j.contains("editor")) {
        const json& e = j["editor"];
        Get(e, "exportQuality", s.editor.exportQuality);
        Get(e, "loopSelection", s.editor.loopSelection);
        Get(e, "volume", s.editor.volume);
        s.editor.exportQuality = Clamp(s.editor.exportQuality, 0, 2);
    }
    if (j.contains("banner")) {
        const json& b = j["banner"];
        Get(b, "enabled", s.banner.enabled);
        Get(b, "corner", s.banner.corner);
        Get(b, "mode", s.banner.mode);
        Get(b, "screenshots", s.banner.screenshots);
        s.banner.corner = Clamp(s.banner.corner, 0, 3);
        s.banner.mode = Clamp(s.banner.mode, 0, 1);
    }
    if (j.contains("window")) {
        const json& w = j["window"];
        Get(w, "x", s.window.x);
        Get(w, "y", s.window.y);
        Get(w, "w", s.window.w);
        Get(w, "h", s.window.h);
        Get(w, "maximized", s.window.maximized);
    }
    if (j.contains("ui")) {
        const json& u = j["ui"];
        Get(u, "lastPage", s.ui.lastPage);
        Get(u, "clipSort", s.ui.clipSort);
        Get(u, "shotSort", s.ui.shotSort);
    }
    if (j.contains("inputDisplay")) {
        const json& d = j["inputDisplay"];
        Get(d, "enabled", s.inputDisplay.enabled);
        Get(d, "mode", s.inputDisplay.mode);
        Get(d, "corner", s.inputDisplay.corner);
        Get(d, "size", s.inputDisplay.size);
        Get(d, "show", s.inputDisplay.show);
        Get(d, "opacity", s.inputDisplay.opacity);
        Get(d, "gamesOnly", s.inputDisplay.gamesOnly);
        s.inputDisplay.mode = Clamp(s.inputDisplay.mode, 0, 2);
        s.inputDisplay.corner = Clamp(s.inputDisplay.corner, 0, 3);
        s.inputDisplay.size = Clamp(s.inputDisplay.size, 0, 2);
        s.inputDisplay.show = Clamp(s.inputDisplay.show, 0, 2);
        s.inputDisplay.opacity = Clamp(s.inputDisplay.opacity, 0.2f, 1.0f);
    }
    if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
        const json& h = j["hotkeys"];
        for (int i = 0; i < kHotkeyCount; ++i) {
            const char* key = HotkeyActionKey((HotkeyAction)i);
            if (!h.contains(key) || !h[key].is_object()) continue;
            Get(h[key], "vk", s.hotkeys[i].vk);
            Get(h[key], "mods", s.hotkeys[i].mods);
        }
    }

    // Sanitize anything a hand-edited file could break.
    if (s.video.fps != 30 && s.video.fps != 60 && s.video.fps != 120 && s.video.fps != 144) s.video.fps = 60;
    s.video.resolution = Clamp(s.video.resolution, 0, 4);
    s.video.bitrateMbps = Clamp(s.video.bitrateMbps, 2, 200);
    s.video.quality = Clamp(s.video.quality, 0, 2);
    s.replay.seconds = Clamp(s.replay.seconds, 15, 600);
    s.audio.systemVolume = Clamp(s.audio.systemVolume, 0.0f, 2.0f);
    s.audio.micVolume = Clamp(s.audio.micVolume, 0.0f, 2.0f);
    s.audio.suppressionStrength = Clamp(s.audio.suppressionStrength, 0, 2);
    s.audio.gateDb = Clamp(s.audio.gateDb, -80.0f, -10.0f);
    if (s.audio.sampleRate != 44100 && s.audio.sampleRate != 48000) s.audio.sampleRate = 48000;
    s.storage.maxLibraryGB = Clamp(s.storage.maxLibraryGB, 0, 4096);
    s.storage.autoDeleteDays = Clamp(s.storage.autoDeleteDays, 1, 3650);
    s.storage.screenshotFormat = Clamp(s.storage.screenshotFormat, 0, 1);
    s.general.toastCorner = Clamp(s.general.toastCorner, 0, 3);
    s.general.glass = Clamp(s.general.glass, 0.0f, 1.0f);
    s.general.backdrop = Clamp(s.general.backdrop, 0, 3);
    s.general.uiScale = Clamp(s.general.uiScale, 0.75f, 1.75f);
    s.editor.exportQuality = Clamp(s.editor.exportQuality, 0, 2);
    s.editor.volume = Clamp(s.editor.volume, 0.0f, 1.0f);
    return true;
}

std::string SerializeSettings(const Settings& s)
{
    json j;
    j["version"] = 1;
    j["video"] = {
        { "source", s.video.source }, { "resolution", s.video.resolution }, { "fps", s.video.fps },
        { "bitrateMbps", s.video.bitrateMbps }, { "encoder", s.video.encoder }, { "codec", s.video.codec },
        { "quality", s.video.quality }, { "captureCursor", s.video.captureCursor }, { "hideBorder", s.video.hideBorder }, { "hideApp", s.video.hideApp },
    };
    j["replay"] = { { "enabled", s.replay.enabled }, { "seconds", s.replay.seconds } };

    json apps = json::array();
    for (const AppTrack& t : s.audio.apps)
        apps.push_back({ { "name", t.name }, { "include", t.include }, { "volume", t.volume } });
    j["audio"] = {
        { "outputDevice", ToUtf8(s.audio.outputDeviceId) }, { "inputDevice", ToUtf8(s.audio.inputDeviceId) },
        { "sampleRate", s.audio.sampleRate }, { "separateTracks", s.audio.separateTracks },
        { "captureSystem", s.audio.captureSystem }, { "systemVolume", s.audio.systemVolume },
        { "systemMuted", s.audio.systemMuted }, { "captureMic", s.audio.captureMic },
        { "micVolume", s.audio.micVolume }, { "micMuted", s.audio.micMuted },
          { "noiseSuppression", s.audio.noiseSuppression }, { "suppressionStrength", s.audio.suppressionStrength },
          { "noiseGate", s.audio.noiseGate }, { "gateDb", s.audio.gateDb }, { "highPass", s.audio.highPass }, { "keepRawMic", s.audio.keepRawMic },
        { "pushToTalk", s.audio.pushToTalk }, { "pushToTalkKey", s.audio.pushToTalkKey }, { "apps", apps },
    };
    j["storage"] = {
        { "outputFolder", ToUtf8(s.storage.outputFolder) }, { "perGameFolders", s.storage.perGameFolders },
        { "maxLibraryGB", s.storage.maxLibraryGB }, { "autoDelete", s.storage.autoDelete },
        { "autoDeleteDays", s.storage.autoDeleteDays }, { "screenshotToClipboard", s.storage.screenshotToClipboard },
        { "screenshotFormat", s.storage.screenshotFormat },
    };
    j["general"] = {
        { "startWithWindows", s.general.startWithWindows }, { "startMinimized", s.general.startMinimized },
        { "minimizeToTray", s.general.minimizeToTray }, { "closeToTray", s.general.closeToTray },
        { "sounds", s.general.sounds }, { "toasts", s.general.toasts }, { "toastCorner", s.general.toastCorner },
        { "accent", { s.general.accent[0], s.general.accent[1], s.general.accent[2] } },
        { "accent2", { s.general.accent2[0], s.general.accent2[1], s.general.accent2[2] } },
        { "glass", s.general.glass }, { "backdrop", s.general.backdrop }, { "designVersion", 3 }, { "animations", s.general.animations }, { "background", s.general.background }, { "uiSounds", s.general.uiSounds }, { "autoUpdate", s.general.autoUpdate }, { "setupDone", s.general.setupDone }, { "accountServer", s.general.accountServer },
        { "uiScale", s.general.uiScale }, { "ffmpegPath", ToUtf8(s.general.ffmpegPath) },
        { "trayHintShown", s.general.trayHintShown },
    };
    j["editor"] = {
        { "exportQuality", s.editor.exportQuality }, { "loopSelection", s.editor.loopSelection }, { "volume", s.editor.volume },
    };
    j["banner"] = {
        { "enabled", s.banner.enabled }, { "corner", s.banner.corner }, { "mode", s.banner.mode }, { "screenshots", s.banner.screenshots },
    };
    j["window"] = { { "x", s.window.x }, { "y", s.window.y }, { "w", s.window.w }, { "h", s.window.h },
                    { "maximized", s.window.maximized } };
    j["ui"] = { { "lastPage", s.ui.lastPage }, { "clipSort", s.ui.clipSort }, { "shotSort", s.ui.shotSort } };
    json hk = json::object();
    for (int i = 0; i < kHotkeyCount; ++i)
        hk[HotkeyActionKey((HotkeyAction)i)] = { { "vk", s.hotkeys[i].vk }, { "mods", s.hotkeys[i].mods } };
    j["hotkeys"] = hk;
    j["inputDisplay"] = { { "enabled", s.inputDisplay.enabled }, { "mode", s.inputDisplay.mode }, { "corner", s.inputDisplay.corner },
                          { "size", s.inputDisplay.size }, { "show", s.inputDisplay.show }, { "opacity", s.inputDisplay.opacity },
                          { "gamesOnly", s.inputDisplay.gamesOnly } };
    return j.dump(2);
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data)
{
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != data.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

// ---- SettingsSaver -----------------------------------------------------------

SettingsSaver::~SettingsSaver() { Stop(); }

void SettingsSaver::Start(const std::wstring& path)
{
    path_ = path;
    quit_ = false;
    thread_ = std::thread([this] { Run(); });
}

void SettingsSaver::Stop()
{
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void SettingsSaver::Save(const Settings& s)
{
    std::string text = SerializeSettings(s);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(text);
        hasPending_ = true;
    }
    cv_.notify_all();
}

void SettingsSaver::Run()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this] { return hasPending_ || quit_; });
        if (hasPending_) {
            std::string text = std::move(pending_);
            hasPending_ = false;
            lock.unlock();
            if (!WriteFileAtomic(path_, text)) Log("Could not write settings to disk");
            lock.lock();
            continue;   // pick up anything saved meanwhile before quitting
        }
        if (quit_) break;
    }
}

} // namespace cs
