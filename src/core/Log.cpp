#include "core/Log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cs {

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;
static std::deque<std::string> g_recent;   // for the in-app log panel
static bool g_toStdout = false;

void LogToStdout(bool on) { g_toStdout = on; }

std::vector<std::string> LogRecent(size_t maxLines)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    const size_t n = std::min(maxLines, g_recent.size());
    return std::vector<std::string>(g_recent.end() - (ptrdiff_t)n, g_recent.end());
}

void LogInit(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) return;
    // Start fresh when the log gets big.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    bool truncate = false;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        truncate = fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 2u * 1024u * 1024u;
    g_logFile = _wfopen(path.c_str(), truncate ? L"wb" : L"ab");
}

void LogShutdown()
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
}

void Log(const char* fmt, ...)
{
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);

    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_toStdout) fputs(line, stdout);
    std::string entry(line);
    if (!entry.empty() && entry.back() == '\n') entry.pop_back();
    g_recent.push_back(std::move(entry));
    if (g_recent.size() > 400) g_recent.pop_front();
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

} // namespace cs
