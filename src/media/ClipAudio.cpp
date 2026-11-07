#include "media/ClipAudio.h"
#include "core/Log.h"

#include <windows.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {
constexpr int kRate = 48000;
constexpr double kMaxPcmSeconds = 20.0 * 60.0;   // longer clips: waveforms only, the player's audio plays
double Now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

struct ClipAudio::Impl {
    std::wstring path;
    std::thread loader, renderer;
    std::atomic<bool> cancel { false }, quit { false };
    std::atomic<bool> loading { false }, ready { false }, rendering { false };

    // Results of a load (written by the loader, then read-only).
    mutable std::mutex m;
    int tracks = 0;
    double duration = 0.0;
    std::vector<std::vector<float>> pcm;     // per track, interleaved stereo, empty if too long
    std::vector<std::vector<float>> peaks;   // per track
    std::vector<ClipAudioLane> lanes;

    // Live mix state (UI thread writes, render thread reads).
    std::vector<float> gains;                // per lane
    float master = 1.0f;
    double followPos = 0.0, followWall = 0.0, followRate = 1.0;
    bool followPlaying = false;

    void Stop()
    {
        cancel = true;
        quit = true;
        if (loader.joinable()) loader.join();
        if (renderer.joinable()) renderer.join();
        cancel = false;
        quit = false;
        loading = false;
        ready = false;
        rendering = false;
    }

    void LoadThread(std::wstring file);
    void RenderThread();
};

ClipAudio::ClipAudio() : d_(std::make_unique<Impl>()) {}
ClipAudio::~ClipAudio() { d_->Stop(); }

void ClipAudio::Load(const std::wstring& path)
{
    d_->Stop();
    {
        std::lock_guard<std::mutex> lock(d_->m);
        d_->path = path;
        d_->tracks = 0;
        d_->duration = 0.0;
        d_->pcm.clear();
        d_->peaks.clear();
        d_->lanes.clear();
        d_->followPlaying = false;
    }
    if (path.empty()) return;
    d_->loading = true;
    d_->loader = std::thread([this, path] { d_->LoadThread(path); });
}

void ClipAudio::Clear() { Load(L""); }

void ClipAudio::Impl::LoadThread(std::wstring file)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    [&] {
        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader))) return;
        PROPVARIANT dur;
        PropVariantInit(&dur);
        double length = 0.0;
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur))) length = dur.uhVal.QuadPart / 1e7;
        PropVariantClear(&dur);
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        std::vector<int> trackOf(32, -1);
        int n = 0;
        for (DWORD i = 0; i < 32 && n < 8; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
            GUID major{};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (!IsEqualGUID(major, MFMediaType_Audio)) continue;
            ComPtr<IMFMediaType> out;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
            reader->SetStreamSelection(i, TRUE);
            if (FAILED(reader->SetCurrentMediaType(i, nullptr, out.Get()))) {
                reader->SetStreamSelection(i, FALSE);
                continue;
            }
            trackOf[i] = n++;
        }
        if (n == 0) return;
        const bool keepPcm = length > 0.0 && length <= kMaxPcmSeconds;
        std::vector<std::vector<float>> pcmL((size_t)n), peakL((size_t)n);
        const size_t buckets = (size_t)std::max(1.0, std::ceil(length * kPeaksPerSecond)) + 1;
        for (auto& p : peakL) p.assign(buckets, 0.0f);
        if (keepPcm)
            for (auto& p : pcmL) p.reserve((size_t)(length * kRate * 2) + kRate);
        for (;;) {
            if (cancel) return;
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &s))) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                bool more = false;   // stop once every audio stream ended
                for (DWORD i = 0; i < 32; ++i) {
                    BOOL sel = FALSE;
                    if (trackOf[i] >= 0 && SUCCEEDED(reader->GetStreamSelection(i, &sel)) && sel && i != si) more = true;
                }
                reader->SetStreamSelection(si, FALSE);
                if (!more) break;
                continue;
            }
            if (!s || si >= trackOf.size() || trackOf[si] < 0) continue;
            const int k = trackOf[si];
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(s->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* data = nullptr;
            DWORD bytes = 0;
            if (FAILED(buf->Lock(&data, nullptr, &bytes))) continue;
            const short* p = (const short*)data;
            const int64_t frames = bytes / 4;
            const int64_t at = std::max<int64_t>(0, (int64_t)std::llround(ts / 1e7 * kRate));
            if (keepPcm) {
                std::vector<float>& dst = pcmL[(size_t)k];
                const size_t need = (size_t)(at + frames) * 2;
                if (dst.size() < need) dst.resize(need, 0.0f);
                for (int64_t i = 0; i < frames * 2; ++i) dst[(size_t)(at * 2 + i)] = p[i] / 32768.0f;
            }
            std::vector<float>& pk = peakL[(size_t)k];
            for (int64_t i = 0; i < frames; ++i) {
                const size_t b = (size_t)((at + i) * kPeaksPerSecond / kRate);
                if (b >= pk.size()) pk.resize(b + 1, 0.0f);
                const float v = std::max(std::fabs(p[i * 2] / 32768.0f), std::fabs(p[i * 2 + 1] / 32768.0f));
                if (v > pk[b]) pk[b] = v;
            }
            buf->Unlock();
        }
        std::vector<ClipAudioLane> lanes;
        if (n >= 3) {
            lanes.push_back({ "Game", 1 });
            lanes.push_back({ "Mic", 2 });
            for (int i = 3; i < n; ++i) lanes.push_back({ "Track " + std::to_string(i + 1), i });
        } else if (n == 2) {
            lanes.push_back({ "Game", 0 });
            lanes.push_back({ "Mic", 1 });
        } else {
            lanes.push_back({ "Audio", 0 });
        }
        std::lock_guard<std::mutex> lock(m);
        tracks = n;
        duration = length;
        pcm = std::move(pcmL);
        peaks = std::move(peakL);
        this->lanes = std::move(lanes);
        gains.assign(this->lanes.size(), 1.0f);
        ready = true;
    }();
    loading = false;
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

const std::wstring& ClipAudio::Path() const { return d_->path; }
bool ClipAudio::Loading() const { return d_->loading; }
bool ClipAudio::Ready() const { return d_->ready; }
int ClipAudio::TrackCount() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->tracks;
}
std::vector<ClipAudioLane> ClipAudio::Lanes() const
{
    if (!d_->ready) return {};
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->lanes;
}
double ClipAudio::Duration() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->duration;
}
std::vector<float> ClipAudio::Peaks(int track) const
{
    if (!d_->ready) return {};
    std::lock_guard<std::mutex> lock(d_->m);
    return track >= 0 && track < (int)d_->peaks.size() ? d_->peaks[(size_t)track] : std::vector<float>();
}

void ClipAudio::SetGains(const std::vector<float>& laneGains, float master)
{
    std::lock_guard<std::mutex> lock(d_->m);
    d_->gains = laneGains;
    d_->master = master;
}

void ClipAudio::Follow(double position, bool playing, double rate)
{
    Impl& m = *d_;
    if (!m.ready) return;
    {
        std::lock_guard<std::mutex> lock(m.m);
        if (m.pcm.empty() || m.pcm[0].empty()) return;   // too long to keep: the player plays its own audio
        m.followPos = position;
        m.followWall = Now();
        m.followPlaying = playing;
        m.followRate = rate;
    }
    if (!m.renderer.joinable()) {
        m.rendering = true;   // optimistic: the player mutes right away, no doubled audio on start
        m.renderer = std::thread([this] { d_->RenderThread(); });
    }
}

bool ClipAudio::Playing() const { return d_->rendering; }

void ClipAudio::Impl::RenderThread()
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
              SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
              SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf()));
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
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /* AUTOCONVERTPCM */ | 0x08000000 /* SRC_DEFAULT_QUALITY */;
    UINT32 bufFrames = 0;
    ok = ok && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 400000 /* 40 ms */, 0, &wf.Format, nullptr)) &&
         SUCCEEDED(client->SetEventHandle(ev)) && SUCCEEDED(client->GetBufferSize(&bufFrames)) &&
         SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))) && SUCCEEDED(client->Start());
    if (!ok) {
        Log("Editor audio: couldn't open the output device; using the player's audio");
        rendering = false;
    }
    double cursor = 0.0;
    std::vector<float> g;
    while (ok && !quit) {
        if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) break;
        const UINT32 avail = bufFrames - pad;
        if (!avail) continue;
        BYTE* out = nullptr;
        if (FAILED(render->GetBuffer(avail, &out))) break;
        float* o = (float*)out;
        std::lock_guard<std::mutex> lock(m);
        if (!followPlaying) {
            memset(o, 0, (size_t)avail * 8);
            cursor = followPos;
        } else {
            const double rate = std::clamp(followRate, 0.1, 8.0);
            const double expected = followPos + (Now() - followWall) * rate;
            if (std::fabs(cursor - expected) > 0.08) cursor = expected;   // seek, or drift past 80 ms
            const double step = rate / kRate;
            for (UINT32 f = 0; f < avail; ++f, cursor += step) {
                const double x = cursor * kRate;
                const int64_t i0 = (int64_t)std::floor(x);
                const float fr = (float)(x - (double)i0);
                float l = 0.0f, r = 0.0f;
                for (size_t li = 0; li < lanes.size() && li < gains.size(); ++li) {
                    const float gain = gains[li];
                    if (gain <= 0.0f) continue;
                    const std::vector<float>& p = pcm[(size_t)lanes[li].track];
                    const size_t a = (size_t)std::max<int64_t>(0, i0) * 2, b = a + 2;
                    if (i0 < 0 || b + 1 >= p.size()) continue;
                    l += (p[a] * (1.0f - fr) + p[b] * fr) * gain;
                    r += (p[a + 1] * (1.0f - fr) + p[b + 1] * fr) * gain;
                }
                o[f * 2] = std::clamp(l * master, -1.0f, 1.0f);
                o[f * 2 + 1] = std::clamp(r * master, -1.0f, 1.0f);
            }
        }
        render->ReleaseBuffer(avail, 0);
    }
    if (client) client->Stop();
    CloseHandle(ev);
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace cs
