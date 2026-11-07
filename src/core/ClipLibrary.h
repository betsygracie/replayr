#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cs {

enum class ClipType { Video, Image };

struct ClipInfo {
    uint64_t id = 0;              // stable per path
    std::wstring path;
    std::string title;            // file name without extension
    std::string game;             // per-game sub-folder ("" for files in the root)
    ClipType type = ClipType::Video;
    uint64_t sizeBytes = 0;
    int64_t modified = 0;         // unix seconds
    double duration = 0.0;        // seconds, videos only (0 = unknown)
    int width = 0, height = 0;
    bool favourite = false;
    std::vector<std::string> tags;  // the user's own labels
    int64_t lastOpened = 0;        // unix seconds it was last opened in Replayr (0 = never)
};
using ClipList = std::vector<ClipInfo>;

// Last-write time as the library records it (ClipInfo::modified), 0 if the file is missing.
int64_t FileModifiedUnix(const std::wstring& path);

struct LibraryPolicy {
    bool autoDelete = false;
    int maxAgeDays = 30;
    uint64_t maxBytes = 0;        // 0 = no limit
};

struct LibraryOpResult {
    bool ok = false;
    std::string message;
    std::wstring oldPath, newPath;
};

// Scans the clips folder on a worker thread (and again whenever Windows reports a change in it),
// reads duration/resolution through the Windows property system, and publishes immutable
// snapshots the UI can hold without locking. File operations run off the UI thread too.
class ClipLibrary {
public:
    ClipLibrary();
    ~ClipLibrary();
    void Start(const std::wstring& root, const std::wstring& stateFile);
    void Stop();

    void SetRoot(const std::wstring& root);
    std::wstring Root() const;
    void SetPolicy(const LibraryPolicy& p);
    void Rescan();

    std::shared_ptr<const ClipList> Snapshot() const;
    uint64_t Version() const;              // bumps on every new snapshot
    bool Scanning() const;
    uint64_t TotalBytes() const;

    void SetFavourite(const std::wstring& path, bool favourite);
    void SetTags(const std::wstring& path, const std::vector<std::string>& tags);
    void MarkOpened(const std::wstring& path);   // remembered for the storage clean-up suggestions
    void Rename(const std::wstring& path, const std::wstring& newStem);
    void Delete(const std::wstring& path);   // to the Recycle Bin
    void Move(const std::wstring& path, const std::wstring& destDir);
    bool PollResult(LibraryOpResult& out);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
