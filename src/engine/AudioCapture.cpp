#include "engine/AudioCapture.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Util.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

#if defined(__MINGW32__)
// mingw-w64's endpointvolume.h only forward-declares IAudioMeterInformation (the Windows SDK
// defines it). Needed only for MinGW builds; MSVC uses the SDK declaration.
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* pfPeak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* pnChannelCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 u32ChannelCount, float* afPeakValues) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* pdwHardwareSupportMask) = 0;
};
__CRT_UUID_DECL(IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64)
#endif

namespace cs::engine {

// PKEY_Device_FriendlyName, defined here so no extra GUID library is needed.
static const PROPERTYKEY kDeviceFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

namespace {
struct SessionMeter {
    std::string name;
    ComPtr<IAudioMeterInformation> meter;
};

std::string PrettyProcessName(DWORD pid)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring exe;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) exe.assign(buf, n);
    CloseHandle(p);
    if (exe.empty()) return {};
    std::string stem = ToUtf8(FileStem(exe));
    std::string low = ToLowerAscii(stem);
    static const std::map<std::string, std::string> known = {
        { "chrome", "Chrome" }, { "msedge", "Edge" }, { "firefox", "Firefox" }, { "opera", "Opera" },
        { "brave", "Brave" }, { "discord", "Discord" }, { "spotify", "Spotify" }, { "steam", "Steam" },
        { "steamwebhelper", "Steam" }, { "vlc", "VLC" }, { "teams", "Teams" }, { "ms-teams", "Teams" },
        { "zoom", "Zoom" }, { "obs64", "OBS" }, { "javaw", "Java (Minecraft)" },
    };
    auto it = known.find(low);
    if (it != known.end()) return it->second;
    if (!stem.empty() && stem[0] >= 'a' && stem[0] <= 'z') stem[0] = (char)(stem[0] - 'a' + 'A');
    return stem;
}
} // namespace

struct AudioCapture::Impl {
    mutable std::mutex mutex;
    AudioLevels levels;
    std::vector<AudioDevice> outputs, inputs;
    std::wstring outputId, inputId;
    bool micEnabled = true;
    bool ptt = false;
    uint32_t pttKey = 0;
    std::atomic<bool> reopen{ true };
    std::atomic<bool> quit{ false };
    std::thread thread;

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioMeterInformation> outMeter;
    ComPtr<IAudioSessionManager2> sessions;
    ComPtr<IAudioClient> micClient;
    ComPtr<IAudioCaptureClient> micCapture;
    bool micFloat = true;
    int micBits = 32;
    int micChannels = 2;
    std::wstring openedDefaultOut, openedDefaultIn;
    std::vector<SessionMeter> sessionMeters;

    void Run();
    void Open();
    void CloseStreams();
    void RefreshDevices();
    void RefreshSessions();
    float DrainMic();
    ComPtr<IMMDevice> Device(EDataFlow flow, const std::wstring& id);
    std::wstring DefaultId(EDataFlow flow);
};

ComPtr<IMMDevice> AudioCapture::Impl::Device(EDataFlow flow, const std::wstring& id)
{
    ComPtr<IMMDevice> dev;
    if (!id.empty() && SUCCEEDED(enumerator->GetDevice(id.c_str(), &dev))) return dev;
    dev.Reset();
    enumerator->GetDefaultAudioEndpoint(flow, flow == eRender ? eConsole : eCommunications, &dev);
    if (!dev) enumerator->GetDefaultAudioEndpoint(flow, eConsole, &dev);
    return dev;
}

std::wstring AudioCapture::Impl::DefaultId(EDataFlow flow)
{
    ComPtr<IMMDevice> dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, flow == eRender ? eConsole : eCommunications, &dev)))
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &dev))) return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(dev->GetId(&id)) && id) out = id;
    if (id) CoTaskMemFree(id);
    return out;
}

void AudioCapture::Impl::CloseStreams()
{
    if (micClient) micClient->Stop();
    micCapture.Reset();
    micClient.Reset();
    outMeter.Reset();
    sessions.Reset();
    sessionMeters.clear();
}

void AudioCapture::Impl::Open()
{
    CloseStreams();
    std::wstring outId, inId;
    bool mic;
    {
        std::lock_guard<std::mutex> lock(mutex);
        outId = outputId;
        inId = inputId;
        mic = micEnabled;
    }
    openedDefaultOut = DefaultId(eRender);
    openedDefaultIn = DefaultId(eCapture);

    if (ComPtr<IMMDevice> out = Device(eRender, outId)) {
        out->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)outMeter.GetAddressOf());
        out->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)sessions.GetAddressOf());
    }
    if (mic) {
        if (ComPtr<IMMDevice> in = Device(eCapture, inId)) {
            if (SUCCEEDED(in->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)micClient.GetAddressOf()))) {
                WAVEFORMATEX* wfx = nullptr;
                if (SUCCEEDED(micClient->GetMixFormat(&wfx)) && wfx) {
                    micChannels = std::max<int>(1, wfx->nChannels);
                    micBits = wfx->wBitsPerSample;
                    micFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                               (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
                    HRESULT hr = micClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /*100 ms*/, 0, wfx, nullptr);
                    if (SUCCEEDED(hr)) hr = micClient->GetService(__uuidof(IAudioCaptureClient), (void**)micCapture.GetAddressOf());
                    if (SUCCEEDED(hr)) hr = micClient->Start();
                    if (FAILED(hr)) {
                        Log("Microphone stream failed (0x%08lx)", (unsigned long)hr);
                        micCapture.Reset();
                        micClient.Reset();
                    }
                    CoTaskMemFree(wfx);
                } else {
                    micClient.Reset();
                }
            }
        }
    }
    RefreshSessions();
}

void AudioCapture::Impl::RefreshDevices()
{
    auto list = [&](EDataFlow flow) {
        std::vector<AudioDevice> out;
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return out;
        std::wstring def = DefaultId(flow);
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(coll->Item(i, &dev))) continue;
            AudioDevice d;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) d.id = id;
            if (id) CoTaskMemFree(id);
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kDeviceFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) d.name = ToUtf8(v.pwszVal);
                PropVariantClear(&v);
            }
            if (d.name.empty()) d.name = "Audio device";
            d.isDefault = (d.id == def);
            out.push_back(d);
        }
        return out;
    };
    auto outs = list(eRender);
    auto ins = list(eCapture);
    std::lock_guard<std::mutex> lock(mutex);
    outputs = std::move(outs);
    inputs = std::move(ins);
}

void AudioCapture::Impl::RefreshSessions()
{
    sessionMeters.clear();
    if (!sessions) return;
    ComPtr<IAudioSessionEnumerator> en;
    if (FAILED(sessions->GetSessionEnumerator(&en))) return;
    int count = 0;
    en->GetCount(&count);
    const DWORD self = GetCurrentProcessId();
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> ctrl;
        if (FAILED(en->GetSession(i, &ctrl))) continue;
        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(ctrl.As(&ctrl2))) continue;
        if (ctrl2->IsSystemSoundsSession() == S_OK) continue;
        AudioSessionState state = AudioSessionStateInactive;
        ctrl->GetState(&state);
        if (state != AudioSessionStateActive) continue;
        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid == 0 || pid == self) continue;
        SessionMeter sm;
        sm.name = PrettyProcessName(pid);
        if (sm.name.empty()) continue;
        if (FAILED(ctrl.As(&sm.meter))) continue;
        sessionMeters.push_back(std::move(sm));
    }
}

float AudioCapture::Impl::DrainMic()
{
    if (!micCapture) return 0.0f;
    float peak = 0.0f;
    UINT32 packet = 0;
    for (int guard = 0; guard < 64; ++guard) {
        HRESULT hr = micCapture->GetNextPacketSize(&packet);
        if (FAILED(hr)) {
            reopen = true;   // device unplugged or format changed
            return 0.0f;
        }
        if (packet == 0) break;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(micCapture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
            const size_t samples = (size_t)frames * micChannels;
            if (micFloat && micBits == 32) {
                const float* s = reinterpret_cast<const float*>(data);
                for (size_t k = 0; k < samples; ++k) peak = std::max(peak, std::fabs(s[k]));
            } else if (micBits == 16) {
                const int16_t* s = reinterpret_cast<const int16_t*>(data);
                for (size_t k = 0; k < samples; ++k) peak = std::max(peak, std::fabs(s[k] / 32768.0f));
            } else if (micBits == 32) {
                const int32_t* s = reinterpret_cast<const int32_t*>(data);
                for (size_t k = 0; k < samples; ++k) peak = std::max(peak, std::fabs((float)s[k] / 2147483648.0f));
            }
        }
        micCapture->ReleaseBuffer(frames);
    }
    return std::min(peak, 1.0f);
}

void AudioCapture::Impl::Run()
{
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        Log("No audio device enumerator: audio meters disabled");
        if (SUCCEEDED(coHr)) CoUninitialize();
        return;
    }
    double lastDevices = -1e9, lastSessions = 0.0;
    while (!quit) {
        const double now = NowSeconds();
        if (now - lastDevices > 3.0) {
            lastDevices = now;
            RefreshDevices();
            // Follow the Windows default device when the user switches it.
            std::wstring outId, inId;
            {
                std::lock_guard<std::mutex> lock(mutex);
                outId = outputId;
                inId = inputId;
            }
            if ((outId.empty() && DefaultId(eRender) != openedDefaultOut) || (inId.empty() && DefaultId(eCapture) != openedDefaultIn))
                reopen = true;
        }
        if (reopen.exchange(false)) {
            Open();
            lastSessions = now;
        }
        if (now - lastSessions > 1.0) {
            lastSessions = now;
            RefreshSessions();
        }

        AudioLevels lv;
        float p = 0.0f;
        if (outMeter && SUCCEEDED(outMeter->GetPeakValue(&p))) {
            lv.system = p;
            lv.systemOk = true;
        }
        lv.mic = DrainMic();
        lv.micOk = micCapture != nullptr;
        bool pttOn;
        uint32_t key;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pttOn = ptt;
            key = pttKey;
        }
        lv.micGated = pttOn && !platform::IsKeyDown(key);
        for (auto& s : sessionMeters) {
            float sp = 0.0f;
            if (s.meter && SUCCEEDED(s.meter->GetPeakValue(&sp))) {
                bool merged = false;
                for (auto& a : lv.apps)
                    if (a.name == s.name) {
                        a.peak = std::max(a.peak, sp);
                        merged = true;
                    }
                if (!merged) lv.apps.push_back(AppAudio{ s.name, sp });
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            levels = std::move(lv);
        }
        Sleep(15);
    }
    CloseStreams();
    enumerator.Reset();
    if (SUCCEEDED(coHr)) CoUninitialize();
}

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { Stop(); }

void AudioCapture::Start()
{
    impl_->quit = false;
    impl_->reopen = true;
    impl_->thread = std::thread([this] { impl_->Run(); });
}

void AudioCapture::Stop()
{
    if (!impl_ || !impl_->thread.joinable()) return;
    impl_->quit = true;
    impl_->thread.join();
}

void AudioCapture::SetDevices(const std::wstring& outputId, const std::wstring& inputId)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->outputId == outputId && impl_->inputId == inputId) return;
    impl_->outputId = outputId;
    impl_->inputId = inputId;
    impl_->reopen = true;
}

void AudioCapture::SetMicEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->micEnabled == enabled) return;
    impl_->micEnabled = enabled;
    impl_->reopen = true;
}

void AudioCapture::SetPushToTalk(bool enabled, uint32_t vk)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->ptt = enabled;
    impl_->pttKey = vk;
}

AudioLevels AudioCapture::Levels() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->levels;
}

std::vector<AudioDevice> AudioCapture::OutputDevices() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->outputs;
}

std::vector<AudioDevice> AudioCapture::InputDevices() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->inputs;
}

bool AudioCapture::PerAppCaptureSupported()
{
    return platform::WindowsBuildNumber() >= 19041;
}

} // namespace cs::engine
