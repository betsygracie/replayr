#include "core/Util.h"
#include "AppInfo.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <cwctype>

namespace cs {

std::string ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) && p)
        out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

std::wstring AppDataDir()
{
    std::wstring dir = JoinPath(KnownFolder(FOLDERID_RoamingAppData), CS_APP_NAME_W);
    EnsureDir(dir);
    return dir;
}

std::wstring LocalDataDir()
{
    std::wstring dir = JoinPath(KnownFolder(FOLDERID_LocalAppData), CS_APP_NAME_W);
    EnsureDir(dir);
    return dir;
}

std::wstring DefaultClipsDir()
{
    std::wstring videos = KnownFolder(FOLDERID_Videos);
    if (videos.empty()) videos = KnownFolder(FOLDERID_Profile);
    return JoinPath(videos, CS_APP_NAME_W);
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return ParentDir(std::wstring(buf, n));
}

std::wstring WindowsFontsDir()
{
    std::wstring dir = KnownFolder(FOLDERID_Fonts);
    if (dir.empty()) dir = L"C:\\Windows\\Fonts";
    return dir;
}

bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path)
{
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static size_t LastSlash(const std::wstring& p)
{
    size_t a = p.find_last_of(L"\\/");
    return a;
}

std::wstring FileStem(const std::wstring& path)
{
    size_t s = LastSlash(path);
    std::wstring name = (s == std::wstring::npos) ? path : path.substr(s + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring FileExtension(const std::wstring& path)
{
    size_t s = LastSlash(path);
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (s != std::wstring::npos && dot < s)) return {};
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext;
}

std::wstring ParentDir(const std::wstring& path)
{
    size_t s = LastSlash(path);
    if (s == std::wstring::npos) return {};
    return path.substr(0, s);
}

std::wstring SanitizeFileName(const std::wstring& name)
{
    std::wstring out;
    out.reserve(name.size());
    for (wchar_t c : name) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) out.push_back(L'_');
        else out.push_back(c);
    }
    // No trailing dots/spaces on Windows.
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.empty()) out = L"Clip";
    if (out.size() > 120) out.resize(120);
    return out;
}

std::wstring UniquePath(const std::wstring& path)
{
    if (!FileExists(path) && !DirExists(path)) return path;
    std::wstring dir = ParentDir(path), stem = FileStem(path), ext = FileExtension(path);
    for (int i = 2; i < 10000; ++i) {
        wchar_t suffix[24];
        swprintf(suffix, 24, L" (%d)", i);
        std::wstring candidate = JoinPath(dir, stem + suffix + ext);
        if (!FileExists(candidate)) return candidate;
    }
    return path;
}

std::wstring TimestampForFile()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

int64_t NowUnix()
{
    return (int64_t)std::time(nullptr);
}

double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string FormatDuration(double seconds)
{
    if (!(seconds >= 0.0)) seconds = 0.0;
    long long t = (long long)(seconds + 0.5);
    long long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

std::string FormatClock(double seconds)
{
    if (!(seconds >= 0.0)) seconds = 0.0;
    long long ms = (long long)(seconds * 1000.0 + 0.5);
    long long m = ms / 60000, s = (ms / 1000) % 60, rem = ms % 1000;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", m, s, rem);
    return buf;
}

std::string FormatBytes(uint64_t bytes)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    else snprintf(buf, sizeof(buf), v >= 100.0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string FormatDate(int64_t unixSeconds)
{
    time_t t = (time_t)unixSeconds;
    time_t now = std::time(nullptr);
    struct tm lt {}, ln {};
    localtime_s(&lt, &t);
    localtime_s(&ln, &now);
    static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char buf[64];
    // Day difference using local midnight boundaries.
    struct tm a = lt, b = ln;
    a.tm_hour = a.tm_min = a.tm_sec = 0;
    b.tm_hour = b.tm_min = b.tm_sec = 0;
    long long days = (long long)((mktime(&b) - mktime(&a)) / 86400);
    if (days == 0) snprintf(buf, sizeof(buf), "Today %02d:%02d", lt.tm_hour, lt.tm_min);
    else if (days == 1) snprintf(buf, sizeof(buf), "Yesterday %02d:%02d", lt.tm_hour, lt.tm_min);
    else snprintf(buf, sizeof(buf), "%d %s %d", lt.tm_mday, months[lt.tm_mon % 12], lt.tm_year + 1900);
    return buf;
}

std::wstring IntToW(long long v)
{
    wchar_t buf[32];
    swprintf(buf, 32, L"%lld", v);
    return buf;
}

std::string ToLowerAscii(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    return ToLowerAscii(haystack).find(ToLowerAscii(needle)) != std::string::npos;
}

std::string TimeOfDayText()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

std::wstring SuggestionsDir() { return JoinPath(LocalDataDir(), L"Suggestions"); }

std::vector<std::wstring> ListFiles(const std::wstring& dir, const wchar_t* pattern)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(JoinPath(dir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

} // namespace cs
