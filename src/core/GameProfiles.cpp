#include "core/GameProfiles.h"
#include "core/Log.h"
#include "core/Util.h"
#include "core/Platform.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace cs {

using json = nlohmann::json;

namespace {

std::wstring FileFor(const std::wstring& dir, const std::string& id)
{
    std::wstring safe;
    for (char c : id) safe += (std::isalnum((unsigned char)c) || c == '-' || c == '.') ? (wchar_t)c : L'_';
    return JoinPath(dir, safe + L".json");
}

template <class T>
void Opt(const json& j, const char* k, std::optional<T>& v)
{
    if (j.contains(k) && !j[k].is_null()) {
        try {
            v = j[k].get<T>();
        } catch (...) {
        }
    }
}

template <class T>
void Put(json& j, const char* k, const std::optional<T>& v)
{
    if (v) j[k] = *v;
}

void OptKey(const json& j, const char* k, std::optional<Hotkey>& v)
{
    if (j.contains(k) && j[k].is_object()) v = Hotkey{ j[k].value("vk", 0u), j[k].value("mods", 0u) };
}
void PutKey(json& j, const char* k, const std::optional<Hotkey>& v)
{
    if (v) j[k] = { { "vk", v->vk }, { "mods", v->mods } };
}

} // namespace

int GameProfile::OverrideCount() const
{
    int n = 0;
    n += replaySeconds.has_value() + resolution.has_value() + fps.has_value() + bitrateMbps.has_value() + quality.has_value();
    n += encoder.has_value() + source.has_value() + captureSystem.has_value() + captureMic.has_value() + noiseSuppression.has_value();
    n += separateTracks.has_value() + highlights.has_value() + highlightSensitivity.has_value();
    n += saveKey.has_value() + bookmarkKey.has_value() + screenshotKey.has_value() + folder.has_value();
    return n;
}

void GameProfiles::Load(const std::wstring& dir)
{
    dir_ = dir;
    EnsureDir(dir);
    profiles_.clear();
    for (const std::wstring& f : ListFiles(dir, L"*.json")) {
        json j = json::parse(ReadTextFile(f), nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("id")) continue;
        GameProfile p;
        p.id = j.value("id", std::string());
        p.name = j.value("name", p.id);
        p.launcher = j.value("launcher", std::string());
        p.exe = ToWide(j.value("exe", std::string()));
        p.installDir = ToWide(j.value("installDir", std::string()));
        p.manual = j.value("manual", false);
        p.lastPlayed = j.value("lastPlayed", (int64_t)0);
        p.autoRecord = j.value("autoRecord", false);
        Opt(j, "replaySeconds", p.replaySeconds);
        Opt(j, "resolution", p.resolution);
        Opt(j, "fps", p.fps);
        Opt(j, "bitrateMbps", p.bitrateMbps);
        Opt(j, "quality", p.quality);
        Opt(j, "encoder", p.encoder);
        Opt(j, "source", p.source);
        Opt(j, "captureSystem", p.captureSystem);
        Opt(j, "captureMic", p.captureMic);
        Opt(j, "noiseSuppression", p.noiseSuppression);
        Opt(j, "separateTracks", p.separateTracks);
        Opt(j, "highlights", p.highlights);
        Opt(j, "highlightSensitivity", p.highlightSensitivity);
        OptKey(j, "saveKey", p.saveKey);
        OptKey(j, "bookmarkKey", p.bookmarkKey);
        OptKey(j, "screenshotKey", p.screenshotKey);
        if (j.contains("folder") && j["folder"].is_string()) p.folder = ToWide(j["folder"].get<std::string>());
        if (!p.id.empty()) profiles_[p.id] = p;
    }
}

const GameProfile* GameProfiles::Find(const std::string& id) const
{
    auto it = profiles_.find(id);
    return it == profiles_.end() ? nullptr : &it->second;
}

GameProfile& GameProfiles::Get(const std::string& id, const std::string& name)
{
    GameProfile& p = profiles_[id];
    if (p.id.empty()) {
        p.id = id;
        p.name = name;
    }
    return p;
}

void GameProfiles::Save(const GameProfile& p)
{
    profiles_[p.id] = p;
    json j = { { "id", p.id }, { "name", p.name }, { "launcher", p.launcher }, { "manual", p.manual }, { "lastPlayed", p.lastPlayed },
               { "autoRecord", p.autoRecord } };
    if (!p.exe.empty()) j["exe"] = ToUtf8(p.exe);
    if (!p.installDir.empty()) j["installDir"] = ToUtf8(p.installDir);
    Put(j, "replaySeconds", p.replaySeconds);
    Put(j, "resolution", p.resolution);
    Put(j, "fps", p.fps);
    Put(j, "bitrateMbps", p.bitrateMbps);
    Put(j, "quality", p.quality);
    Put(j, "encoder", p.encoder);
    Put(j, "source", p.source);
    Put(j, "captureSystem", p.captureSystem);
    Put(j, "captureMic", p.captureMic);
    Put(j, "noiseSuppression", p.noiseSuppression);
    Put(j, "separateTracks", p.separateTracks);
    Put(j, "highlights", p.highlights);
    Put(j, "highlightSensitivity", p.highlightSensitivity);
    PutKey(j, "saveKey", p.saveKey);
    PutKey(j, "bookmarkKey", p.bookmarkKey);
    PutKey(j, "screenshotKey", p.screenshotKey);
    if (p.folder) j["folder"] = ToUtf8(*p.folder);
    if (!WriteTextFile(FileFor(dir_, p.id), j.dump(2))) Log("Profiles: couldn't save %s", p.id.c_str());
}

void GameProfiles::Remove(const std::string& id)
{
    profiles_.erase(id);
    platform::DeleteFileNow(FileFor(dir_, id));
}

std::vector<GameProfile> GameProfiles::All() const
{
    std::vector<GameProfile> v;
    for (const auto& kv : profiles_) v.push_back(kv.second);
    return v;
}

std::vector<GameInfo> GameProfiles::ManualGames() const
{
    std::vector<GameInfo> v;
    for (const auto& kv : profiles_)
        if (kv.second.manual) {
            GameInfo g;
            g.id = kv.second.id;
            g.name = kv.second.name;
            g.launcher = "Added by you";
            g.exe = kv.second.exe;
            g.installDir = kv.second.installDir;
            v.push_back(g);
        }
    return v;
}

Settings ApplyProfile(const Settings& base, const GameProfile& p)
{
    Settings s = base;
    if (p.replaySeconds) s.replay.seconds = *p.replaySeconds;
    if (p.resolution) s.video.resolution = *p.resolution;
    if (p.fps) s.video.fps = *p.fps;
    if (p.bitrateMbps) s.video.bitrateMbps = *p.bitrateMbps;
    if (p.quality) s.video.quality = *p.quality;
    if (p.encoder) s.video.encoder = *p.encoder;
    if (p.source) s.video.source = *p.source;
    if (p.captureSystem) s.audio.captureSystem = *p.captureSystem;
    if (p.captureMic) s.audio.captureMic = *p.captureMic;
    if (p.noiseSuppression) s.audio.noiseSuppression = *p.noiseSuppression;
    if (p.separateTracks) s.audio.separateTracks = *p.separateTracks;
    if (p.highlights) s.highlights.enabled = *p.highlights;
    if (p.highlightSensitivity) s.highlights.sensitivity = *p.highlightSensitivity;
    if (p.saveKey) s.hotkeys[(size_t)HotkeyAction::SaveReplay] = *p.saveKey;
    if (p.bookmarkKey) s.hotkeys[(size_t)HotkeyAction::Bookmark] = *p.bookmarkKey;
    if (p.screenshotKey) s.hotkeys[(size_t)HotkeyAction::Screenshot] = *p.screenshotKey;
    // p.folder is a sub-folder name inside the clips folder (so the library still shows those
    // clips): the app passes it as the "game" name the engine files clips under.
    return s;
}

} // namespace cs
