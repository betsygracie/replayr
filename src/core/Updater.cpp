#include "core/Updater.h"
#include "AppInfo.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include "nlohmann/json.hpp"

#include <cstdio>
#include <vector>

namespace cs {

namespace {

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

// "v1.2.3" / "1.2.3" -> {1,2,3}
std::vector<int> ParseVersion(std::string v)
{
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    int cur = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            parts.push_back(cur);
            cur = 0;
        } else {
            break;
        }
    }
    if (any) parts.push_back(cur);
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

bool Newer(const std::string& a, const std::string& b) { return ParseVersion(a) > ParseVersion(b); }

// GET over HTTPS (follows redirects). Body into `out`, or streamed to `file` when given.
bool HttpGet(const std::wstring& url, std::string* out, const std::wstring& file, const std::atomic<bool>* quit)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    const std::wstring fullPath = std::wstring(path) + extra;
    HINTERNET s = WinHttpOpen(L"Replayr-Updater/" CS_APP_NAME_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            FILE* f = file.empty() ? nullptr : _wfopen(file.c_str(), L"wb");
            ok = file.empty() || f;
            for (;;) {
                if (quit && quit->load()) {
                    ok = false;
                    break;
                }
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) {
                    ok = false;
                    break;
                }
                if (avail == 0) break;
                std::vector<char> buf(avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, buf.data(), avail, &got)) {
                    ok = false;
                    break;
                }
                if (f) fwrite(buf.data(), 1, got, f);
                else if (out) out->append(buf.data(), got);
            }
            if (f) fclose(f);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

std::atomic<bool> g_quit { false };

} // namespace

Updater::~Updater()
{
    g_quit = true;
    if (thread_.joinable()) thread_.join();
}

bool Updater::Enabled() { return CS_UPDATE_REPO[0] != 0; }

void Updater::CleanupOld()
{
    const std::wstring old = ParentDir(ExePath()) + L"\\Replayr.old.exe";
    DeleteFileW(old.c_str());
}

std::string Updater::Version() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

std::string Updater::Message() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

void Updater::Check(bool download)
{
    if (!Enabled()) {
        state_ = UpdateState::Off;
        return;
    }
    const UpdateState st = state_.load();
    if (st == UpdateState::Checking || st == UpdateState::Downloading || st == UpdateState::Ready) return;
    if (thread_.joinable()) thread_.join();
    state_ = UpdateState::Checking;
    thread_ = std::thread([this, download] { Run(download); });
}

void Updater::Run(bool download)
{
    auto fail = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = msg;
        state_ = UpdateState::Failed;
        Log("Update check: %s", msg.c_str());
    };
    std::string body;
    const std::wstring api = L"https://api.github.com/repos/" + ToWide(CS_UPDATE_REPO) + L"/releases/latest";
    if (!HttpGet(api, &body, L"", &g_quit)) return fail("Couldn't reach the update server.");
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("tag_name")) return fail("The update server sent something unexpected.");
    const std::string tag = j["tag_name"].get<std::string>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version_ = tag;
    }
    if (!Newer(tag, CS_APP_VERSION)) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "You have the latest version.";
        state_ = UpdateState::UpToDate;
        return;
    }
    std::string url;
    if (j.contains("assets") && j["assets"].is_array())
        for (auto& a : j["assets"]) {
            const std::string name = a.value("name", "");
            if (name.size() > 4 && (name.substr(name.size() - 4) == ".exe" || name.substr(name.size() - 4) == ".EXE")) {
                url = a.value("browser_download_url", "");
                break;
            }
        }
    if (url.empty()) return fail("Version " + tag + " is out, but it has no .exe to download.");
    if (!download) {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Version " + tag + " is available.";
        state_ = UpdateState::Idle;
        return;
    }
    state_ = UpdateState::Downloading;
    const std::wstring dest = ParentDir(ExePath()) + L"\\Replayr.new.exe";
    if (!HttpGet(ToWide(url), nullptr, dest, &g_quit)) {
        DeleteFileW(dest.c_str());
        return fail("Couldn't download version " + tag + ".");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    downloaded_ = dest;
    message_ = "Version " + tag + " is ready. Restart Replayr to use it.";
    state_ = UpdateState::Ready;
    Log("Update %s downloaded", tag.c_str());
}

bool Updater::Apply()
{
    std::wstring file;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        file = downloaded_;
    }
    if (state_.load() != UpdateState::Ready || file.empty()) return false;
    const std::wstring exe = ExePath(), old = ParentDir(exe) + L"\\Replayr.old.exe";
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log("Update: couldn't move the running exe aside (%lu)", GetLastError());
        return false;
    }
    if (!MoveFileExW(file.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);   // put it back
        Log("Update: couldn't put the new exe in place (%lu)", GetLastError());
        return false;
    }
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"--updated", nullptr, SW_SHOWNORMAL);
    return true;
}

} // namespace cs
