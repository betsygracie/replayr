#include "engine/Encoder.h"
#include "core/Util.h"

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

const char* EncoderLabel(EncoderKind k)
{
    switch (k) {
    case EncoderKind::Auto: return "Auto";
    case EncoderKind::NVENC: return "NVIDIA NVENC";
    case EncoderKind::AMF: return "AMD AMF";
    case EncoderKind::QuickSync: return "Intel Quick Sync";
    case EncoderKind::X264: return "x264 (CPU)";
    }
    return "?";
}

const char* EncoderShortLabel(EncoderKind k)
{
    switch (k) {
    case EncoderKind::Auto: return "Auto";
    case EncoderKind::NVENC: return "NVENC";
    case EncoderKind::AMF: return "AMF";
    case EncoderKind::QuickSync: return "QuickSync";
    case EncoderKind::X264: return "x264";
    }
    return "?";
}

const char* CodecLabel(Codec c)
{
    switch (c) {
    case Codec::H264: return "H.264";
    case Codec::HEVC: return "HEVC";
    case Codec::AV1: return "AV1";
    }
    return "?";
}

const char* EncoderKey(EncoderKind k)
{
    switch (k) {
    case EncoderKind::Auto: return "auto";
    case EncoderKind::NVENC: return "nvenc";
    case EncoderKind::AMF: return "amf";
    case EncoderKind::QuickSync: return "qsv";
    case EncoderKind::X264: return "x264";
    }
    return "auto";
}

const char* CodecKey(Codec c)
{
    switch (c) {
    case Codec::H264: return "h264";
    case Codec::HEVC: return "hevc";
    case Codec::AV1: return "av1";
    }
    return "h264";
}

EncoderKind ParseEncoder(const std::string& s)
{
    if (s == "nvenc") return EncoderKind::NVENC;
    if (s == "amf") return EncoderKind::AMF;
    if (s == "qsv") return EncoderKind::QuickSync;
    if (s == "x264") return EncoderKind::X264;
    return EncoderKind::Auto;
}

Codec ParseCodec(const std::string& s)
{
    if (s == "hevc") return Codec::HEVC;
    if (s == "av1") return Codec::AV1;
    return Codec::H264;
}

std::vector<GpuAdapter> EnumerateGpus()
{
    std::vector<GpuAdapter> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            GpuAdapter g;
            g.name = ToUtf8(d.Description);
            g.vendorId = d.VendorId;
            g.dedicatedMemory = d.DedicatedVideoMemory;
            bool dup = false;
            for (auto& e : out)
                if (e.name == g.name && e.vendorId == g.vendorId) dup = true;
            if (!dup) out.push_back(g);
        }
        adapter.Reset();
    }
    return out;
}

std::vector<EncoderInfo> DetectEncoders()
{
    std::vector<EncoderInfo> list;
    auto gpus = EnumerateGpus();
    // Discrete GPU (most dedicated VRAM) first: that is what "Auto" should pick.
    std::sort(gpus.begin(), gpus.end(), [](const GpuAdapter& a, const GpuAdapter& b) { return a.dedicatedMemory > b.dedicatedMemory; });
    auto add = [&](EncoderKind k, uint32_t vendor) {
        EncoderInfo e;
        e.kind = k;
        e.hardware = true;
        for (auto& g : gpus)
            if (g.vendorId == vendor) {
                e.detected = true;
                e.gpuName = g.name;
                break;
            }
        list.push_back(e);
    };
    // Keep the order of the detected GPUs so the best hardware encoder comes first.
    std::vector<std::pair<EncoderKind, uint32_t>> order;
    for (auto& g : gpus) {
        EncoderKind k = g.vendorId == 0x10DE ? EncoderKind::NVENC : g.vendorId == 0x1002 ? EncoderKind::AMF
                      : g.vendorId == 0x8086 ? EncoderKind::QuickSync : EncoderKind::Auto;
        if (k == EncoderKind::Auto) continue;
        bool seen = false;
        for (auto& o : order)
            if (o.first == k) seen = true;
        if (!seen) order.emplace_back(k, g.vendorId);
    }
    for (auto& o : order) add(o.first, o.second);
    for (auto k : { EncoderKind::NVENC, EncoderKind::AMF, EncoderKind::QuickSync }) {
        bool present = false;
        for (auto& e : list)
            if (e.kind == k) present = true;
        if (!present) add(k, k == EncoderKind::NVENC ? 0x10DE : k == EncoderKind::AMF ? 0x1002 : 0x8086);
    }
    EncoderInfo cpu;
    cpu.kind = EncoderKind::X264;
    cpu.detected = true;
    list.push_back(cpu);
    return list;
}

EncoderKind ResolveEncoder(EncoderKind wanted, const std::vector<EncoderInfo>& encoders)
{
    if (wanted != EncoderKind::Auto) return wanted;
    for (const EncoderInfo& e : encoders)
        if (e.hardware && e.detected && e.probe != 0) return e.kind;
    return EncoderKind::X264;
}

std::string FfmpegEncoderName(EncoderKind k, Codec c)
{
    const char* codec = c == Codec::HEVC ? "hevc" : c == Codec::AV1 ? "av1" : "h264";
    switch (k) {
    case EncoderKind::NVENC: return std::string(codec) + "_nvenc";
    case EncoderKind::AMF: return std::string(codec) + "_amf";
    case EncoderKind::QuickSync: return std::string(codec) + "_qsv";
    default: return c == Codec::HEVC ? "libx265" : c == Codec::AV1 ? "libsvtav1" : "libx264";
    }
}

std::wstring FfmpegQualityArgs(EncoderKind k, Codec c, int q)
{
    wchar_t buf[160];
    switch (k) {
    case EncoderKind::NVENC:
        swprintf(buf, 160, L"-preset p5 -rc vbr -cq %d -b:v 0", q);
        break;
    case EncoderKind::AMF:
        swprintf(buf, 160, L"-quality balanced -rc cqp -qp_i %d -qp_p %d", q, q + 2);
        break;
    case EncoderKind::QuickSync:
        swprintf(buf, 160, L"-preset medium -global_quality %d", q);
        break;
    default:
        if (c == Codec::AV1) swprintf(buf, 160, L"-preset 8 -crf %d -pix_fmt yuv420p", q + 10);
        else swprintf(buf, 160, L"-preset veryfast -crf %d -pix_fmt yuv420p", q);
        break;
    }
    return buf;
}

} // namespace cs::engine
