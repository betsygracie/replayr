#pragma once
#include "engine/ClipEngine.h"

#include <atomic>
#include <functional>
#include <string>

namespace cs::engine {

// Optional FFmpeg helpers (encoder probing, test clips). Exports no longer need FFmpeg: see Transcoder.
// Locates ffmpeg.exe: configured path, next to Replayr.exe, then PATH. "" if not found.
std::wstring FindFFmpeg(const std::wstring& configured);

struct FFmpegResult {
    int exitCode = -1;
    bool started = false;
    bool cancelled = false;
    std::string errors;   // last lines FFmpeg printed at loglevel "error"
};

// Runs ffmpeg.exe hidden, at below-normal priority, parsing "-progress pipe:1" output.
// Blocking: call from a worker thread.
FFmpegResult RunFFmpeg(const std::wstring& exe, const std::wstring& args, double expectedSeconds,
                       const std::function<void(float)>& onProgress, const std::atomic<bool>* cancel);

std::wstring QuoteArg(const std::wstring& s);
std::wstring BuildTestClipArgs(const std::wstring& out, double seconds, int fps, bool useX264);
std::wstring BuildProbeArgs(const std::string& encoderName);

} // namespace cs::engine
