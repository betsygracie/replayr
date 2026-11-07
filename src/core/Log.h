#pragma once
#include <string>
#include <vector>

namespace cs {

// Tiny thread-safe logger: %APPDATA%\Replayr\logs\replayr.log + debugger output, and the
// last few hundred lines in memory for the in-app log panel (Settings > About).
void LogInit(const std::wstring& path);
void LogShutdown();
void Log(const char* fmt, ...);
void LogToStdout(bool on);                       // --console (debug builds)
std::vector<std::string> LogRecent(size_t maxLines);

} // namespace cs
