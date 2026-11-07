#pragma once
#include "engine/ClipEngine.h"

#include <string>
#include <vector>

namespace cs::engine {

struct GpuAdapter {
    std::string name;
    uint32_t vendorId = 0;           // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    uint64_t dedicatedMemory = 0;
};

std::vector<GpuAdapter> EnumerateGpus();
std::vector<EncoderInfo> DetectEncoders();          // NVENC/AMF/QSV by GPU vendor, plus x264
EncoderKind ResolveEncoder(EncoderKind wanted, const std::vector<EncoderInfo>& encoders);

std::string FfmpegEncoderName(EncoderKind k, Codec c);   // "h264_nvenc", "libx264", ...
std::wstring FfmpegQualityArgs(EncoderKind k, Codec c, int quality);

} // namespace cs::engine
