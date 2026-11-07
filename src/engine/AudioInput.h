#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cs::engine {

// One WASAPI shared-mode stream: loopback of an output device ("what you hear") or a microphone.
// Polled from the session thread; delivers 48 kHz stereo float, whatever the device format is.
// Silence is filled in when loopback delivers nothing (Windows sends no packets while nothing
// plays), and drift against the session clock is corrected, so tracks stay in sync with video.
class AudioInput {
public:
    static constexpr int Rate = 48000;

    AudioInput();
    ~AudioInput();
    bool Open(bool loopback, const std::wstring& deviceId, std::string& error);   // "" = default device
    void Close();
    bool IsOpen() const;

    // Reads everything the device has, then pads/trims so `Available()` lines up with
    // `expectedFrames` (frames since the session started) within a small latency window.
    void Poll(int64_t expectedFrames);
    int64_t Available() const;                       // frames ready to take
    void Take(float* dst, int64_t frames, float gain, bool add);   // interleaved stereo; add = mix into dst
    void Discard(int64_t frames);
    std::string DeviceName() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs::engine
