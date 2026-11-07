#pragma once
// Knows which games are installed and notices when one starts and stops.
//
// Installed games come from the launchers' own files: Steam (libraryfolders.vdf + app
// manifests), Epic (launcher manifests), and Battle.net / EA / Ubisoft / Riot (their uninstall
// entries in the registry), plus a built-in list of popular games' exe names and games the user
// adds by hand. A running process counts as "in game" only when its exe lives in a known game
// folder (or has a known name) AND it has a large visible window, so launchers, updaters and
// anti-cheat helpers don't trigger it.
//
// Safe for anti-cheat: only process names / paths (PROCESS_QUERY_LIMITED_INFORMATION) and window
// sizes are read. Nothing is injected into, or read from, a game's memory.
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cs {

struct GameInfo {
    std::string id;            // stable key: lower-case exe name, or "steam:<appid>", "epic:<name>"...
    std::string name;          // "Valorant"
    std::string launcher;      // "Steam", "Epic", "Battle.net", "EA", "Ubisoft", "Riot", "Built-in", "Added by you"
    std::wstring installDir;   // folder the game's exes live in (may be empty for built-in names)
    std::wstring exe;          // a known exe (manual adds, built-ins by name, or the one seen running)
    std::wstring icon;         // PNG extracted from the exe (cached), "" until there is one
};

enum class GameEventType { Started, Stopped };
struct GameEvent {
    GameEventType type;
    GameInfo game;
};

class GameDetector {
public:
    ~GameDetector();
    void Start(const std::wstring& cacheDir);     // scans the launchers, then watches processes
    void Stop();
    void Rescan();                                // launcher libraries again (e.g. after installing a game)
    void SetManual(const std::vector<GameInfo>& games);   // the user's own additions (from profiles)

    std::vector<GameInfo> Games() const;          // everything known, sorted by name
    bool Scanning() const { return scanning_.load(); }
    GameInfo Active() const;                      // the game being played now (id empty if none)
    bool PollEvent(GameEvent& out);

    // Exe -> display name for a picked file ("C:\...\Game.exe" -> "Game").
    static std::string NameFromExe(const std::wstring& exe);

private:
    void Run();
    void ScanLibraries();
    void Watch();
    std::string IconFor(GameInfo& g);

    std::thread thread_;
    std::atomic<bool> quit_ { false }, rescan_ { true }, scanning_ { false };
    mutable std::mutex m_;
    std::vector<GameInfo> scanned_, manual_;
    GameInfo active_;
    unsigned long activePid_ = 0;
    std::deque<GameEvent> events_;
    std::wstring cacheDir_;
};

} // namespace cs
