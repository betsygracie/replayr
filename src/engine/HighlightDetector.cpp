#include "engine/HighlightDetector.h"

#include <algorithm>
#include <cmath>

namespace cs::engine {

namespace {
constexpr int kRate = 48000;
constexpr int kWindow = kRate / 20;               // 50 ms
constexpr size_t kHistory = 15 * 20;              // 15 s of windows
constexpr int kMinWindows = 6;                    // 300 ms
constexpr double kMerge = 5.0, kCooldown = 10.0;  // seconds
constexpr float kFloorDb = -55.0f;                // quieter than this never counts

// Threshold over the baseline, per sensitivity (low / medium / high).
float GameThreshold(int s) { return s <= 0 ? 15.0f : (s == 1 ? 12.0f : 9.0f); }
float MicThreshold(int s) { return s <= 0 ? 18.0f : (s == 1 ? 15.0f : 12.0f); }
} // namespace

void HighlightDetector::Reset()
{
    game_ = Source{};
    mic_ = Source{};
    cluster_.clear();
    ready_.clear();
    lastEmitEnd_ = -1e9;
}

void HighlightDetector::Feed(const float* game, const float* mic, int64_t frames, double t)
{
    if (!cfg_.enabled) return;
    const bool useGame = game && cfg_.sources != 1, useMic = mic && cfg_.sources != 0;
    for (int64_t i = 0; i < frames; ++i) {
        if (useGame) {
            const double l = game[i * 2], r = game[i * 2 + 1];
            game_.acc += (l * l + r * r) * 0.5;
            if (++game_.accN == kWindow) Window(game_, false, t + (double)(i + 1) / kRate);
        }
        if (useMic) {
            const double l = mic[i * 2], r = mic[i * 2 + 1];
            mic_.acc += (l * l + r * r) * 0.5;
            if (++mic_.accN == kWindow) Window(mic_, true, t + (double)(i + 1) / kRate);
        }
    }
    now_ = t + (double)frames / kRate;
    Close(now_, false);
}

void HighlightDetector::Window(Source& s, bool mic, double tEnd)
{
    const float db = (float)(10.0 * std::log10(s.acc / s.accN + 1e-12));
    s.acc = 0.0;
    s.accN = 0;
    // Baseline: the 30th percentile of the last 15 s (needs a couple of seconds of history first).
    float base = kFloorDb;
    if (s.history.size() >= 40) {
        std::vector<float> h = s.history;
        std::nth_element(h.begin(), h.begin() + (ptrdiff_t)(h.size() * 3 / 10), h.end());
        base = std::max(kFloorDb, h[h.size() * 3 / 10]);
    }
    if (s.history.size() < kHistory) s.history.push_back(db);
    else {
        s.history[s.head] = db;
        s.head = (s.head + 1) % kHistory;
    }
    const float th = mic ? MicThreshold(cfg_.sensitivity) : GameThreshold(cfg_.sensitivity);
    const float excess = db - base;
    const bool loud = s.history.size() >= 40 && db > kFloorDb + 10.0f && excess >= th;
    constexpr int kMaxWindows = 6 * 20;   // loud for over 6 s straight: the level changed (music, a new area), not a moment
    if (loud && s.above < 0) return;      // still in such a stretch: wait for it to end (the baseline catches up)
    if (!loud && s.above < 0) {
        s.above = 0;
        return;
    }
    if (loud && s.above >= kMaxWindows) {
        s.above = -1;
        return;
    }
    if (loud) {
        if (s.above == 0) {
            s.spikeStart = tEnd - 0.05;
            s.spikePeak = 0.0f;
        }
        s.above++;
        s.spikePeak = std::max(s.spikePeak, excess);
    } else {
        if (s.above >= kMinWindows && s.spikeStart - lastEmitEnd_ > kCooldown)
            cluster_.push_back({ s.spikeStart, tEnd - 0.05, s.spikePeak, mic });   // a real spike, not a click
        s.above = 0;
    }
}

void HighlightDetector::Close(double now, bool force)
{
    if (cluster_.empty()) return;
    std::sort(cluster_.begin(), cluster_.end(), [](const Spike& a, const Spike& b) { return a.start < b.start; });
    double last = 0.0;
    for (const Spike& sp : cluster_) last = std::max(last, sp.end);
    if (!force && now - last < kMerge) return;   // may still grow

    // The cluster is everything chained within kMerge of each other; anything after a gap waits.
    Highlight h;
    h.start = cluster_.front().start;
    h.end = cluster_.front().end;
    float peak = 0.0f;
    double dur = 0.0;
    int n = 0;
    bool together = false;
    for (const Spike& sp : cluster_) {
        if (sp.start - h.end > kMerge) break;
        h.end = std::max(h.end, sp.end);
        peak = std::max(peak, sp.excess);
        dur += sp.end - sp.start;
        ++n;
        (sp.mic ? h.mic : h.game) = true;
        for (const Spike& o : cluster_)
            if (o.mic != sp.mic && std::fabs(o.start - sp.start) < 1.5) together = true;
    }
    cluster_.erase(cluster_.begin(), cluster_.begin() + n);

    const float th = GameThreshold(cfg_.sensitivity);
    const double jump = std::clamp((peak - th) / 15.0, 0.0, 1.0);
    const double length = std::clamp(dur / 3.0, 0.0, 1.0);
    const double count = std::clamp((n - 1) / 4.0, 0.0, 1.0);
    h.score = (int)std::lround(std::clamp(25.0 + 30.0 * jump + 20.0 * length + 20.0 * count + (together ? 20.0 : 0.0), 0.0, 100.0));
    lastEmitEnd_ = h.end;
    ready_.push_back(h);
}

bool HighlightDetector::Poll(Highlight& out)
{
    if (ready_.empty()) return false;
    out = ready_.front();
    ready_.pop_front();
    return true;
}

} // namespace cs::engine
