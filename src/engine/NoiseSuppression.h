#pragma once
// Microphone clean-up, run on the audio thread at 48 kHz:
//   high-pass (~80 Hz, cuts fan and desk rumble) -> RNNoise (BSD, xiph.org) -> noise gate.
// RNNoise works on 10 ms mono frames, so the processor adds 10 ms of delay; the recorder
// accounts for it by keeping the output length equal to the input length (the first 10 ms
// are silence).
#include <cstdint>
#include <memory>
#include <vector>

namespace cs::engine {

struct MicFx {
    bool suppress = true;
    int strength = 1;            // 0 low, 1 medium, 2 high
    bool gate = false;
    float gateDb = -45.0f;       // opens above this level (dBFS)
    bool highPass = true;
    bool operator==(const MicFx&) const = default;
};

class MicProcessor {
public:
    MicProcessor();
    ~MicProcessor();
    void Configure(const MicFx& fx);
    void Reset();
    // In place, interleaved stereo float (-1..1). Output is mono duplicated to both channels.
    void Process(float* stereo, int64_t frames);
    float LevelDb() const;       // level going into the gate (after suppression), for meters
    bool GateOpen() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace cs::engine
