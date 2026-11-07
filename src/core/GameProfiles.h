#pragma once
// Per-game profiles: each game can override any of the main settings ("Using default" until
// changed), auto-start the replay buffer when it launches, and remember when it was last played.
// One JSON file per game in %APPDATA%\Replayr\profiles\.
#include "core/GameDetector.h"
#include "core/Settings.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cs {

struct GameProfile {
    std::string id, name, launcher;
    std::wstring exe, installDir;    // for games added by hand
    bool manual = false;
    int64_t lastPlayed = 0;          // unix seconds
    bool autoRecord = false;         // start the replay buffer when it launches, stop when it closes

    // Overrides (empty = use the main setting).
    std::optional<int> replaySeconds, resolution, fps, bitrateMbps, quality;
    std::optional<std::string> encoder, source;
    std::optional<bool> captureSystem, captureMic, noiseSuppression, separateTracks;
    std::optional<bool> highlights;
    std::optional<int> highlightSensitivity;
    std::optional<Hotkey> saveKey, bookmarkKey, screenshotKey;
    std::optional<std::wstring> folder;   // sub-folder of the clips folder ("Valorant ranked")

    int OverrideCount() const;
};

class GameProfiles {
public:
    void Load(const std::wstring& dir);
    const GameProfile* Find(const std::string& id) const;
    GameProfile& Get(const std::string& id, const std::string& name);   // creates it if needed
    void Save(const GameProfile& p);
    void Remove(const std::string& id);
    std::vector<GameProfile> All() const;
    std::vector<GameInfo> ManualGames() const;

private:
    std::wstring dir_;
    std::map<std::string, GameProfile> profiles_;
};

// The main settings with a game's overrides applied on top (the settings used while it runs).
Settings ApplyProfile(const Settings& base, const GameProfile& p);

} // namespace cs
