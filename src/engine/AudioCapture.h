#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cs::engine {

struct AudioDevice {
    std::wstring id;
    std::string name;
    bool isDefault = false;
};

struct AppAudio {
    std::string name;         // "Discord", "Chrome"...
    float peak = 0.0f;        // 0..1
};

struct AudioLevels {
    float system = 0.0f;      // peak of the output device (what you hear)
    float mic = 0.0f;         // peak of the microphone
    bool micGated = false;    // push-to-talk is on and the key is up
    bool systemOk = false;
    bool micOk = false;
    std::vector<AppAudio> apps;   // apps currently playing sound
};

// Live WASAPI meters for the audio panel:
// - system output: the endpoint's own peak meter (no capture stream needed)
// - microphone: a lightweight shared-mode capture stream, drained on this thread
// - per-app: active audio sessions on the output device, each with its own peak meter
// The real engine will reuse this thread for loopback/mic capture into the encoder.
class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    void Start();
    void Stop();
    void SetDevices(const std::wstring& outputId, const std::wstring& inputId);   // "" = default device
    void SetMicEnabled(bool enabled);
    void SetPushToTalk(bool enabled, uint32_t vk);

    AudioLevels Levels() const;
    std::vector<AudioDevice> OutputDevices() const;
    std::vector<AudioDevice> InputDevices() const;
    static bool PerAppCaptureSupported();   // process loopback: Windows 10 2004 (build 19041)+

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs::engine
