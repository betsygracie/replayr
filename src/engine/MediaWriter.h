#pragma once
#include "engine/ClipEngine.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace cs::engine {

void MediaStartup();    // MFStartup, ref-counted
void MediaShutdown();

struct WriterParams {
    std::wstring path;
    int width = 1920, height = 1080, fps = 60;
    int bitrateKbps = 40000;
    Codec codec = Codec::H264;
    bool hardware = true;            // allow GPU encoders
    int inputKind = 0;               // 0 NV12 DXGI surfaces, 1 NV12 memory, 2 RGB32 memory
    ID3D11Device* device = nullptr;  // for inputKind 0
    int audioTracks = 0;             // 48 kHz stereo AAC tracks
};

// One MP4 file written with the Media Foundation sink writer: H.264/HEVC video (hardware encoder
// when there is one) plus AAC audio tracks. Times are in seconds from the start of this file.
class MediaWriter {
public:
    MediaWriter();
    ~MediaWriter();
    bool Open(const WriterParams& p, std::string& error);
    bool WriteVideoSurface(ID3D11Texture2D* nv12, double time, double duration);
    bool WriteVideoMemory(const uint8_t* data, size_t size, double time, double duration);
    bool WriteAudio(int track, const float* stereo, int frames, double time);
    bool Finalize();
    std::string EncoderName() const;   // "NVIDIA H.264 Encoder MFT" or similar
    bool Hardware() const;
    Codec UsedCodec() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Joins MP4 files that share the same streams into one, without re-encoding. Times are in
// seconds relative to the start of the first input. Output starts at the last video keyframe at
// or before `start` and stops after `end` (end <= 0: to the end).
struct RemuxInput {
    std::wstring path;
    double offset = 0.0;    // where this file starts on the shared timeline
};
bool RemuxMp4(const std::vector<RemuxInput>& inputs, const std::wstring& output, double start, double end,
              const std::function<void(float)>& progress, const std::atomic<bool>* cancel, double* outDuration, std::string& error);

double Mp4Duration(const std::wstring& path);

} // namespace cs::engine
