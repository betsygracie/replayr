#pragma once
#include <cstdint>
#include <memory>
#include <string>

struct ID3D11Device;

namespace cs {

// Video preview for the editor, built on the Windows Media Engine (IMFMediaEngine) in
// frame-server mode: hardware decode on our own D3D11 device, frames copied into a texture
// ImGui can draw, audio played by Windows. Seeking, frame stepping and volume included.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    bool Init(ID3D11Device* device);
    void Shutdown();

    bool Open(const std::wstring& path);
    void Close();
    void Update();                        // UI thread, once per frame

    const std::wstring& Path() const;
    bool IsOpen() const;
    bool IsReady() const;                 // metadata loaded (duration and size known)
    bool HasFrame() const;
    bool HasError() const;
    std::string Error() const;

    uint64_t Texture() const;             // ID3D11ShaderResourceView* (ImTextureID)
    int VideoWidth() const;
    int VideoHeight() const;
    double Duration() const;
    double Position() const;
    bool IsPlaying() const;
    bool IsEnded() const;
    bool IsSeeking() const;

    void Play();
    void Pause();
    void TogglePlay();
    void Seek(double seconds, bool approximate = false);
    void StepFrame(bool forward);
    void SetVolume(float v);
    void SetMuted(bool m);
    void SetRate(double rate);            // 0.25 .. 2.0
    double Rate() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
