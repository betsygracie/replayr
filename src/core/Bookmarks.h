#pragma once
// Bookmarks: moments marked while playing (the bookmark hotkey, or auto-highlights), kept with
// each clip in a sidecar file next to it ("<clip>.replayr.json"), shown as pins in the editor,
// and written into exported videos as MP4 chapters so other players see them too.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cs {

struct Bookmark {
    double t = 0.0;               // seconds from the start of the clip
    std::string label;
    std::string game;
    std::string source = "manual";   // "manual" (hotkey / editor) or "auto" (highlight detection)
    int score = 0;                // auto highlights: 0..100
};

std::wstring SidecarPath(const std::wstring& clipPath);   // "<folder>\<stem>.replayr.json"
std::vector<Bookmark> LoadBookmarks(const std::wstring& clipPath);
bool SaveBookmarks(const std::wstring& clipPath, const std::vector<Bookmark>& marks);   // empty list removes the file
int CountBookmarks(const std::wstring& clipPath);   // quick, for library badges (0 if none)

// Marks made while the buffer or a recording runs, in wall-clock seconds (NowSeconds()).
// When a clip is saved, the marks inside it are taken out with times relative to the clip.
class BookmarkLog {
public:
    void Add(double wallTime, const std::string& game, const std::string& source = "manual", int score = 0, const std::string& label = "");
    std::vector<Bookmark> Range(double start, double end) const;   // marks in [start, end], t relative to start
    size_t Count() const;

private:
    mutable std::mutex m_;
    struct Entry {
        double wall;
        Bookmark b;
    };
    std::vector<Entry> marks_;
};

// Writes (or replaces) Nero-style chapter markers ("chpl" in moov/udta) in an MP4 file.
// Fixes the sample offsets if the movie header sits before the media data.
struct Chapter {
    double start = 0.0;
    std::string title;
};
bool WriteMp4Chapters(const std::wstring& path, std::vector<Chapter> chapters, std::string& error);

} // namespace cs
