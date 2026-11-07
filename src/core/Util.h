#pragma once
#include <cstdint>
#include <string>

// Small helpers shared by every layer. No Windows types in this header.
namespace cs {

std::string  ToUtf8(const std::wstring& w);
std::wstring ToWide(const std::string& s);

std::wstring AppDataDir();        // %APPDATA%\Replayr (created on demand)
std::wstring LocalDataDir();      // %LOCALAPPDATA%\Replayr (created on demand)
std::string TimeOfDayText();      // local time, "21:14"
std::wstring DefaultClipsDir();   // %USERPROFILE%\Videos\Replayr
std::wstring ExeDir();
std::wstring WindowsFontsDir();

bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring FileStem(const std::wstring& path);      // "C:\a\b.mp4" -> "b"
std::wstring FileExtension(const std::wstring& path); // ".mp4" (lower case)
std::wstring ParentDir(const std::wstring& path);
std::wstring SanitizeFileName(const std::wstring& name);
std::wstring UniquePath(const std::wstring& path);    // adds " (2)", " (3)"... if the file exists

std::wstring TimestampForFile();                    // "2026-11-06 03-30-12"
int64_t      NowUnix();
double       NowSeconds();                          // steady clock

std::string FormatDuration(double seconds);         // "0:32", "12:04", "1:02:03"
std::string FormatClock(double seconds);            // "1:02.345" (editor timestamps)
std::string FormatBytes(uint64_t bytes);            // "12.4 MB"
std::string FormatDate(int64_t unixSeconds);        // "Today 03:30", "Yesterday 21:10", "6 Nov 2026"

std::wstring IntToW(long long v);
std::string ToLowerAscii(std::string s);
bool        ContainsNoCase(const std::string& haystack, const std::string& needle);

template <typename T> inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace cs
