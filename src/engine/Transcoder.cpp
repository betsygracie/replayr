#include "engine/Transcoder.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

namespace {

const GUID kCodecApiIid = { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };
const GUID kGopSize = { 0x95f31b26, 0x95a4, 0x41aa, { 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1 } };
const GUID kBFrames = { 0x8d390aac, 0xdc5c, 0x4200, { 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2 } };

struct ICodecApiLite : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) = 0;
};


bool Fail(std::string& error, const char* what, HRESULT hr)
{
    error = std::string(what) + ": " + DescribeFailure(hr);
    Log("Transcode: %s (0x%08lx)", what, (unsigned long)hr);
    return false;
}

LONGLONG Hns(double s) { return (LONGLONG)std::llround(s * 1e7); }

} // namespace

std::string DescribeFailure(long hr)
{
    switch ((unsigned long)hr) {
    case 0x80070070: case 0x80070027: return "the disk is full";
    case 0x80070005: return "Windows didn't allow writing to that folder";
    case 0x80070020: return "the file is open in another app";
    case 0x80070002: case 0x80070003: return "the file or folder doesn't exist any more";
    case 0xC00D36B4: case 0xC00D36B9: case 0xC00D5212: return "this video's format isn't supported by Windows' encoder";
    case 0xC00D36C4: return "the file isn't a video Windows can read";
    default: break;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "the video encoder failed (0x%08lx)", (unsigned long)hr);
    return buf;
}

double KeyframeAtOrBefore(const std::wstring& path, double t)
{
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return -1.0;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = Hns(std::max(0.0, t));
    if (FAILED(r->SetCurrentPosition(GUID_NULL, pv))) return -1.0;
    for (int i = 0; i < 8; ++i) {
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &ts, &s))) return -1.0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return -1.0;
        if (!s) continue;
        UINT32 key = 0;
        s->GetUINT32(MFSampleExtension_CleanPoint, &key);
        return key ? ts / 1e7 : -1.0;
    }
    return -1.0;
}

namespace {
// 0..1 brightness multiplier for the fades at the start / end of the export.
float FadeAt(double t, double total, const Look& l)
{
    constexpr double kFade = 0.5;
    double f = 1.0;
    if (l.fadeIn) f = std::min(f, std::max(0.0, t / kFade));
    if (l.fadeOut) f = std::min(f, std::max(0.0, (total - t) / kFade));
    return (float)std::clamp(f, 0.0, 1.0);
}

// Colour filter + fade, in place on BGRA pixels.
void ApplyLook(uint8_t* px, UINT32 w, UINT32 h, const Look& l, float fade)
{
    if (l.filter == 0 && fade >= 0.999f) return;
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = px + i * 4;
        float b = p[0], g = p[1], r = p[2];
        switch (l.filter) {
        case 1: {   // vivid: more saturation and a little contrast
            const float y = 0.299f * r + 0.587f * g + 0.114f * b;
            r = y + (r - y) * 1.35f; g = y + (g - y) * 1.35f; b = y + (b - y) * 1.35f;
            r = (r - 128) * 1.08f + 128; g = (g - 128) * 1.08f + 128; b = (b - 128) * 1.08f + 128;
            break;
        }
        case 2: r = r * 1.08f + 10; g = g * 1.02f + 4; b = b * 0.88f; break;    // warm
        case 3: r = r * 0.90f; g = g * 1.0f + 2; b = b * 1.10f + 10; break;     // cool
        case 4: { const float y = 0.299f * r + 0.587f * g + 0.114f * b; r = g = b = (y - 128) * 1.1f + 128; break; }   // B&W
        default: break;
        }
        p[0] = (uint8_t)std::clamp(b * fade, 0.0f, 255.0f);
        p[1] = (uint8_t)std::clamp(g * fade, 0.0f, 255.0f);
        p[2] = (uint8_t)std::clamp(r * fade, 0.0f, 255.0f);
    }
}
} // namespace

double OutputLength(const std::vector<Segment>& segments)
{
    double total = 0.0;
    for (const Segment& s : segments) total += std::max(0.0, s.end - s.start) / std::clamp(s.speed, 0.1, 8.0);
    return total;
}

namespace {

struct SourceInfo {
    UINT32 w = 0, h = 0, fpsN = 60, fpsD = 1, bitrate = 0;
    int audioTracks = 0;
};

SourceInfo Probe(const std::wstring& path)
{
    SourceInfo info;
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return info;
    bool video = false;
    for (DWORD i = 0; i < 16; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
        GUID major{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (IsEqualGUID(major, MFMediaType_Video) && !video) {
            video = true;
            MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &info.w, &info.h);
            MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &info.fpsN, &info.fpsD);
            t->GetUINT32(MF_MT_AVG_BITRATE, &info.bitrate);
        } else if (IsEqualGUID(major, MFMediaType_Audio)) {
            ++info.audioTracks;
        }
    }
    if (!info.fpsN || !info.fpsD) info.fpsN = 60, info.fpsD = 1;
    if (info.bitrate == 0) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        double dur = 0.0;
        if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            dur = pv.uhVal.QuadPart / 1e7;
        PropVariantClear(&pv);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (dur > 0 && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            info.bitrate = (UINT32)std::min(4.0e8, (((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) * 8.0 / dur * 0.92);
    }
    return info;
}

// Bilinear BGRA scale, used only if Media Foundation's video processor won't scale a source.
void ScaleBgra(const uint8_t* src, int sw, int sh, int spitch, uint8_t* dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * sh / dh - 0.5f);
        const int y0 = std::min((int)fy, sh - 1), y1 = std::min(y0 + 1, sh - 1);
        const float wy = fy - y0;
        for (int x = 0; x < dw; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * sw / dw - 0.5f);
            const int x0 = std::min((int)fx, sw - 1), x1 = std::min(x0 + 1, sw - 1);
            const float wx = fx - x0;
            const uint8_t *a = src + (size_t)y0 * spitch + x0 * 4, *b = src + (size_t)y0 * spitch + x1 * 4;
            const uint8_t *c = src + (size_t)y1 * spitch + x0 * 4, *d = src + (size_t)y1 * spitch + x1 * 4;
            uint8_t* o = dst + ((size_t)y * dw + x) * 4;
            for (int k = 0; k < 4; ++k)
                o[k] = (uint8_t)std::lround((a[k] * (1 - wx) + b[k] * wx) * (1 - wy) + (c[k] * (1 - wx) + d[k] * wx) * wy);
        }
    }
}

struct TrackOut {
    DWORD writerIndex = 0;
    int64_t written = 0;            // output samples (per channel) written so far
};

constexpr UINT32 kRate = 48000, kChannels = 2;

} // namespace

bool Transcode(const TranscodeRequest& req, const std::function<void(float)>& progress, const std::atomic<bool>* cancel, std::string& error)
{
    const double total = OutputLength(req.segments);
    if (req.segments.empty() || total <= 0.05) {
        error = "The selection is empty.";
        return false;
    }
    const SourceInfo first = Probe(req.segments[0].source);
    if (!first.w || !first.h) {
        error = "Couldn't export: the clip has no video.";
        return false;
    }
    const double srcFps = (double)first.fpsN / first.fpsD;
    int outTracks = 0;
    for (const Segment& sg : req.segments) outTracks = std::max(outTracks, std::min(2, Probe(sg.source).audioTracks));

    // Output size, frame rate and bitrate.
    UINT32 W = first.w & ~1u, H = first.h & ~1u;
    double fps = srcFps;
    UINT32 fpsN = first.fpsN, fpsD = first.fpsD;
    const double pixelsPerSec = (double)W * H * fps;
    double bitrate;
    switch (req.quality) {
    case ExportQuality::Small: bitrate = std::clamp(pixelsPerSec * 0.035, 2.0e6, 12.0e6); break;
    case ExportQuality::High: bitrate = std::clamp(first.bitrate > 0 ? first.bitrate * 0.65 : pixelsPerSec * 0.1, 6.0e6, 60.0e6); break;
    default: bitrate = std::clamp(first.bitrate > 0 ? (double)first.bitrate : pixelsPerSec * 0.15, 8.0e6, 150.0e6); break;
    }
    if (req.targetMB > 0.0) {
        // Fit under the size: what's left after audio goes to video; drop resolution / frame rate
        // until there are enough bits per pixel to look decent.
        const double budget = req.targetMB * 1e6 * 8.0 * 0.93;
        const double video = std::max(250e3, budget / total - 192e3 * outTracks);
        static const UINT32 heights[] = { 2160, 1440, 1080, 720, 540, 480, 360 };
        UINT32 bestH = H;
        bool fpsHalved = false;
        bool found = false;
        for (int pass = 0; pass < 2 && !found; ++pass) {
            const double f = pass == 0 ? srcFps : std::min(srcFps, 30.0);
            for (UINT32 hh : heights) {
                if (hh > H) continue;
                const double ww = (double)hh * W / H;
                if (video / (ww * hh * f) >= 0.05) {
                    bestH = hh;
                    fpsHalved = pass == 1 && srcFps > 31.0;
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            bestH = std::min<UINT32>(H, 360);
            fpsHalved = srcFps > 31.0;
        }
        if (bestH < H) {
            W = (UINT32)std::lround((double)bestH * W / H / 2.0) * 2;
            H = bestH & ~1u;
        }
        if (fpsHalved) {
            fps = 30.0;
            fpsN = 30;
            fpsD = 1;
        }
        bitrate = std::min(bitrate, video);
    }
    const double frameDur = 1.0 / fps;

    // Aspect crop: decode at W x H, encode the cropped OW x OH window.
    const Look& look = req.look;
    UINT32 OW = W, OH = H;
    if (look.aspect == 1) OW = std::min(W, (UINT32)std::lround(H * 9.0 / 16.0 / 2.0) * 2);
    else if (look.aspect == 2) OW = std::min(W, H);
    if (look.aspect == 1 && OW == W) OH = std::min(H, (UINT32)std::lround(W * 16.0 / 9.0 / 2.0) * 2);   // already narrow
    const UINT32 cropX = (UINT32)std::lround((W - OW) * std::clamp(look.cropX, 0.0f, 1.0f)) & ~1u;
    const UINT32 cropY = (H - OH) / 2 & ~1u;
    if (OW != W) bitrate *= std::max(0.4, (double)OW * OH / ((double)W * H));

    // Writer.
    DeleteFileW(req.destination.c_str());
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 2);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, req.hardware ? TRUE : FALSE);
    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(req.destination.c_str(), nullptr, wa.Get(), &writer);
    if (FAILED(hr)) return Fail(error, "Couldn't create the file", hr);
    DWORD videoOut = 0;
    {
        ComPtr<IMFMediaType> o, i;
        MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        o->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)bitrate);
        o->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        o->SetUINT32(MF_MT_MPEG2_PROFILE, 100);
        MFSetAttributeSize(o.Get(), MF_MT_FRAME_SIZE, OW, OH);
        MFSetAttributeRatio(o.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        MFSetAttributeRatio(o.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(o.Get(), &videoOut);
        if (FAILED(hr)) return Fail(error, "Couldn't start the encoder", hr);
        MFCreateMediaType(&i);
        i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        i->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        i->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        i->SetUINT32(MF_MT_DEFAULT_STRIDE, OW * 4);
        MFSetAttributeSize(i.Get(), MF_MT_FRAME_SIZE, OW, OH);
        MFSetAttributeRatio(i.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        MFSetAttributeRatio(i.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(videoOut, i.Get(), nullptr);
        if (FAILED(hr)) return Fail(error, "Couldn't start the encoder", hr);
        ComPtr<IUnknown> unk;
        if (SUCCEEDED(writer->GetServiceForStream(videoOut, GUID_NULL, kCodecApiIid, (void**)unk.GetAddressOf())) && unk) {
            ICodecApiLite* api = (ICodecApiLite*)unk.Get();
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = (ULONG)std::lround(fps);
            api->SetValue(&kGopSize, &v);
            v.ulVal = 0;
            api->SetValue(&kBFrames, &v);
        }
    }
    std::vector<TrackOut> tracks((size_t)outTracks);
    for (TrackOut& t : tracks) {
        ComPtr<IMFMediaType> o, i;
        MFCreateMediaType(&o);
        o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        o->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        o->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        o->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        o->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        o->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        hr = writer->AddStream(o.Get(), &t.writerIndex);
        if (FAILED(hr)) return Fail(error, "Couldn't start the audio encoder", hr);
        MFCreateMediaType(&i);
        i->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        i->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        i->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        i->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        i->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        i->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
        i->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
        hr = writer->SetInputMediaType(t.writerIndex, i.Get(), nullptr);
        if (FAILED(hr)) return Fail(error, "Couldn't start the audio encoder", hr);
    }
    hr = writer->BeginWriting();
    if (FAILED(hr)) return Fail(error, "Couldn't start writing", hr);

    // Overlays, rendered once at the output size.
    std::vector<uint8_t> banner;
    BannerLayout layout;
    if (req.banner) {
        layout = BannerLayoutFor((int)OW, (int)OH, req.bannerStyle.corner);
        if (!RenderBanner(layout, req.bannerStyle, banner)) Log("Transcode: banner couldn't be rendered; exporting without it");
    }
    struct CaptionImage {
        const Caption* c;
        std::vector<uint8_t> px;
        int w = 0, h = 0, x = 0, y = 0;
    };
    std::vector<CaptionImage> captions;
    for (const Caption& c : req.captions) {
        CaptionImage ci;
        ci.c = &c;
        if (!RenderCaption(c.text, c.size, (int)OW, (int)OH, ci.px, ci.w, ci.h)) continue;
        ci.x = std::clamp((int)std::lround(c.x * OW - ci.w * 0.5), 0, std::max(0, (int)OW - ci.w));
        ci.y = std::clamp((int)std::lround(c.y * OH - ci.h * 0.5), 0, std::max(0, (int)OH - ci.h));
        captions.push_back(std::move(ci));
    }

    auto abandon = [&]() {
        writer.Reset();
        DeleteFileW(req.destination.c_str());
    };
    std::vector<uint8_t> frame((size_t)W * H * 4), work((size_t)OW * OH * 4);
    bool haveFrame = false;
    double nextGrid = 0.0;   // output time of the next video frame
    int frames = 0;

    auto emitFrame = [&](double t) -> HRESULT {
        for (UINT32 y = 0; y < OH; ++y)
            memcpy(work.data() + (size_t)y * OW * 4, frame.data() + ((size_t)(y + cropY) * W + cropX) * 4, (size_t)OW * 4);
        ApplyLook(work.data(), OW, OH, look, FadeAt(t, total, look));
        if (!banner.empty()) CompositeBanner(work.data(), (int)OW * 4, (int)OW, (int)OH, banner, layout, BannerAt(t, layout, req.bannerStyle.mode), req.bannerStyle.corner);
        for (const CaptionImage& ci : captions) {
            if (t < ci.c->start || t > ci.c->end) continue;
            const float a = (float)std::min({ 1.0, (t - ci.c->start) / 0.15, (ci.c->end - t) / 0.15 });
            CompositeImage(work.data(), (int)OW * 4, (int)OW, (int)OH, ci.px, ci.w, ci.h, ci.x, ci.y, a);
        }
        ComPtr<IMFMediaBuffer> ob;
        HRESULT r = MFCreateMemoryBuffer((DWORD)work.size(), &ob);
        if (FAILED(r)) return r;
        BYTE* dst = nullptr;
        ob->Lock(&dst, nullptr, nullptr);
        memcpy(dst, work.data(), work.size());
        ob->Unlock();
        ob->SetCurrentLength((DWORD)work.size());
        ComPtr<IMFSample> os;
        MFCreateSample(&os);
        os->AddBuffer(ob.Get());
        os->SetSampleTime(Hns(t));
        os->SetSampleDuration(Hns(frameDur));
        ++frames;
        return writer->WriteSample(videoOut, os.Get());
    };
    auto writeAudio = [&](TrackOut& t, const short* data, int64_t count) -> HRESULT {
        if (count <= 0) return S_OK;
        const DWORD bytes = (DWORD)(count * kChannels * 2);
        ComPtr<IMFMediaBuffer> ob;
        HRESULT r = MFCreateMemoryBuffer(bytes, &ob);
        if (FAILED(r)) return r;
        BYTE* dst = nullptr;
        ob->Lock(&dst, nullptr, nullptr);
        if (data) {
            short* o = (short*)dst;
            const bool plain = look.volume == 1.0f && !look.fadeIn && !look.fadeOut;
            if (plain) memcpy(dst, data, bytes);
            else
                for (int64_t i = 0; i < count; ++i) {
                    const float g = look.volume * FadeAt((double)(t.written + i) / kRate, total, look);
                    for (UINT32 c = 0; c < kChannels; ++c)
                        o[i * kChannels + c] = (short)std::clamp((int)std::lround(data[i * kChannels + c] * g), -32768, 32767);
                }
        } else memset(dst, 0, bytes);
        ob->Unlock();
        ob->SetCurrentLength(bytes);
        ComPtr<IMFSample> os;
        MFCreateSample(&os);
        os->AddBuffer(ob.Get());
        os->SetSampleTime(Hns((double)t.written / kRate));
        os->SetSampleDuration(Hns((double)count / kRate));
        t.written += count;
        return writer->WriteSample(t.writerIndex, os.Get());
    };
    // Pads with silence or drops so the next write starts exactly at `sample`.
    auto alignAudio = [&](TrackOut& t, int64_t sample, int64_t& skip) -> HRESULT {
        skip = 0;
        if (sample > t.written) {
            std::vector<short> zero;   // silence in chunks of 0.5 s
            while (t.written < sample) {
                const HRESULT r = writeAudio(t, nullptr, std::min<int64_t>(sample - t.written, kRate / 2));
                if (FAILED(r)) return r;
            }
        } else if (sample < t.written) {
            skip = t.written - sample;
        }
        return S_OK;
    };

    double outBase = 0.0;
    std::vector<short> resampled;
    for (const Segment& seg : req.segments) {
        const double speed = std::clamp(seg.speed, 0.1, 8.0);
        const double segLen = std::max(0.0, seg.end - seg.start), outLen = segLen / speed;
        if (segLen <= 0.01) continue;

        ComPtr<IMFAttributes> ra;
        MFCreateAttributes(&ra, 2);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(seg.source.c_str(), ra.Get(), &reader);
        if (FAILED(hr)) {
            abandon();
            return Fail(error, "Couldn't open a clip", hr);
        }
        int videoIndex = -1;
        std::vector<DWORD> audioIdx;
        for (DWORD i = 0; i < 16; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(reader->GetNativeMediaType(i, 0, &t))) break;
            GUID major{};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (IsEqualGUID(major, MFMediaType_Video) && videoIndex < 0) videoIndex = (int)i;
            else if (IsEqualGUID(major, MFMediaType_Audio)) audioIdx.push_back(i);
        }
        if (videoIndex < 0) {
            abandon();
            error = "Couldn't export: a clip has no video.";
            return false;
        }
        reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD)videoIndex, TRUE);
        UINT32 sw = W, sh = H;
        {
            ComPtr<IMFMediaType> out;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, W, H);   // the video processor scales to the output size
            hr = reader->SetCurrentMediaType((DWORD)videoIndex, nullptr, out.Get());
            if (FAILED(hr)) {
                ComPtr<IMFMediaType> plain;
                MFCreateMediaType(&plain);
                plain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                plain->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                hr = reader->SetCurrentMediaType((DWORD)videoIndex, nullptr, plain.Get());
            }
            if (FAILED(hr)) {
                abandon();
                return Fail(error, "Couldn't decode a clip", hr);
            }
            ComPtr<IMFMediaType> cur;
            reader->GetCurrentMediaType((DWORD)videoIndex, &cur);
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &sw, &sh);
        }
        // Track i of the output takes this clip's i-th audio stream, converted to 48 kHz stereo.
        std::vector<int> trackOf(16, -1);
        std::vector<bool> trackDone((size_t)outTracks, true);
        for (int k = 0; k < outTracks && k < (int)audioIdx.size(); ++k) {
            ComPtr<IMFMediaType> out;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
            out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
            reader->SetStreamSelection(audioIdx[(size_t)k], TRUE);
            if (FAILED(reader->SetCurrentMediaType(audioIdx[(size_t)k], nullptr, out.Get()))) {
                Log("Transcode: an audio track couldn't be converted; it will be silent in this section");
                reader->SetStreamSelection(audioIdx[(size_t)k], FALSE);
                continue;
            }
            trackOf[audioIdx[(size_t)k]] = k;
            trackDone[(size_t)k] = false;
        }

        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = Hns(seg.start);
        reader->SetCurrentPosition(GUID_NULL, pos);
        const double segEndOut = outBase + outLen;
        bool videoDone = false;
        for (;;) {
            if (cancel && cancel->load()) {
                abandon();
                error = "Export cancelled.";
                return false;
            }
            bool audioDone = true;
            for (bool d : trackDone) audioDone = audioDone && d;
            if (videoDone && audioDone) break;

            DWORD si = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            hr = reader->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &s);
            if (FAILED(hr)) {
                abandon();
                return Fail(error, "Couldn't decode a clip", hr);
            }
            const int track = si < trackOf.size() ? trackOf[si] : -1;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if ((int)si == videoIndex) videoDone = true;
                else if (track >= 0) trackDone[(size_t)track] = true;
                if (flags & MF_SOURCE_READERF_ERROR) break;
                continue;
            }
            if (!s) continue;
            const double rel = ts / 1e7 - seg.start;

            if ((int)si == videoIndex) {
                if (videoDone) continue;
                LONGLONG sd = 0;
                const double srcDur = SUCCEEDED(s->GetSampleDuration(&sd)) && sd > 0 ? sd / 1e7 : 1.0 / srcFps;
                if (rel + srcDur <= 0.0) continue;   // before the in-point
                if (rel >= segLen - 1e-4) {            // past the out-point
                    videoDone = true;
                    reader->SetStreamSelection((DWORD)videoIndex, FALSE);
                    continue;
                }
                ComPtr<IMFMediaBuffer> buf;
                if (FAILED(s->ConvertToContiguousBuffer(&buf))) continue;
                ComPtr<IMF2DBuffer> b2;
                BYTE* data = nullptr;
                LONG pitch = (LONG)sw * 4;
                bool locked2d = false;
                if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) locked2d = true;
                else if (FAILED(buf->Lock(&data, nullptr, nullptr))) continue;
                if (sw == W && sh == H) {
                    for (UINT32 y = 0; y < H; ++y) memcpy(&frame[(size_t)y * W * 4], data + (LONG_PTR)y * pitch, (size_t)W * 4);
                } else {
                    ScaleBgra(data, (int)sw, (int)sh, (int)pitch, frame.data(), (int)W, (int)H);
                }
                if (locked2d) b2->Unlock2D();
                else buf->Unlock();
                haveFrame = true;
                // This source frame covers [oEnd - its length, oEnd) of the output: put it on every
                // output frame-grid slot in there (slow motion repeats frames, fast skips them).
                const double oEnd = std::min(segEndOut, outBase + (rel + srcDur) / speed);
                while (nextGrid < oEnd - 1e-6) {
                    hr = emitFrame(nextGrid);
                    if (FAILED(hr)) {
                        abandon();
                        return Fail(error, "Couldn't export", hr);
                    }
                    nextGrid += frameDur;
                }
                if (progress) progress((float)std::min(1.0, nextGrid / total));
            } else if (track >= 0 && !trackDone[(size_t)track]) {
                TrackOut& tr = tracks[(size_t)track];
                ComPtr<IMFMediaBuffer> buf;
                if (FAILED(s->ConvertToContiguousBuffer(&buf))) continue;
                BYTE* data = nullptr;
                DWORD bytes = 0;
                if (FAILED(buf->Lock(&data, nullptr, &bytes))) continue;
                const short* pcm = (const short*)data;
                const int64_t n = bytes / (kChannels * 2);
                int64_t firstS = 0;
                if (rel < 0.0) firstS = std::min<int64_t>(n, (int64_t)std::llround(-rel * kRate));
                const double startRel = rel + (double)firstS / kRate;
                const int64_t remain = (int64_t)std::llround((segLen - startRel) * kRate);
                const int64_t count = std::min<int64_t>(n - firstS, std::max<int64_t>(0, remain));
                HRESULT wr = S_OK;
                if (count > 0) {
                    // Resample by the speed (pitch follows, like a tape).
                    const int64_t outCount = speed == 1.0 ? count : std::max<int64_t>(1, (int64_t)std::llround(count / speed));
                    const short* src = pcm + firstS * kChannels;
                    const short* outData = src;
                    if (speed != 1.0) {
                        resampled.resize((size_t)(outCount * kChannels));
                        for (int64_t i = 0; i < outCount; ++i) {
                            const double x = i * speed;
                            const int64_t i0 = std::min<int64_t>((int64_t)x, count - 1), i1 = std::min<int64_t>(i0 + 1, count - 1);
                            const double f = x - (double)i0;
                            for (UINT32 c = 0; c < kChannels; ++c)
                                resampled[(size_t)(i * kChannels + c)] = (short)std::lround(src[i0 * kChannels + c] * (1.0 - f) + src[i1 * kChannels + c] * f);
                        }
                        outData = resampled.data();
                    }
                    int64_t skip = 0;
                    wr = alignAudio(tr, (int64_t)std::llround((outBase + startRel / speed) * kRate), skip);
                    const int64_t maxOut = (int64_t)std::llround(segEndOut * kRate) - tr.written;
                    const int64_t toWrite = std::min(outCount - skip, maxOut);
                    if (SUCCEEDED(wr) && toWrite > 0) wr = writeAudio(tr, outData + skip * kChannels, toWrite);
                }
                buf->Unlock();
                if (FAILED(wr)) {
                    abandon();
                    return Fail(error, "Couldn't export the audio", wr);
                }
                if (startRel + (double)(n - firstS) / kRate >= segLen) {
                    trackDone[(size_t)track] = true;
                    reader->SetStreamSelection(si, FALSE);
                }
            }
        }
        // Close the section: hold the last frame if the video ran short, pad audio with silence.
        while (haveFrame && nextGrid < segEndOut - frameDur * 0.5) {
            hr = emitFrame(nextGrid);
            if (FAILED(hr)) {
                abandon();
                return Fail(error, "Couldn't export", hr);
            }
            nextGrid += frameDur;
        }
        for (TrackOut& tr : tracks) {
            int64_t skip = 0;
            hr = alignAudio(tr, (int64_t)std::llround(segEndOut * kRate), skip);
            if (FAILED(hr)) {
                abandon();
                return Fail(error, "Couldn't export the audio", hr);
            }
        }
        outBase = segEndOut;
    }
    if (frames == 0) {
        abandon();
        error = "Couldn't export: no video frames in the selection.";
        return false;
    }
    hr = writer->Finalize();
    if (FAILED(hr)) {
        DeleteFileW(req.destination.c_str());
        return Fail(error, "Couldn't finish the file", hr);
    }
    if (progress) progress(1.0f);
    Log("Transcode: %d sections, %d frames, %.2f s, %ux%u @ %.0f fps, %.1f Mbit/s%s%s", (int)req.segments.size(), frames, total, W, H, fps,
        bitrate / 1e6, req.banner ? ", banner" : "", captions.empty() ? "" : ", captions");
    return true;
}

} // namespace cs::engine
