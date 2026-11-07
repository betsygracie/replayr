#include "account/Account.h"
#include "AppInfo.h"
#include "core/Log.h"
#include "core/Util.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <wincred.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace cs {

namespace {

const wchar_t* kCredTarget = L"Replayr/session";
std::string g_server = CS_ACCOUNT_SERVER;

std::string B64Url(const unsigned char* data, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        if (i + 1 < n) out += t[(v >> 6) & 63];
        if (i + 2 < n) out += t[v & 63];
    }
    return out;
}

std::string RandomToken(size_t bytes)
{
    std::vector<unsigned char> b(bytes);
    BCryptGenRandom(nullptr, b.data(), (ULONG)b.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return B64Url(b.data(), b.size());
}

std::string Sha256B64Url(const std::string& text)
{
    unsigned char hash[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)text.data(), (ULONG)text.size(), hash, sizeof(hash));
    return B64Url(hash, sizeof(hash));
}

std::string UrlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& key)
{
    size_t p = 0;
    while (p < query.size()) {
        size_t amp = query.find('&', p);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(p, amp - p);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) return UrlDecode(kv.substr(eq + 1));
        p = amp + 1;
    }
    return "";
}

std::string ComputerName()
{
    wchar_t buf[64] = {};
    DWORD n = 64;
    GetComputerNameW(buf, &n);
    return ToUtf8(buf);
}

struct HttpResult {
    int status = 0;
    std::string body;
};

} // namespace

struct Account::Impl {
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    std::thread worker, signInThread;
    std::atomic<bool> quit { false }, cancelSignIn { false };
    AccountState state = AccountState::SignedOut;
    AccountUser user;
    std::vector<CloudClip> clips;
    bool clipsLoading = false;
    std::deque<UploadItem> uploads;
    std::deque<AccountEvent> events;
    std::wstring cacheDir, avatarPath;
    // Tokens: touched only on the worker thread (and the sign-in thread before it hands over).
    std::string access, refresh;
    int64_t accessExpires = 0;
    HINTERNET session = nullptr;

    void Post(std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> lock(m);
            tasks.push_back(std::move(f));
        }
        cv.notify_one();
    }

    void Emit(AccountEventType t, const std::string& msg, const std::string& title = "")
    {
        std::lock_guard<std::mutex> lock(m);
        events.push_back({ t, msg, title });
    }

    void Run()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return quit.load() || !tasks.empty(); });
                if (quit) return;
                f = std::move(tasks.front());
                tasks.pop_front();
            }
            f();
        }
    }

    // ---- HTTP --------------------------------------------------------------------------------

    // One request. `url` is a full https URL. When `uploadFile` is given its contents are streamed
    // as the body (with progress); otherwise `body` (JSON) is sent.
    HttpResult Request(const std::wstring& method, const std::wstring& url, const std::string& body, bool auth,
                       const std::wstring& uploadFile = L"", const std::function<void(float)>& progress = {})
    {
        HttpResult res;
        if (!session)
            session = WinHttpOpen(L"Replayr/" CS_APP_NAME_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) return res;
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        uc.lpszExtraInfo = extra;
        uc.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return res;
        const std::wstring full = std::wstring(path) + extra;
        HINTERNET c = WinHttpConnect(session, host, uc.nPort, 0);
        HINTERNET r = c ? WinHttpOpenRequest(c, method.c_str(), full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
        if (r) {
            DWORD timeout = 30000;
            WinHttpSetOption(r, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
            std::wstring headers = uploadFile.empty() ? L"Content-Type: application/json\r\n" : L"Content-Type: video/mp4\r\n";
            if (auth && !access.empty()) headers += L"Authorization: Bearer " + ToWide(access) + L"\r\n";
            bool sent = false;
            if (uploadFile.empty()) {
                sent = WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                                          (DWORD)body.size(), 0);
            } else if (FILE* f = _wfopen(uploadFile.c_str(), L"rb")) {
                _fseeki64(f, 0, SEEK_END);
                const int64_t total = _ftelli64(f);
                _fseeki64(f, 0, SEEK_SET);
                sent = WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)total, 0);
                std::vector<char> buf(256 * 1024);
                int64_t done = 0;
                while (sent && done < total && !quit) {
                    const size_t n = fread(buf.data(), 1, buf.size(), f);
                    if (n == 0) break;
                    DWORD wrote = 0;
                    sent = WinHttpWriteData(r, buf.data(), (DWORD)n, &wrote);
                    done += wrote;
                    if (progress) progress((float)done / (float)std::max<int64_t>(1, total));
                }
                fclose(f);
            }
            if (sent && WinHttpReceiveResponse(r, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                res.status = (int)status;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(r, chunk.data(), avail, &got)) break;
                    res.body.append(chunk.data(), got);
                }
            }
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        return res;
    }

    static std::wstring Server(const std::string& path) { return ToWide(g_server + path); }

    static std::string ErrorOf(const HttpResult& r, const char* fallback)
    {
        if (r.status == 0) return "Couldn't reach the Replayr server. Check your internet connection.";
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (!j.is_discarded() && j.contains("error") && j["error"].is_string()) return j["error"].get<std::string>();
        return fallback;
    }

    // An authorised request: refreshes the access token first when it's about to expire, and once
    // more if the server says it's no longer valid.
    HttpResult Authed(const std::wstring& method, const std::string& path, const std::string& body, const std::wstring& file = L"",
                      const std::function<void(float)>& progress = {})
    {
        if (refresh.empty()) return HttpResult{ 401, "" };
        if (access.empty() || (int64_t)time(nullptr) > accessExpires - 60)
            if (!Refresh()) return HttpResult{ 401, "" };
        HttpResult r = Request(method, Server(path), body, true, file, progress);
        if (r.status == 401 && Refresh()) r = Request(method, Server(path), body, true, file, progress);
        return r;
    }

    void TakeTokens(const nlohmann::json& j)
    {
        access = j.value("accessToken", "");
        refresh = j.value("refreshToken", "");
        accessExpires = (int64_t)time(nullptr) + j.value("expiresIn", 3600);
        SaveRefresh();
        if (j.contains("user")) TakeUser(j["user"]);
    }

    void TakeUser(const nlohmann::json& u)
    {
        AccountUser nu;
        nu.id = u.value("id", "");
        nu.name = u.value("name", "");
        nu.provider = u.value("provider", "");
        if (u.contains("avatar") && u["avatar"].is_string()) nu.avatarUrl = u["avatar"].get<std::string>();
        nu.cloudClips = u.contains("storage") && u["storage"].is_boolean() && u["storage"].get<bool>();
        nu.usedBytes = u.value("usedBytes", (uint64_t)0);
        nu.quotaBytes = u.value("quotaBytes", (uint64_t)0);
        bool newAvatar;
        {
            std::lock_guard<std::mutex> lock(m);
            newAvatar = nu.avatarUrl != user.avatarUrl || avatarPath.empty();
            user = nu;
            state = AccountState::SignedIn;
        }
        if (newAvatar && !nu.avatarUrl.empty()) DownloadAvatar(nu.avatarUrl);
    }

    void DownloadAvatar(const std::string& url)
    {
        HttpResult r = Request(L"GET", ToWide(url), "", false);
        if (r.status != 200 || r.body.empty()) return;
        const std::wstring file = JoinPath(cacheDir, L"avatar_" + ToWide(Sha256B64Url(url).substr(0, 12)) + L".img");
        if (FILE* f = _wfopen(file.c_str(), L"wb")) {
            fwrite(r.body.data(), 1, r.body.size(), f);
            fclose(f);
            std::lock_guard<std::mutex> lock(m);
            avatarPath = file;
        }
    }

    bool Refresh()
    {
        if (refresh.empty()) return false;
        nlohmann::json b;
        b["refreshToken"] = refresh;
        HttpResult r = Request(L"POST", Server("/auth/refresh"), b.dump(), false);
        if (r.status == 200) {
            auto j = nlohmann::json::parse(r.body, nullptr, false);
            if (!j.is_discarded()) {
                TakeTokens(j);
                return true;
            }
        }
        if (r.status == 401 || r.status == 403) {
            // The session ended (expired, signed out elsewhere or banned): forget it.
            ClearLocal();
            Emit(AccountEventType::SignedOut, "You were signed out. Sign in again to upload.");
        }
        return false;
    }

    void ClearLocal()
    {
        access.clear();
        refresh.clear();
        CredDeleteW(kCredTarget, CRED_TYPE_GENERIC, 0);
        std::lock_guard<std::mutex> lock(m);
        state = AccountState::SignedOut;
        user = AccountUser{};
        clips.clear();
        avatarPath.clear();
    }

    // The refresh token lives in Windows Credential Manager (encrypted for this Windows user), never in
    // a settings file.
    void SaveRefresh()
    {
        if (refresh.empty()) return;
        CREDENTIALW c{};
        c.Type = CRED_TYPE_GENERIC;
        c.TargetName = (LPWSTR)kCredTarget;
        c.CredentialBlobSize = (DWORD)refresh.size();
        c.CredentialBlob = (LPBYTE)refresh.data();
        c.Persist = CRED_PERSIST_LOCAL_MACHINE;
        c.UserName = (LPWSTR)L"Replayr";
        if (!CredWriteW(&c, 0)) Log("Account: couldn't save the session (%lu)", GetLastError());
    }

    std::string LoadRefresh()
    {
        PCREDENTIALW c = nullptr;
        if (!CredReadW(kCredTarget, CRED_TYPE_GENERIC, 0, &c) || !c) return "";
        std::string s((const char*)c->CredentialBlob, c->CredentialBlobSize);
        CredFree(c);
        return s;
    }

    // ---- sign-in -------------------------------------------------------------------------------

    // Username + password; with an invite code it makes the account first. Runs on the worker thread.
    void SignInFlow(const std::string& username, const std::string& password, const std::string& code)
    {
        nlohmann::json b;
        b["username"] = username;
        b["password"] = password;
        b["device"] = ComputerName();
        if (!code.empty()) b["code"] = code;
        HttpResult r = Request(L"POST", Server(code.empty() ? "/auth/login" : "/auth/register"), b.dump(), false);
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (r.status != 200 || j.is_discarded()) {
            Fail(ErrorOf(r, code.empty() ? "Couldn't sign in." : "Couldn't make the account."));
            return;
        }
        TakeTokens(j);
        Emit(AccountEventType::SignedIn, (code.empty() ? "Signed in as " : "Welcome to Replayr, ") + user.name);
        LoadClips();
    }

    void Fail(const std::string& msg)
    {
        {
            std::lock_guard<std::mutex> lock(m);
            state = AccountState::SignedOut;
        }
        Emit(AccountEventType::SignInFailed, msg);
    }

    // ---- clips ---------------------------------------------------------------------------------

    void LoadClips()
    {
        {
            std::lock_guard<std::mutex> lock(m);
            clipsLoading = true;
        }
        HttpResult r = Authed(L"GET", "/clips", "");
        std::vector<CloudClip> list;
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (r.status == 200 && !j.is_discarded()) {
            for (auto& c : j["clips"]) {
                CloudClip cc;
                cc.id = c.value("id", "");
                cc.title = c.value("title", "");
                cc.privacy = c.value("privacy", "unlisted");
                cc.url = c.value("url", "");
                cc.sizeBytes = c.value("sizeBytes", (uint64_t)0);
                cc.duration = c.value("duration", 0.0);
                cc.views = c.value("views", (int64_t)0);
                cc.createdAt = c.value("createdAt", (int64_t)0);
                list.push_back(cc);
            }
            if (j.contains("user")) TakeUser(j["user"]);
        }
        std::lock_guard<std::mutex> lock(m);
        clipsLoading = false;
        if (r.status == 200) clips = std::move(list);
    }

    void DoUpload(UploadItem item, double duration, std::string privacy)
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(item.path.c_str(), GetFileExInfoStandard, &fa)) {
            Emit(AccountEventType::UploadFailed, "The file is gone.", item.title);
            return;
        }
        const uint64_t size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        nlohmann::json b;
        b["title"] = item.title;
        b["sizeBytes"] = size;
        b["duration"] = duration;
        b["privacy"] = privacy;
        HttpResult r = Authed(L"POST", "/clips", b.dump());
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (r.status != 200 || j.is_discarded()) {
            Emit(AccountEventType::UploadFailed, ErrorOf(r, "The upload was refused."), item.title);
            return;
        }
        const std::string uploadPath = j.value("uploadPath", "");
        auto progress = [this, &item](float p) {
            std::lock_guard<std::mutex> lock(m);
            for (auto& u : uploads)
                if (u.path == item.path) u.progress = p;
        };
        r = Authed(L"PUT", uploadPath, "", item.path, progress);
        j = nlohmann::json::parse(r.body, nullptr, false);
        if (r.status != 200 || j.is_discarded()) {
            Emit(AccountEventType::UploadFailed, ErrorOf(r, "The upload didn't finish."), item.title);
            return;
        }
        Emit(AccountEventType::Uploaded, j.value("url", ""), item.title);
        LoadClips();
    }
};

Account::Account() : d_(std::make_unique<Impl>()) {}

Account::~Account()
{
    d_->quit = true;
    d_->cancelSignIn = true;
    d_->cv.notify_all();
    if (d_->signInThread.joinable()) d_->signInThread.join();
    if (d_->worker.joinable()) d_->worker.join();
    if (d_->session) WinHttpCloseHandle(d_->session);
}

bool Account::Enabled() { return !g_server.empty(); }

void Account::SetServer(const std::string& url)
{
    std::string u = url;
    while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
    if (!u.empty() && u.rfind("https://", 0) != 0 && u.rfind("http://127.0.0.1", 0) != 0) {
        Log("Account server must be https: %s", u.c_str());
        return;
    }
    g_server = u;
}

void Account::Start(const std::wstring& cacheDir)
{
    if (!Enabled()) {
        d_->state = AccountState::Off;
        return;
    }
    d_->cacheDir = cacheDir;
    EnsureDir(cacheDir);
    d_->worker = std::thread([this] { d_->Run(); });
    d_->Post([this] {
        d_->refresh = d_->LoadRefresh();
        if (d_->refresh.empty()) return;
        {
            std::lock_guard<std::mutex> lock(d_->m);
            d_->state = AccountState::SigningIn;
        }
        if (d_->Refresh()) d_->LoadClips();
        else {
            std::lock_guard<std::mutex> lock(d_->m);
            // Offline: keep the saved session for next time, but show signed out for now.
            if (d_->state == AccountState::SigningIn) d_->state = AccountState::SignedOut;
        }
    });
}

AccountState Account::State() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->state;
}

AccountUser Account::User() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->user;
}

std::wstring Account::AvatarPath() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->avatarPath;
}

void Account::SignIn(const std::string& username, const std::string& password, const std::string& inviteCode)
{
    if (!Enabled()) return;
    {
        std::lock_guard<std::mutex> lock(d_->m);
        if (d_->state == AccountState::SigningIn) return;
        d_->state = AccountState::SigningIn;
    }
    d_->Post([this, username, password, inviteCode] { d_->SignInFlow(username, password, inviteCode); });
}

void Account::CancelSignIn() { d_->cancelSignIn = true; }

void Account::SignOut()
{
    d_->Post([this] {
        if (!d_->refresh.empty()) {
            nlohmann::json b;
            b["refreshToken"] = d_->refresh;
            d_->Request(L"POST", Impl::Server("/auth/logout"), b.dump(), false);
        }
        d_->ClearLocal();
        d_->Emit(AccountEventType::SignedOut, "Signed out");
    });
}

void Account::RefreshClips()
{
    d_->Post([this] { d_->LoadClips(); });
}

std::vector<CloudClip> Account::Clips() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->clips;
}

bool Account::ClipsLoading() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return d_->clipsLoading;
}

void Account::SetPrivacy(const std::string& id, const std::string& privacy)
{
    {
        std::lock_guard<std::mutex> lock(d_->m);
        for (auto& c : d_->clips)
            if (c.id == id) c.privacy = privacy;
    }
    d_->Post([this, id, privacy] {
        nlohmann::json b;
        b["privacy"] = privacy;
        HttpResult r = d_->Authed(L"PATCH", "/clips/" + id, b.dump());
        if (r.status != 200) {
            d_->Emit(AccountEventType::Error, Impl::ErrorOf(r, "Couldn't change who can see it."));
            d_->LoadClips();
        }
    });
}

void Account::DeleteClip(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(d_->m);
        d_->clips.erase(std::remove_if(d_->clips.begin(), d_->clips.end(), [&](const CloudClip& c) { return c.id == id; }), d_->clips.end());
    }
    d_->Post([this, id] {
        HttpResult r = d_->Authed(L"DELETE", "/clips/" + id, "");
        if (r.status != 200) d_->Emit(AccountEventType::Error, Impl::ErrorOf(r, "Couldn't delete the upload."));
        d_->LoadClips();
    });
}

void Account::Upload(const std::wstring& path, const std::string& title, double duration, const std::string& privacy)
{
    UploadItem item;
    item.path = path;
    item.title = title;
    {
        std::lock_guard<std::mutex> lock(d_->m);
        for (const auto& u : d_->uploads)
            if (u.path == path) return;   // already queued
        d_->uploads.push_back(item);
    }
    d_->Post([this, item, duration, privacy] {
        {
            std::lock_guard<std::mutex> lock(d_->m);
            for (auto& u : d_->uploads)
                if (u.path == item.path) u.active = true;
        }
        d_->DoUpload(item, duration, privacy);
        std::lock_guard<std::mutex> lock(d_->m);
        for (auto it = d_->uploads.begin(); it != d_->uploads.end(); ++it)
            if (it->path == item.path) {
                d_->uploads.erase(it);
                break;
            }
    });
}

std::vector<UploadItem> Account::Uploads() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return std::vector<UploadItem>(d_->uploads.begin(), d_->uploads.end());
}

bool Account::PollEvent(AccountEvent& out)
{
    std::lock_guard<std::mutex> lock(d_->m);
    if (d_->events.empty()) return false;
    out = d_->events.front();
    d_->events.pop_front();
    return true;
}

} // namespace cs
