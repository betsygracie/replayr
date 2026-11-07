#pragma once
// Checks the latest GitHub release (CS_UPDATE_REPO), downloads a newer Replayr.exe next to the
// running one, and swaps it in on restart: the running exe is renamed to Replayr.old.exe (Windows
// allows renaming a running exe), the new one takes its name, and the old file is deleted on the
// next start. Network work runs on a background thread.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace cs {

enum class UpdateState { Off, Idle, Checking, UpToDate, Downloading, Ready, Failed };

class Updater {
public:
    ~Updater();
    static bool Enabled();                  // CS_UPDATE_REPO is set
    void CleanupOld();                      // at startup: remove the previous version's exe
    void Check(bool download);              // async
    UpdateState State() const { return state_.load(); }
    std::string Version() const;            // of the release found
    std::string Message() const;
    bool Apply();                           // swap the files and start the new exe; the caller then quits

private:
    void Run(bool download);
    std::atomic<UpdateState> state_ { UpdateState::Idle };
    mutable std::mutex mutex_;
    std::string version_, message_;
    std::wstring downloaded_;
    std::thread thread_;
};

} // namespace cs
