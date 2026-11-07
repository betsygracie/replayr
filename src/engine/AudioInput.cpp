#include "engine/AudioInput.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

namespace {
const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
constexpr int64_t kLatency = AudioInput::Rate / 8;   // 125 ms: how far a stream may lag before silence is inserted
constexpr int64_t kMaxAhead = AudioInput::Rate / 4;  // 250 ms: more than this ahead is drift, drop it
} // namespace

struct AudioInput::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::string name;
    bool loopback = false;
    int channels = 2, srcRate = 48000, bits = 32;
    bool isFloat = true;

    std::vector<float> fifo;   // interleaved stereo at 48 kHz
    size_t readPos = 0;
    int64_t received = 0;      // frames ever added (incl. padding), minus drops
    int64_t taken = 0;
    bool primed = false;

    // Linear resampler state
    double resamplePos = 0.0;
    float lastL = 0.0f, lastR = 0.0f;

    void Append(const BYTE* data, UINT32 frames, bool silent);
    void PushStereo(float l, float r)
    {
        fifo.push_back(l);
        fifo.push_back(r);
        received++;
    }
    int64_t Buffered() const { return (int64_t)(fifo.size() - readPos) / 2; }
    void Compact()
    {
        if (readPos > 1 << 16 && readPos * 2 > fifo.size()) {
            fifo.erase(fifo.begin(), fifo.begin() + (ptrdiff_t)readPos);
            readPos = 0;
        }
    }
};

AudioInput::AudioInput() : impl_(std::make_unique<Impl>()) {}
AudioInput::~AudioInput() { Close(); }

bool AudioInput::Open(bool loopback, const std::wstring& deviceId, std::string& error)
{
    Close();
    Impl& m = *impl_;
    m.loopback = loopback;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        error = "Audio isn't available.";
        return false;
    }
    ComPtr<IMMDevice> dev;
    HRESULT hr = E_FAIL;
    if (!deviceId.empty()) hr = en->GetDevice(deviceId.c_str(), &dev);
    if (FAILED(hr)) hr = en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, loopback ? eConsole : eCommunications, &dev);
    if (FAILED(hr) || !dev) {
        error = loopback ? "No audio output device found." : "No microphone found.";
        return false;
    }
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR) m.name = ToUtf8(pv.pwszVal);
        PropVariantClear(&pv);
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)m.client.GetAddressOf()))) {
        error = "Couldn't open " + m.name + ".";
        return false;
    }
    WAVEFORMATEX* wf = nullptr;
    if (FAILED(m.client->GetMixFormat(&wf)) || !wf) {
        error = "Couldn't read the format of " + m.name + ".";
        return false;
    }
    m.channels = wf->nChannels;
    m.srcRate = (int)wf->nSamplesPerSec;
    m.bits = wf->wBitsPerSample;
    m.isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)wf;
        m.isFloat = IsEqualGUID(x->SubFormat, kSubtypeFloat);
        if (!m.isFloat && !IsEqualGUID(x->SubFormat, kSubtypePcm)) m.bits = 0;
    }
    const DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = m.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000 /* 200 ms */, 0, wf, nullptr);
    CoTaskMemFree(wf);
    if (FAILED(hr) || (m.bits != 16 && m.bits != 24 && m.bits != 32) || m.channels < 1) {
        error = "Couldn't start audio capture on " + m.name + ".";
        m.client.Reset();
        return false;
    }
    if (FAILED(m.client->GetService(IID_PPV_ARGS(&m.capture))) || FAILED(m.client->Start())) {
        error = "Couldn't start audio capture on " + m.name + ".";
        m.client.Reset();
        m.capture.Reset();
        return false;
    }
    m.fifo.reserve(Rate * 2);
    Log("Audio %s: %s, %d Hz, %d ch, %d-bit%s", loopback ? "loopback" : "mic", m.name.c_str(), m.srcRate, m.channels, m.bits,
        m.isFloat ? " float" : "");
    return true;
}

void AudioInput::Close()
{
    Impl& m = *impl_;
    if (m.client) m.client->Stop();
    m.capture.Reset();
    m.client.Reset();
    m.fifo.clear();
    m.readPos = 0;
    m.received = m.taken = 0;
    m.primed = false;
    m.resamplePos = 0.0;
}

bool AudioInput::IsOpen() const { return impl_->client != nullptr; }
std::string AudioInput::DeviceName() const { return impl_->name; }

void AudioInput::Impl::Append(const BYTE* data, UINT32 frames, bool silent)
{
    // Convert one source frame to stereo float, then resample linearly to 48 kHz.
    auto sampleAt = [&](UINT32 f, int ch) -> float {
        if (silent) return 0.0f;
        const size_t idx = (size_t)f * channels + ch;
        if (isFloat) return ((const float*)data)[idx];
        if (bits == 16) return ((const int16_t*)data)[idx] / 32768.0f;
        if (bits == 24) {
            const BYTE* p = data + idx * 3;
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            return v / 8388608.0f;
        }
        return ((const int32_t*)data)[idx] / 2147483648.0f;
    };
    const double step = (double)srcRate / Rate;
    for (UINT32 f = 0; f < frames; ++f) {
        float l = sampleAt(f, 0);
        float r = channels > 1 ? sampleAt(f, 1) : l;
        if (channels > 2) {   // fold centre into front L/R for 5.1/7.1
            const float c = sampleAt(f, 2) * 0.707f;
            l += c;
            r += c;
        }
        if (srcRate == Rate) {
            PushStereo(l, r);
        } else {
            // Emit every output sample that falls between the previous and this input sample.
            while (resamplePos <= 1.0) {
                const float t = (float)resamplePos;
                PushStereo(lastL + (l - lastL) * t, lastR + (r - lastR) * t);
                resamplePos += step;
            }
            resamplePos -= 1.0;
        }
        lastL = l;
        lastR = r;
    }
}

void AudioInput::Poll(int64_t expectedFrames)
{
    Impl& m = *impl_;
    if (m.capture) {
        for (int guard = 0; guard < 64; ++guard) {
            UINT32 packet = 0;
            if (FAILED(m.capture->GetNextPacketSize(&packet)) || packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(m.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (m.primed) m.Append(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            m.capture->ReleaseBuffer(frames);
        }
        // Whatever was queued before the session started belongs to the past.
        m.primed = true;
    }
    // Keep this stream aligned to the session clock.
    if (m.received < expectedFrames - kLatency) {
        const int64_t pad = expectedFrames - kLatency / 2 - m.received;
        m.fifo.insert(m.fifo.end(), (size_t)pad * 2, 0.0f);
        m.received += pad;
    } else if (m.received > expectedFrames + kMaxAhead) {
        const int64_t drop = std::min<int64_t>(m.received - expectedFrames - kMaxAhead / 2, m.Buffered());
        if (drop > 0) {
            // Remove the newest samples: the device clock runs fast.
            m.fifo.resize(m.fifo.size() - (size_t)drop * 2);
            m.received -= drop;
        }
    }
}

int64_t AudioInput::Available() const { return impl_->Buffered(); }

void AudioInput::Take(float* dst, int64_t frames, float gain, bool add)
{
    Impl& m = *impl_;
    frames = std::min(frames, m.Buffered());
    const float* src = m.fifo.data() + m.readPos;
    const size_t n = (size_t)frames * 2;
    if (add)
        for (size_t i = 0; i < n; ++i) dst[i] += src[i] * gain;
    else
        for (size_t i = 0; i < n; ++i) dst[i] = src[i] * gain;
    m.readPos += n;
    m.taken += frames;
    m.Compact();
}

void AudioInput::Discard(int64_t frames)
{
    Impl& m = *impl_;
    frames = std::min(frames, m.Buffered());
    m.readPos += (size_t)frames * 2;
    m.taken += frames;
    m.Compact();
}

} // namespace cs::engine
