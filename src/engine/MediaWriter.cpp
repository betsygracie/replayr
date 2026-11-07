#include "engine/MediaWriter.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

namespace {

// Defined here so linking never depends on which GUIDs a given SDK's import libraries carry.
const GUID kCodecApiIid = { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
const GUID kGopSize = { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
const GUID kBFrames = { 0x8d390aac, 0xdc5c, 0x4200, { 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2 } };
const GUID kMtVideoEncoder = { 0xf79eac7d, 0xe545, 0x4387, { 0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a } };
const GUID kSubtypeHEVC = { 0x43564548, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };   // 'HEVC'

// Minimal ICodecAPI vtable (only SetValue is used), so no strmif.h/icodecapi.h mismatch can bite.
struct ICodecApiLite : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) = 0;
};

std::mutex g_mfMutex;
int g_mfRefs = 0;

LONGLONG Hns(double seconds) { return (LONGLONG)std::llround(seconds * 1e7); }

bool SetVideoBasics(IMFMediaType* t, const GUID& subtype, int w, int h, int fps)
{
    return SUCCEEDED(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) && SUCCEEDED(t->SetGUID(MF_MT_SUBTYPE, subtype)) &&
           SUCCEEDED(t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)) &&
           SUCCEEDED(MFSetAttributeSize(t, MF_MT_FRAME_SIZE, w, h)) && SUCCEEDED(MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1)) &&
           SUCCEEDED(MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
}

bool SetAudioBasics(IMFMediaType* t, const GUID& subtype)
{
    return SUCCEEDED(t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) && SUCCEEDED(t->SetGUID(MF_MT_SUBTYPE, subtype)) &&
           SUCCEEDED(t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) && SUCCEEDED(t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)) &&
           SUCCEEDED(t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
}

std::string HrText(HRESULT hr)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
    return buf;
}

} // namespace

void MediaStartup()
{
    std::lock_guard<std::mutex> lock(g_mfMutex);
    if (g_mfRefs++ == 0) MFStartup(MF_VERSION, MFSTARTUP_FULL);
}

void MediaShutdown()
{
    std::lock_guard<std::mutex> lock(g_mfMutex);
    if (g_mfRefs > 0 && --g_mfRefs == 0) MFShutdown();
}

struct MediaWriter::Impl {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFDXGIDeviceManager> manager;
    DWORD video = 0;
    DWORD audio[4] = {};
    int audioTracks = 0;
    WriterParams params;
    Codec codec = Codec::H264;
    std::string encoderName;
    bool hardware = false;
    std::vector<int16_t> pcm;
    bool finalized = false;

    bool TryOpen(const WriterParams& p, Codec c, std::string& error);
    void DetectEncoder();
};

MediaWriter::MediaWriter() : impl_(std::make_unique<Impl>()) {}

MediaWriter::~MediaWriter()
{
    // An unfinalized writer is abandoned (the file is incomplete and gets deleted by the owner).
    impl_->writer.Reset();
    impl_->manager.Reset();
}

bool MediaWriter::Impl::TryOpen(const WriterParams& p, Codec c, std::string& error)
{
    writer.Reset();
    manager.Reset();
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 4);
    attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, p.hardware ? TRUE : FALSE);
    if (p.inputKind == 0) {
        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &manager)) || FAILED(manager->ResetDevice(p.device, token))) {
            error = "Media Foundation couldn't use the GPU device.";
            return false;
        }
        attr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.Get());
    }
    HRESULT hr = MFCreateSinkWriterFromURL(p.path.c_str(), nullptr, attr.Get(), &writer);
    if (FAILED(hr)) {
        error = "Couldn't create the video file (" + HrText(hr) + ").";
        return false;
    }

    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    const GUID sub = c == Codec::HEVC ? kSubtypeHEVC : MFVideoFormat_H264;
    SetVideoBasics(out.Get(), sub, p.width, p.height, p.fps);
    out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::max(1000, p.bitrateKbps) * 1000u);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, c == Codec::HEVC ? 1 /* Main */ : 100 /* High */);
    hr = writer->AddStream(out.Get(), &video);
    if (FAILED(hr)) {
        error = "The video encoder rejected the output format (" + HrText(hr) + ").";
        return false;
    }
    MFCreateMediaType(&in);
    const GUID inSub = p.inputKind == 2 ? MFVideoFormat_RGB32 : MFVideoFormat_NV12;
    SetVideoBasics(in.Get(), inSub, p.width, p.height, p.fps);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(p.inputKind == 2 ? p.width * 4 : p.width));
    hr = writer->SetInputMediaType(video, in.Get(), nullptr);
    if (FAILED(hr)) {
        error = "No video encoder accepts this input (" + HrText(hr) + ").";
        return false;
    }

    // One keyframe per second and no B-frames: clips can be cut on any second, and segments join cleanly.
    ComPtr<IUnknown> unk;
    if (SUCCEEDED(writer->GetServiceForStream(video, GUID_NULL, kCodecApiIid, (void**)unk.GetAddressOf())) && unk) {
        ICodecApiLite* api = (ICodecApiLite*)unk.Get();
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = (ULONG)p.fps;
        api->SetValue(&kGopSize, &v);
        v.ulVal = 0;
        api->SetValue(&kBFrames, &v);
    }

    audioTracks = std::min(p.audioTracks, 4);
    for (int i = 0; i < audioTracks; ++i) {
        ComPtr<IMFMediaType> aout, ain;
        MFCreateMediaType(&aout);
        SetAudioBasics(aout.Get(), MFAudioFormat_AAC);
        aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   // 192 kbit/s
        aout->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        hr = writer->AddStream(aout.Get(), &audio[i]);
        if (FAILED(hr)) {
            error = "The AAC audio encoder isn't available (" + HrText(hr) + ").";
            return false;
        }
        MFCreateMediaType(&ain);
        SetAudioBasics(ain.Get(), MFAudioFormat_PCM);
        ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        hr = writer->SetInputMediaType(audio[i], ain.Get(), nullptr);
        if (FAILED(hr)) {
            error = "The AAC audio encoder rejected the input (" + HrText(hr) + ").";
            return false;
        }
    }
    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        error = "The encoder failed to start (" + HrText(hr) + ").";
        return false;
    }
    codec = c;
    return true;
}

void MediaWriter::Impl::DetectEncoder()
{
    hardware = false;
    encoderName = codec == Codec::HEVC ? "HEVC" : "H.264";
    ComPtr<IMFSinkWriterEx> ex;
    if (FAILED(writer.As(&ex))) return;
    for (DWORD i = 0; i < 8; ++i) {
        GUID cat{};
        ComPtr<IMFTransform> mft;
        if (FAILED(ex->GetTransformForStream(video, i, &cat, &mft))) break;
        if (!IsEqualGUID(cat, kMtVideoEncoder)) continue;
        ComPtr<IMFAttributes> a;
        if (SUCCEEDED(mft->GetAttributes(&a)) && a) {
            UINT32 len = 0;
            hardware = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len));
            wchar_t name[256] = {};
            if (SUCCEEDED(a->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, nullptr)) && name[0]) encoderName = ToUtf8(name);
        }
        if (!hardware && encoderName.find("Microsoft") == std::string::npos)
            encoderName = std::string("Microsoft ") + (codec == Codec::HEVC ? "HEVC" : "H.264") + " (CPU)";
        break;
    }
}

bool MediaWriter::Open(const WriterParams& p, std::string& error)
{
    Impl& m = *impl_;
    m.params = p;
    Codec c = p.codec == Codec::AV1 ? Codec::H264 : p.codec;   // MF has no AV1 encoder on most PCs
    if (!m.TryOpen(p, c, error)) {
        if (c != Codec::HEVC) return false;
        Log("HEVC writer failed (%s), using H.264", error.c_str());
        DeleteFileW(p.path.c_str());
        if (!m.TryOpen(p, Codec::H264, error)) return false;
    }
    m.DetectEncoder();
    return true;
}

bool MediaWriter::WriteVideoSurface(ID3D11Texture2D* nv12, double time, double duration)
{
    Impl& m = *impl_;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buf))) return false;
    ComPtr<IMF2DBuffer> b2;
    DWORD len = 0;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
    ComPtr<IMFSample> s;
    if (FAILED(MFCreateSample(&s))) return false;
    s->AddBuffer(buf.Get());
    s->SetSampleTime(Hns(time));
    s->SetSampleDuration(Hns(duration));
    return SUCCEEDED(m.writer->WriteSample(m.video, s.Get()));
}

bool MediaWriter::WriteVideoMemory(const uint8_t* data, size_t size, double time, double duration)
{
    Impl& m = *impl_;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer((DWORD)size, &buf))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, data, size);
    buf->Unlock();
    buf->SetCurrentLength((DWORD)size);
    ComPtr<IMFSample> s;
    if (FAILED(MFCreateSample(&s))) return false;
    s->AddBuffer(buf.Get());
    s->SetSampleTime(Hns(time));
    s->SetSampleDuration(Hns(duration));
    return SUCCEEDED(m.writer->WriteSample(m.video, s.Get()));
}

bool MediaWriter::WriteAudio(int track, const float* stereo, int frames, double time)
{
    Impl& m = *impl_;
    if (track < 0 || track >= m.audioTracks || frames <= 0) return false;
    m.pcm.resize((size_t)frames * 2);
    for (size_t i = 0; i < m.pcm.size(); ++i) {
        float v = stereo[i];
        v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        m.pcm[i] = (int16_t)std::lrint(v * 32767.0f);
    }
    const DWORD bytes = (DWORD)(m.pcm.size() * sizeof(int16_t));
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
    memcpy(dst, m.pcm.data(), bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    if (FAILED(MFCreateSample(&s))) return false;
    s->AddBuffer(buf.Get());
    s->SetSampleTime(Hns(time));
    s->SetSampleDuration(Hns(frames / 48000.0));
    return SUCCEEDED(m.writer->WriteSample(m.audio[track], s.Get()));
}

bool MediaWriter::Finalize()
{
    Impl& m = *impl_;
    if (!m.writer || m.finalized) return m.finalized;
    HRESULT hr = m.writer->Finalize();
    m.finalized = SUCCEEDED(hr);
    if (!m.finalized) Log("Sink writer finalize failed: %s", HrText(hr).c_str());
    m.writer.Reset();
    m.manager.Reset();
    return m.finalized;
}

std::string MediaWriter::EncoderName() const { return impl_->encoderName; }
bool MediaWriter::Hardware() const { return impl_->hardware; }
Codec MediaWriter::UsedCodec() const { return impl_->codec; }

// ---------------------------------------------------------------------------------------------
// Remux

namespace {

bool OpenReader(const std::wstring& path, ComPtr<IMFSourceReader>& r)
{
    r.Reset();
    return SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r)) && r;
}

double ReaderDuration(IMFSourceReader* r)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    double d = 0.0;
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
        d = pv.uhVal.QuadPart / 1e7;
    PropVariantClear(&pv);
    return d;
}

struct StreamInfo {
    GUID major{};
    DWORD writerIndex = 0;
};

} // namespace

double Mp4Duration(const std::wstring& path)
{
    ComPtr<IMFSourceReader> r;
    if (!OpenReader(path, r)) return 0.0;
    return ReaderDuration(r.Get());
}

bool RemuxMp4(const std::vector<RemuxInput>& inputs, const std::wstring& output, double start, double end,
              const std::function<void(float)>& progress, const std::atomic<bool>* cancel, double* outDuration, std::string& error)
{
    if (inputs.empty()) {
        error = "Nothing to save.";
        return false;
    }
    // The input that contains `start`.
    size_t first = 0;
    for (size_t i = 0; i < inputs.size(); ++i)
        if (inputs[i].offset <= start + 1e-6) first = i;

    ComPtr<IMFSourceReader> reader;
    if (!OpenReader(inputs[first].path, reader)) {
        error = "Couldn't open a buffer segment.";
        return false;
    }
    std::vector<StreamInfo> streams;
    int videoStream = -1;
    for (DWORD i = 0; i < 16; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
        StreamInfo si;
        t->GetGUID(MF_MT_MAJOR_TYPE, &si.major);
        if (videoStream < 0 && IsEqualGUID(si.major, MFMediaType_Video)) videoStream = (int)i;
        streams.push_back(si);
    }
    if (streams.empty() || videoStream < 0) {
        error = "The buffer segment has no video.";
        return false;
    }

    // Find the last keyframe at or before `start` in that input.
    double clipStart = inputs[first].offset;
    {
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD)videoStream, TRUE);
        for (;;) {
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader->ReadSample((DWORD)videoStream, 0, &si, &flags, &ts, &s))) break;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!s) continue;
            const double t = inputs[first].offset + ts / 1e7;
            if (t > start + 1e-4) break;
            UINT32 key = 0;
            if (SUCCEEDED(s->GetUINT32(MFSampleExtension_CleanPoint, &key)) && key) clipStart = t;
        }
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, pv);
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, TRUE);
    }

    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr)) {
        error = "Couldn't create the clip file (" + HrText(hr) + ").";
        return false;
    }
    for (size_t i = 0; i < streams.size(); ++i) {
        ComPtr<IMFMediaType> t;
        reader->GetNativeMediaType((DWORD)i, 0, &t);
        if (FAILED(writer->AddStream(t.Get(), &streams[i].writerIndex)) ||
            FAILED(writer->SetInputMediaType(streams[i].writerIndex, t.Get(), nullptr))) {
            error = "Couldn't copy the streams into the clip.";
            return false;
        }
    }
    if (FAILED(writer->BeginWriting())) {
        error = "Couldn't start writing the clip.";
        return false;
    }

    double maxEnd = 0.0;
    bool stop = false;
    const double total = std::max(0.001, (end > 0 ? end : inputs.back().offset + 1.0) - clipStart);
    for (size_t f = first; f < inputs.size() && !stop; ++f) {
        if (f != first && !OpenReader(inputs[f].path, reader)) {
            Log("Remux: skipping unreadable %s", ToUtf8(inputs[f].path).c_str());
            continue;
        }
        if (f != first) reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, TRUE);
        std::vector<bool> ended(streams.size(), false);
        size_t endedCount = 0;
        while (endedCount < streams.size()) {
            if (cancel && cancel->load()) {
                writer.Reset();
                DeleteFileW(output.c_str());
                error = "Cancelled.";
                return false;
            }
            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &s))) break;
            if (si >= streams.size()) continue;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (!ended[si]) {
                    ended[si] = true;
                    endedCount++;
                }
                continue;
            }
            if (!s || ended[si]) continue;
            const double abs = inputs[f].offset + ts / 1e7;
            if (end > 0 && abs > end) {
                ended[si] = true;
                endedCount++;
                if ((int)si == videoStream) stop = true;
                continue;
            }
            const double t = abs - clipStart;
            if (t < -1e-4) continue;
            LONGLONG dur = 0;
            s->GetSampleDuration(&dur);
            s->SetSampleTime((LONGLONG)std::llround(std::max(0.0, t) * 1e7));
            if (f != first || t > 0.0) s->SetUINT32(MFSampleExtension_Discontinuity, FALSE);
            if (FAILED(writer->WriteSample(streams[si].writerIndex, s.Get()))) {
                error = "Writing the clip failed.";
                writer.Reset();
                DeleteFileW(output.c_str());
                return false;
            }
            maxEnd = std::max(maxEnd, t + dur / 1e7);
            if (progress && (int)si == videoStream) progress((float)std::min(1.0, t / total));
        }
    }
    hr = writer->Finalize();
    if (FAILED(hr)) {
        error = "Finishing the clip failed (" + HrText(hr) + ").";
        DeleteFileW(output.c_str());
        return false;
    }
    if (outDuration) *outDuration = maxEnd;
    if (progress) progress(1.0f);
    return true;
}

} // namespace cs::engine
