#pragma once
// "Test mic" in Settings > Audio: records 5 seconds from the microphone, then plays it back twice,
// first as recorded and then with the clean-up (noise suppression / gate / high-pass) applied,
// so you can hear the difference. Runs on its own thread; the UI polls the state.
#include "engine/NoiseSuppression.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace cs::engine {

enum class MicTestState { Idle, Recording, PlayingRaw, PlayingClean, Done, Failed };

class MicTest {
public:
    ~MicTest();
    void Start(const std::wstring& deviceId, const MicFx& fx);
    void Stop();
    MicTestState State() const { return state_.load(); }
    float Progress() const { return progress_.load(); }   // 0..1 within the current state
    std::string Error() const;

private:
    void Run(std::wstring deviceId, MicFx fx);
    std::thread thread_;
    std::atomic<MicTestState> state_ { MicTestState::Idle };
    std::atomic<float> progress_ { 0.0f };
    std::atomic<bool> stop_ { false };
    std::string error_;
};

} // namespace cs::engine
