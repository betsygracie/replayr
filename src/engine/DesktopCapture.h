#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace cs::engine {

// How frames reach the encoder, best first. The session falls back a step when one fails.
enum class FramePath {
    GpuSurfaces,   // GPU BGRA -> NV12 (+ scale) with the D3D11 video processor, handed to MF as DXGI surfaces
    GpuReadback,   // same GPU conversion, NV12 copied to system memory for the encoder
    CpuRgb,        // no video processor: BGRA copied to system memory, MF converts (native size only)
};

const char* FramePathLabel(FramePath p);

struct DesktopCaptureParams {
    int monitorIndex = 0;          // index into platform::EnumerateMonitors()
    int outputHeight = 0;          // 0 = native
    int fps = 60;
    bool cursor = true;
};

// Captures one monitor with DXGI Desktop Duplication on the GPU that drives it, composites the
// mouse cursor, and converts each output frame to NV12 at the output size. Frames are repeated
// when the desktop doesn't change, so the encoder always gets a constant frame rate.
class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();

    bool Open(const DesktopCaptureParams& p, std::string& error);
    void Close();

    // Pulls the newest desktop image if there is one (non-blocking). Returns false if the
    // duplication was lost and couldn't be recreated yet (the last image keeps repeating).
    bool Update();

    // Builds the next output frame. For GpuSurfaces, *texture receives a new NV12 texture
    // (caller owns one reference). For the readback paths, the frame is written to `dst`
    // (NV12: width*height*3/2 bytes, RGB32: width*height*4 bytes, top-down).
    bool ComposeGpu(ID3D11Texture2D** texture);
    bool ComposeCpu(uint8_t* dst, size_t dstSize);

    bool SetPath(FramePath p);     // downgrade (e.g. after an encoder failure)
    FramePath Path() const;
    int OutWidth() const;          // even numbers, aspect preserved
    int OutHeight() const;
    int SourceWidth() const;
    int SourceHeight() const;
    ID3D11Device* Device() const;
    std::string MonitorName() const;
    std::string GpuName() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs::engine
