#pragma once
// The editor's view of a clip's audio tracks.
//
// Load() decodes every audio track of the clip on a background thread (Media Foundation, 48 kHz
// stereo float) and builds waveform peaks. Once loaded, Play-along mixes the tracks live through
// WASAPI with per-lane gain / mute / solo, following the video player's position, so what you
// hear in the editor is what the export will contain. The video player's own audio is muted
// while this plays.
//
// Track layout written by the recorder (read back by count):
//   3+ tracks: Mixed, Game, Mic(, more)  -> lanes are Game, Mic (Mixed is just their sum)
//   2 tracks:  Game, Mic (older clips)   -> lanes Game, Mic
//   1 track:   Audio                     -> lane Audio
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cs {

struct ClipAudioLane {
    std::string name;
    int track = 0;                 // source audio track index in the file
};

class ClipAudio {
public:
    ClipAudio();
    ~ClipAudio();

    void Load(const std::wstring& path);   // async; replaces whatever was loaded
    void Clear();

    const std::wstring& Path() const;
    bool Loading() const;
    bool Ready() const;
    int TrackCount() const;                 // audio tracks in the file
    std::vector<ClipAudioLane> Lanes() const;
    double Duration() const;

    // Waveform peaks for a source track: one value (0..1) per 1/kPeaksPerSecond seconds.
    static constexpr int kPeaksPerSecond = 50;
    std::vector<float> Peaks(int track) const;

    // Live mix. gains[i] is the gain for lane i (0 when muted / not soloed); master on top.
    void SetGains(const std::vector<float>& laneGains, float master);
    // Called every UI frame with the video player's state.
    void Follow(double position, bool playing, double rate);
    bool Playing() const;          // the live mix is running (the video player should be muted)

    // Noise suppression on one track for the preview (computed in the background the first time).
    void SetDenoise(int track, bool on);
    bool DenoiseReady(int track) const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace cs
