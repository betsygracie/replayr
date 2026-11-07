#pragma once
#include "media/Banner.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace cs::engine {

enum class ExportQuality { Original = 0, High = 1, Small = 2 };

// One kept section of a clip. Sections play back to back in the output, so cutting out the middle
// of a clip is two sections, and a montage is sections from several clips.
struct Segment {
    std::wstring source;
    double start = 0.0, end = 0.0;           // seconds in the source
    double speed = 1.0;                      // 0.25 .. 4; audio is resampled along with it
};

// Text burned into the video, in output time.
struct Caption {
    std::wstring text;
    double start = 0.0, end = 0.0;
    float x = 0.5f, y = 0.82f;               // centre of the text, as a fraction of the frame
    int size = 1;                            // 0 small, 1 medium, 2 large
};

// Free "look" edits applied to the whole export.
struct Look {
    int aspect = 0;          // 0 original, 1 vertical 9:16, 2 square 1:1
    float cropX = 0.5f;      // 0 = left edge .. 1 = right edge (for vertical / square)
    int filter = 0;          // 0 none, 1 vivid, 2 warm, 3 cool, 4 black & white
    float volume = 1.0f;     // 0 .. 2
    bool fadeIn = false, fadeOut = false;   // 0.5 s from / to black and silence
    bool Any() const { return aspect != 0 || filter != 0 || volume != 1.0f || fadeIn || fadeOut; }
};

struct TranscodeRequest {
    std::vector<Segment> segments;
    std::wstring destination;
    ExportQuality quality = ExportQuality::Original;
    double targetMB = 0.0;                   // > 0: fit the file under this size (picks bitrate, then resolution / fps)
    bool banner = false;
    BannerStyle bannerStyle;
    std::vector<Caption> captions;
    bool hardware = true;                    // GPU encoder when there is one
    Look look;
    // Audio mix: mix[o][k] is the gain from the source's k-th audio track into output track o.
    // Empty = each source track goes to the same output track unchanged (up to 4).
    std::vector<std::vector<float>> mix;
};

// Length of the output in seconds.
double OutputLength(const std::vector<Segment>& segments);

// Decodes each section with Media Foundation (scaling later clips to the first one's size), applies
// speed changes, burns in the banner and captions, and encodes H.264 + AAC (48 kHz stereo) to an MP4.
// Video is written on a fixed frame grid and audio is padded/trimmed per section, so sound stays in
// sync across cuts. Blocking: run on a worker thread.
bool Transcode(const TranscodeRequest& req, const std::function<void(float)>& progress, const std::atomic<bool>* cancel, std::string& error);

// Time of the last video keyframe at or before `t` (seconds), or -1 if the file can't be read.
double KeyframeAtOrBefore(const std::wstring& path, double t);

// "Couldn't export: the disk is full" style text for an HRESULT.
std::string DescribeFailure(long hr);

} // namespace cs::engine
