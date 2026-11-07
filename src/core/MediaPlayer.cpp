#include "core/MediaPlayer.h"
#include "core/Log.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#ifdef GetCurrentTime
#undef GetCurrentTime   // winbase.h macro clashes with IMFMediaEngine::GetCurrentTime
#endif
#include <mfmediaengine.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

struct Shared {
    std::atomic<bool> metadata{ false };
    std::atomic<bool> canPlay{ false };
    std::atomic<bool> ended{ false };
    std::atomic<bool> formatChanged{ false };
    std::atomic<int> error{ 0 };
    std::atomic<long> errorHr{ 0 };
};

// Media Engine callbacks arrive on Media Foundation worker threads: only touch atomics here.
class EngineNotify : public IMFMediaEngineNotify {
public:
    explicit EngineNotify(std::shared_ptr<Shared> s) : shared_(std::move(s)) {}
    virtual ~EngineNotify() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = (ULONG)InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        switch (event) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: shared_->metadata = true; break;
        case MF_MEDIA_ENGINE_EVENT_CANPLAY: shared_->canPlay = true; break;
        case MF_MEDIA_ENGINE_EVENT_ENDED: shared_->ended = true; break;
        case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE: shared_->formatChanged = true; break;
        case MF_MEDIA_ENGINE_EVENT_ERROR:
            shared_->error = (int)param1;
            shared_->errorHr = (long)param2;
            break;
        default: break;
        }
        return S_OK;
    }

private:
    LONG ref_ = 1;
    std::shared_ptr<Shared> shared_;
};

} // namespace

struct MediaPlayer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    UINT resetToken = 0;
    bool mfStarted = false;
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFMediaEngine> engine;
    ComPtr<IMFMediaEngineEx> engineEx;
    std::shared_ptr<Shared> shared;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int vw = 0, vh = 0;
    bool hasFrame = false;
    bool primed = false;      // asked for a first frame while paused
    std::wstring path;
    float volume = 0.8f;
    bool muted = false;
    double rate = 1.0;

    void CreateTexture(int w, int h)
    {
        tex.Reset();
        srv.Reset();
        hasFrame = false;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex)))
            device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        vw = w;
        vh = h;
    }
};

MediaPlayer::MediaPlayer() : impl_(std::make_unique<Impl>()) {}
MediaPlayer::~MediaPlayer() { Shutdown(); }

bool MediaPlayer::Init(ID3D11Device* device)
{
    Impl& d = *impl_;
    d.device = device;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        Log("MFStartup failed: editor preview disabled");
        return false;
    }
    d.mfStarted = true;
    if (FAILED(MFCreateDXGIDeviceManager(&d.resetToken, &d.dxgiManager)) ||
        FAILED(d.dxgiManager->ResetDevice(device, d.resetToken))) {
        Log("MFCreateDXGIDeviceManager failed: editor preview disabled");
        d.dxgiManager.Reset();
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d.factory)))) {
        Log("Media Engine not available (Windows N edition without the Media Feature Pack?)");
        return false;
    }
    return true;
}

void MediaPlayer::Shutdown()
{
    if (!impl_) return;
    Close();
    impl_->factory.Reset();
    impl_->dxgiManager.Reset();
    impl_->device.Reset();
    if (impl_->mfStarted) {
        MFShutdown();
        impl_->mfStarted = false;
    }
}

bool MediaPlayer::Open(const std::wstring& path)
{
    Impl& d = *impl_;
    Close();
    d.path = path;
    d.shared = std::make_shared<Shared>();
    if (!d.factory || !d.dxgiManager) {
        d.shared->error = 4;
        return false;
    }

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 3);
    EngineNotify* notify = new EngineNotify(d.shared);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    notify->Release();   // the attribute store holds the reference now
    attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, d.dxgiManager.Get());
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);

    HRESULT hr = d.factory->CreateInstance(0, attr.Get(), &d.engine);
    if (FAILED(hr)) {
        Log("IMFMediaEngine creation failed (0x%08lx)", (unsigned long)hr);
        d.shared->error = 4;
        return false;
    }
    d.engine.As(&d.engineEx);
    d.engine->SetAutoPlay(FALSE);
    d.engine->SetLoop(FALSE);
    d.engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    d.engine->SetVolume(d.volume);
    d.engine->SetMuted(d.muted ? TRUE : FALSE);
    d.engine->SetDefaultPlaybackRate(d.rate);

    BSTR url = SysAllocString(path.c_str());
    hr = d.engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) {
        Log("Media Engine SetSource failed (0x%08lx)", (unsigned long)hr);
        d.shared->error = 4;
        return false;
    }
    return true;
}

void MediaPlayer::Close()
{
    Impl& d = *impl_;
    if (d.engine) d.engine->Shutdown();
    d.engineEx.Reset();
    d.engine.Reset();
    d.srv.Reset();
    d.tex.Reset();
    d.vw = d.vh = 0;
    d.hasFrame = false;
    d.primed = false;
    d.shared.reset();
    d.path.clear();
}

void MediaPlayer::Update()
{
    Impl& d = *impl_;
    if (!d.engine || !d.shared) return;
    if (d.shared->formatChanged.exchange(false) || (d.shared->metadata && !d.tex)) {
        DWORD w = 0, h = 0;
        if (SUCCEEDED(d.engine->GetNativeVideoSize(&w, &h)) && w > 0 && h > 0 && ((int)w != d.vw || (int)h != d.vh || !d.tex))
            d.CreateTexture((int)w, (int)h);
    }
    if (!d.tex) return;
    // Show the first frame of a paused clip instead of a blank preview.
    if (!d.primed && d.shared->canPlay && !d.hasFrame) {
        d.primed = true;
        if (d.engine->IsPaused()) d.engine->SetCurrentTime(d.engine->GetCurrentTime());
    }
    LONGLONG pts = 0;
    if (d.engine->OnVideoStreamTick(&pts) == S_OK) {
        MFVideoNormalizedRect src{ 0.0f, 0.0f, 1.0f, 1.0f };
        RECT dst{ 0, 0, d.vw, d.vh };
        MFARGB border{ 0, 0, 0, 255 };
        if (SUCCEEDED(d.engine->TransferVideoFrame(d.tex.Get(), &src, &dst, &border))) d.hasFrame = true;
    }
}

const std::wstring& MediaPlayer::Path() const { return impl_->path; }
bool MediaPlayer::IsOpen() const { return impl_->engine != nullptr; }
bool MediaPlayer::IsReady() const { return impl_->shared && impl_->shared->metadata; }
bool MediaPlayer::HasFrame() const { return impl_->hasFrame; }
bool MediaPlayer::HasError() const { return impl_->shared && impl_->shared->error != 0; }

std::string MediaPlayer::Error() const
{
    if (!impl_->shared) return {};
    switch (impl_->shared->error.load()) {
    case 0: return {};
    case MF_MEDIA_ENGINE_ERR_ABORTED: return "Loading was aborted.";
    case MF_MEDIA_ENGINE_ERR_NETWORK: return "Couldn't read the file.";
    case MF_MEDIA_ENGINE_ERR_DECODE:
    case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
        return "Windows can't play this format. HEVC and AV1 clips need the free codec extensions from the Microsoft Store.";
    case MF_MEDIA_ENGINE_ERR_ENCRYPTED: return "The file is encrypted.";
    default: return "Playback error.";
    }
}

uint64_t MediaPlayer::Texture() const { return impl_->hasFrame ? (uint64_t)(uintptr_t)impl_->srv.Get() : 0; }
int MediaPlayer::VideoWidth() const { return impl_->vw; }
int MediaPlayer::VideoHeight() const { return impl_->vh; }

double MediaPlayer::Duration() const
{
    if (!impl_->engine || !IsReady()) return 0.0;
    double d = impl_->engine->GetDuration();
    return (std::isfinite(d) && d > 0.0) ? d : 0.0;
}

double MediaPlayer::Position() const
{
    if (!impl_->engine) return 0.0;
    double t = impl_->engine->GetCurrentTime();
    return std::isfinite(t) ? t : 0.0;
}

bool MediaPlayer::IsPlaying() const
{
    return impl_->engine && !impl_->engine->IsPaused() && !impl_->engine->IsEnded();
}

bool MediaPlayer::IsEnded() const { return impl_->engine && impl_->engine->IsEnded(); }
bool MediaPlayer::IsSeeking() const { return impl_->engine && impl_->engine->IsSeeking(); }

void MediaPlayer::Play()
{
    if (!impl_->engine) return;
    if (impl_->engine->IsEnded()) impl_->engine->SetCurrentTime(0.0);
    impl_->engine->Play();
}

void MediaPlayer::Pause()
{
    if (impl_->engine) impl_->engine->Pause();
}

void MediaPlayer::TogglePlay()
{
    if (IsPlaying()) Pause();
    else Play();
}

void MediaPlayer::Seek(double seconds, bool approximate)
{
    Impl& d = *impl_;
    if (!d.engine) return;
    if (seconds < 0.0) seconds = 0.0;
    if (d.engineEx)
        d.engineEx->SetCurrentTimeEx(seconds, approximate ? MF_MEDIA_ENGINE_SEEK_MODE_APPROXIMATE : MF_MEDIA_ENGINE_SEEK_MODE_NORMAL);
    else
        d.engine->SetCurrentTime(seconds);
}

void MediaPlayer::StepFrame(bool forward)
{
    Impl& d = *impl_;
    if (!d.engine) return;
    if (!d.engine->IsPaused()) d.engine->Pause();
    if (d.engineEx) d.engineEx->FrameStep(forward ? TRUE : FALSE);
}

void MediaPlayer::SetVolume(float v)
{
    impl_->volume = v;
    if (impl_->engine) impl_->engine->SetVolume((double)v);
}

void MediaPlayer::SetRate(double rate)
{
    impl_->rate = rate;
    if (impl_->engine) impl_->engine->SetPlaybackRate(rate);
}

double MediaPlayer::Rate() const { return impl_->rate; }

void MediaPlayer::SetMuted(bool m)
{
    impl_->muted = m;
    if (impl_->engine) impl_->engine->SetMuted(m ? TRUE : FALSE);
}

} // namespace cs
