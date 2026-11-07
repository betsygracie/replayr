#include "engine/MicTest.h"
#include "engine/AudioInput.h"
#include "core/Log.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

namespace {
constexpr int kRate = AudioInput::Rate;
constexpr double kSeconds = 5.0;

double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Plays interleaved 48 kHz stereo float on the default output. Returns false if the device can't open.
bool Play(const std::vector<float>& pcm, std::atomic<bool>& stop, std::atomic<float>& progress)
{
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf())))
        return false;
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = kRate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = kRate * 8;
    wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    UINT32 bufFrames = 0;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0x80000000 | 0x08000000, 2000000 /* 200 ms */, 0, &wf.Format, nullptr)) ||
        FAILED(client->GetBufferSize(&bufFrames)) || FAILED(client->GetService(IID_PPV_ARGS(&render))))
        return false;
    const size_t total = pcm.size() / 2;
    size_t done = 0;
    client->Start();
    while (!stop) {
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) break;
        if (done >= total && pad == 0) break;
        const UINT32 avail = bufFrames - pad;
        if (avail > 0) {
            BYTE* out = nullptr;
            if (FAILED(render->GetBuffer(avail, &out))) break;
            float* o = (float*)out;
            for (UINT32 i = 0; i < avail; ++i, ++done) {
                o[i * 2] = done < total ? pcm[done * 2] : 0.0f;
                o[i * 2 + 1] = done < total ? pcm[done * 2 + 1] : 0.0f;
            }
            render->ReleaseBuffer(avail, 0);
        }
        progress = (float)std::min(1.0, (double)std::min(done, total) / std::max<size_t>(1, total));
        Sleep(20);
    }
    client->Stop();
    return true;
}
} // namespace

MicTest::~MicTest() { Stop(); }

void MicTest::Start(const std::wstring& deviceId, const MicFx& fx)
{
    Stop();
    error_.clear();
    stop_ = false;
    state_ = MicTestState::Recording;
    progress_ = 0.0f;
    thread_ = std::thread([this, deviceId, fx] { Run(deviceId, fx); });
}

void MicTest::Stop()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (state_ != MicTestState::Failed) state_ = MicTestState::Idle;
}

std::string MicTest::Error() const { return error_; }

void MicTest::Run(std::wstring deviceId, MicFx fx)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto fail = [&](const std::string& msg) {
        error_ = msg;
        state_ = MicTestState::Failed;
    };
    [&] {
        AudioInput in;
        std::string err;
        if (!in.Open(false, deviceId, err)) {
            fail("Couldn't open the microphone: " + err);
            return;
        }
        std::vector<float> raw;
        raw.reserve((size_t)(kSeconds * kRate * 2) + kRate);
        const double t0 = Now();
        while (!stop_) {
            const double t = Now() - t0;
            in.Poll((int64_t)(t * kRate));
            const int64_t n = in.Available();
            if (n > 0) {
                const size_t at = raw.size();
                raw.resize(at + (size_t)n * 2);
                in.Take(raw.data() + at, n, 1.0f, false);
            }
            progress_ = (float)std::min(1.0, t / kSeconds);
            if (t >= kSeconds) break;
            Sleep(20);
        }
        in.Close();
        if (stop_) return;
        std::vector<float> clean = raw;
        MicProcessor proc;
        proc.Configure(fx);
        proc.Process(clean.data(), (int64_t)(clean.size() / 2));
        state_ = MicTestState::PlayingRaw;
        progress_ = 0.0f;
        if (!Play(raw, stop_, progress_)) {
            fail("Couldn't play the test back on your speakers.");
            return;
        }
        if (stop_) return;
        Sleep(400);
        state_ = MicTestState::PlayingClean;
        progress_ = 0.0f;
        Play(clean, stop_, progress_);
        if (!stop_) state_ = MicTestState::Done;
    }();
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace cs::engine
