#include "engine/NoiseSuppression.h"

extern "C" {
#include "rnnoise.h"
}

#include <algorithm>
#include <cmath>

namespace cs::engine {

namespace {
constexpr int kRate = 48000, kFrame = 480;   // RNNoise: 10 ms at 48 kHz

// 2nd-order Butterworth high-pass (RBJ cookbook).
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    void HighPass(float hz)
    {
        const float w = 2.0f * 3.14159265f * hz / kRate, c = std::cos(w), al = std::sin(w) / (2.0f * 0.7071f), a0 = 1.0f + al;
        b0 = (1.0f + c) / 2.0f / a0;
        b1 = -(1.0f + c) / a0;
        b2 = (1.0f + c) / 2.0f / a0;
        a1 = -2.0f * c / a0;
        a2 = (1.0f - al) / a0;
    }
    float Run(float x)
    {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};
} // namespace

struct MicProcessor::Impl {
    MicFx fx;
    DenoiseState* rnn = nullptr;
    Biquad hp;
    std::vector<float> in, out;   // mono FIFOs: waiting for a full frame / processed
    float frame[kFrame];
    float gateGain = 0.0f, level = -100.0f;
    double holdLeft = 0.0;
    bool open = false;

    void RunFrame()
    {
        // Dry copy for the strength blend: low keeps some of the original under the cleaned voice.
        float dry[kFrame];
        for (int i = 0; i < kFrame; ++i) dry[i] = frame[i];
        if (fx.suppress) {
            float scaled[kFrame];
            for (int i = 0; i < kFrame; ++i) scaled[i] = frame[i] * 32768.0f;   // RNNoise works in 16-bit range
            rnnoise_process_frame(rnn, scaled, scaled);
            const float keep = fx.strength == 0 ? 0.30f : (fx.strength == 1 ? 0.10f : 0.0f);
            for (int i = 0; i < kFrame; ++i) frame[i] = scaled[i] / 32768.0f + dry[i] * keep;
        }
        // Level of this frame (RMS, dBFS).
        double e = 0.0;
        for (int i = 0; i < kFrame; ++i) e += (double)frame[i] * frame[i];
        level = (float)(10.0 * std::log10(e / kFrame + 1e-12));
        // Gate with hold and smooth attack/release so words aren't chopped.
        float target = 1.0f;
        if (fx.gate) {
            if (level > fx.gateDb) {
                open = true;
                holdLeft = 0.15;
            } else if (holdLeft > 0.0) {
                holdLeft -= (double)kFrame / kRate;
            } else {
                open = false;
            }
            target = open ? 1.0f : 0.0f;
        } else {
            open = true;
        }
        const float step = target > gateGain ? 1.0f / (0.005f * kRate) : 1.0f / (0.12f * kRate);   // 5 ms attack, 120 ms release
        for (int i = 0; i < kFrame; ++i) {
            gateGain = target > gateGain ? std::min(target, gateGain + step) : std::max(target, gateGain - step);
            frame[i] *= gateGain;
        }
        out.insert(out.end(), frame, frame + kFrame);
    }
};

MicProcessor::MicProcessor() : d_(std::make_unique<Impl>())
{
    d_->rnn = rnnoise_create(nullptr);
    d_->hp.HighPass(80.0f);
    d_->out.assign(kFrame, 0.0f);   // 10 ms of silence: output stays the same length as the input
}

MicProcessor::~MicProcessor()
{
    if (d_->rnn) rnnoise_destroy(d_->rnn);
}

void MicProcessor::Configure(const MicFx& fx) { d_->fx = fx; }

void MicProcessor::Reset()
{
    if (d_->rnn) rnnoise_destroy(d_->rnn);
    d_->rnn = rnnoise_create(nullptr);
    d_->hp = Biquad{};
    d_->hp.HighPass(80.0f);
    d_->in.clear();
    d_->out.assign(kFrame, 0.0f);
    d_->gateGain = 0.0f;
}

void MicProcessor::Process(float* stereo, int64_t frames)
{
    Impl& m = *d_;
    for (int64_t i = 0; i < frames; ++i) {
        float x = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        if (m.fx.highPass) x = m.hp.Run(x);
        m.in.push_back(x);
        if ((int)m.in.size() == kFrame) {
            std::copy(m.in.begin(), m.in.end(), m.frame);
            m.in.clear();
            m.RunFrame();
        }
    }
    const int64_t n = std::min<int64_t>(frames, (int64_t)m.out.size());
    for (int64_t i = 0; i < n; ++i) stereo[i * 2] = stereo[i * 2 + 1] = m.out[(size_t)i];
    for (int64_t i = n; i < frames; ++i) stereo[i * 2] = stereo[i * 2 + 1] = 0.0f;   // never happens once primed
    m.out.erase(m.out.begin(), m.out.begin() + (ptrdiff_t)n);
}

float MicProcessor::LevelDb() const { return d_->level; }
bool MicProcessor::GateOpen() const { return d_->open; }

} // namespace cs::engine
