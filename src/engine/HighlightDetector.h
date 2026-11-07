#pragma once
// Auto-highlights: finds sudden loud moments in the game audio and the mic, live and locally.
//
// Each source is measured in 50 ms windows (RMS, dBFS) against a rolling baseline (a low
// percentile of the last 15 s, so loud moments don't raise their own bar). A spike is the level
// staying well above the baseline for at least 300 ms. Spikes less than 5 s apart are one
// highlight; it's reported 5 s after the last spike, with a 0-100 score from how big the jump
// was, how long it lasted, whether game and mic spiked together (a reaction) and how many spikes
// there were (a fight scores higher than one bang). Nothing is stored or sent anywhere.
#include <cstdint>
#include <deque>
#include <vector>

namespace cs::engine {

struct HighlightConfig {
    bool enabled = true;
    int sensitivity = 1;      // 0 low, 1 medium, 2 high
    int sources = 2;          // 0 game audio, 1 mic, 2 both
    bool operator==(const HighlightConfig&) const = default;
};

struct Highlight {
    double start = 0.0, end = 0.0;   // seconds on the caller's clock (the first spike .. the last)
    int score = 0;
    bool game = false, mic = false;  // which sources spiked
};

class HighlightDetector {
public:
    void Configure(const HighlightConfig& c) { cfg_ = c; }
    void Reset();
    // Interleaved stereo float at 48 kHz, starting at clock time `t` (seconds). Either may be null.
    void Feed(const float* game, const float* mic, int64_t frames, double t);
    bool Poll(Highlight& out);   // finished highlights, oldest first

private:
    struct Source {
        std::vector<float> history;   // dB of the last 15 s of windows
        size_t head = 0;
        double acc = 0.0;
        int accN = 0;
        int above = 0;                // consecutive windows above the threshold
        double spikeStart = 0.0;
        float spikePeak = 0.0f;       // dB over the baseline
    };
    struct Spike {
        double start, end;
        float excess;
        bool mic;
    };
    void Window(Source& s, bool mic, double tEnd);
    void Close(double now, bool force);

    HighlightConfig cfg_;
    Source game_, mic_;
    std::vector<Spike> cluster_;
    double lastEmitEnd_ = -1e9, now_ = 0.0;
    std::deque<Highlight> ready_;
};

} // namespace cs::engine
