#include "engine/FFmpegTools.h"
#include "engine/Encoder.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>

namespace cs::engine {

std::wstring FindFFmpeg(const std::wstring& configured)
{
    if (!configured.empty() && FileExists(configured)) return configured;
    const std::wstring dir = ExeDir();
    for (const wchar_t* rel : { L"ffmpeg.exe", L"ffmpeg\\bin\\ffmpeg.exe", L"ffmpeg\\ffmpeg.exe" }) {
        std::wstring p = JoinPath(dir, rel);
        if (FileExists(p)) return p;
    }
    wchar_t found[MAX_PATH * 2];
    DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, (DWORD)(sizeof(found) / sizeof(found[0])), found, nullptr);
    if (n > 0 && n < sizeof(found) / sizeof(found[0])) return std::wstring(found, n);
    return {};
}

std::wstring QuoteArg(const std::wstring& s)
{
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L"\\\"";
        else out.push_back(c);
    }
    // A trailing backslash would escape the closing quote.
    if (!s.empty() && s.back() == L'\\') out += L"\\";
    out += L"\"";
    return out;
}

static std::wstring Num(double v)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.3f", v);
    return buf;
}

static const wchar_t* kCommon = L"-hide_banner -nostdin -y -loglevel error -progress pipe:1 -nostats";

std::wstring BuildTestClipArgs(const std::wstring& out, double seconds, int fps, bool useX264)
{
    wchar_t src[160];
    swprintf(src, 160, L"testsrc2=size=1280x720:rate=%d", std::max(24, std::min(fps, 60)));
    std::wstring a = kCommon;
    a += L" -f lavfi -i " + QuoteArg(src);
    a += L" -f lavfi -i " + QuoteArg(L"sine=frequency=440:sample_rate=48000");
    a += L" -t " + Num(std::max(1.0, seconds));
    if (useX264) a += L" -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p -g 60";
    else a += L" -c:v mpeg4 -q:v 5 -g 60";
    a += L" -c:a aac -b:a 128k -shortest -movflags +faststart " + QuoteArg(out);
    return a;
}

std::wstring BuildProbeArgs(const std::string& encoderName)
{
    std::wstring a = L"-hide_banner -nostdin -loglevel error -f lavfi -i color=black:s=256x256:r=30 -frames:v 3 -c:v ";
    a += ToWide(encoderName);
    a += L" -f null -";
    return a;
}

FFmpegResult RunFFmpeg(const std::wstring& exe, const std::wstring& args, double expectedSeconds,
                       const std::function<void(float)>& onProgress, const std::atomic<bool>* cancel)
{
    FFmpegResult res;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        res.errors = "Couldn't create a pipe for FFmpeg.";
        return res;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nulIn;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = QuoteArg(exe) + L" " + args;
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
    if (!ok) {
        CloseHandle(readPipe);
        res.errors = "Couldn't start FFmpeg (" + ToUtf8(exe) + ").";
        return res;
    }
    res.started = true;

    std::string pending;
    auto handleLine = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) return;
        if (line.rfind("out_time_us=", 0) == 0 || line.rfind("out_time_ms=", 0) == 0) {
            // Despite its name, out_time_ms is in microseconds too.
            long long us = atoll(line.c_str() + line.find('=') + 1);
            if (onProgress && expectedSeconds > 0.0 && us > 0)
                onProgress((float)std::min(1.0, (us / 1e6) / expectedSeconds));
            return;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq < 24 && line.find(' ') == std::string::npos) return;   // other progress keys
        res.errors += line + "\n";
        if (res.errors.size() > 4000) res.errors.erase(0, res.errors.size() - 4000);
    };

    bool exited = false;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) break;   // pipe closed
        if (avail > 0) {
            DWORD n = 0;
            if (!ReadFile(readPipe, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || n == 0) break;
            pending.append(buf, n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                handleLine(pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
            continue;
        }
        if (exited) break;
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            res.cancelled = true;
            WaitForSingleObject(pi.hProcess, 3000);
            break;
        }
        exited = WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0;
    }
    if (!pending.empty()) handleLine(pending);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    res.exitCode = (int)code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    if (onProgress && res.exitCode == 0 && !res.cancelled) onProgress(1.0f);
    return res;
}

} // namespace cs::engine
