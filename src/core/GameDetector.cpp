#include "core/GameDetector.h"
#include "core/Log.h"
#include "core/Util.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

// Popular games by exe name, for installs no launcher scan finds (or before the scan is done).
struct Known {
    const wchar_t* exe;
    const char* name;
};
const Known kKnown[] = {
    { L"valorant-win64-shipping.exe", "Valorant" }, { L"league of legends.exe", "League of Legends" }, { L"cs2.exe", "Counter-Strike 2" },
    { L"fortniteclient-win64-shipping.exe", "Fortnite" }, { L"r5apex.exe", "Apex Legends" }, { L"r5apex_dx12.exe", "Apex Legends" },
    { L"robloxplayerbeta.exe", "Roblox" }, { L"gta5.exe", "Grand Theft Auto V" }, { L"gta5_enhanced.exe", "Grand Theft Auto V" },
    { L"rocketleague.exe", "Rocket League" }, { L"overwatch.exe", "Overwatch 2" }, { L"cod.exe", "Call of Duty" },
    { L"destiny2.exe", "Destiny 2" }, { L"eldenring.exe", "Elden Ring" }, { L"rainbowsix.exe", "Rainbow Six Siege" },
    { L"rainbowsix_vulkan.exe", "Rainbow Six Siege" }, { L"dota2.exe", "Dota 2" }, { L"tslgame.exe", "PUBG: Battlegrounds" },
    { L"warframe.x64.exe", "Warframe" }, { L"escapefromtarkov.exe", "Escape from Tarkov" }, { L"rustclient.exe", "Rust" },
    { L"palworld-win64-shipping.exe", "Palworld" }, { L"cyberpunk2077.exe", "Cyberpunk 2077" }, { L"bf2042.exe", "Battlefield 2042" },
    { L"diablo iv.exe", "Diablo IV" }, { L"fallout76.exe", "Fallout 76" }, { L"marvel-win64-shipping.exe", "Marvel Rivals" },
    { L"project8.exe", "Deadlock" }, { L"helldivers2.exe", "Helldivers 2" }, { L"monsterhunterwilds.exe", "Monster Hunter Wilds" },
    { L"minecraft.windows.exe", "Minecraft" }, { L"javaw.exe", "" },   // "" = never a game on its own (Minecraft Java needs a window title)
    { L"thefinals.exe", "The Finals" }, { L"deadbydaylight-win64-shipping.exe", "Dead by Daylight" }, { L"bg3.exe", "Baldur's Gate 3" },
    { L"bg3_dx11.exe", "Baldur's Gate 3" }, { L"starfield.exe", "Starfield" }, { L"hogwartslegacy.exe", "Hogwarts Legacy" },
    { L"rdr2.exe", "Red Dead Redemption 2" }, { L"forzahorizon5.exe", "Forza Horizon 5" }, { L"seaofthieves.exe", "Sea of Thieves" },
    { L"fifa23.exe", "EA Sports FC" }, { L"fc25.exe", "EA Sports FC 25" }, { L"fc26.exe", "EA Sports FC 26" }, { L"wow.exe", "World of Warcraft" },
    { L"hearthstone.exe", "Hearthstone" }, { L"osu!.exe", "osu!" }, { L"terraria.exe", "Terraria" }, { L"stardew valley.exe", "Stardew Valley" },
};

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::wstring FileName(const std::wstring& p)
{
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring NormDir(std::wstring d)
{
    for (wchar_t& c : d)
        if (c == L'/') c = L'\\';
    while (!d.empty() && d.back() == L'\\') d.pop_back();
    return Lower(d) + L"\\";
}

std::string ReadText(const std::wstring& path) { return ReadTextFile(path); }

std::wstring RegString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view = 0)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    // REG_SZ, or REG_EXPAND_SZ taken as-is (asking for both with expansion on is an invalid call).
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | view, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return L"";
}

bool IsHelperExe(const std::wstring& lowerName)
{
    static const wchar_t* bad[] = { L"unins", L"crash", L"setup", L"redist", L"vc_", L"vcredist", L"dxsetup", L"prereq", L"easyanticheat",
                                    L"battleye", L"beservice", L"launcher", L"updater", L"report", L"helper", L"install", L"dotnet",
                                    L"cefprocess", L"webhelper", L"benchmark", L"config", L"touchup", L"cleanup" };
    for (const wchar_t* b : bad)
        if (lowerName.find(b) != std::wstring::npos) return true;
    return false;
}

// The biggest exe in a folder (two levels deep): the usual "main" exe, used for the icon.
std::wstring GuessExe(const std::wstring& dir, int depth = 0)
{
    std::wstring best;
    uint64_t bestSize = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    std::vector<std::wstring> subdirs;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 3) subdirs.push_back(dir + L"\\" + n);
            continue;
        }
        const std::wstring ln = Lower(n);
        if (ln.size() < 5 || ln.compare(ln.size() - 4, 4, L".exe") != 0 || IsHelperExe(ln)) continue;
        const uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size > bestSize) {
            bestSize = size;
            best = dir + L"\\" + n;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty() || depth == 0)
        for (const std::wstring& sd : subdirs) {
            const std::wstring c = GuessExe(sd, depth + 1);
            if (c.empty()) continue;
            WIN32_FILE_ATTRIBUTE_DATA ad{};
            if (GetFileAttributesExW(c.c_str(), GetFileExInfoStandard, &ad)) {
                const uint64_t size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
                if (size > bestSize) {
                    bestSize = size;
                    best = c;
                }
            }
        }
    return best;
}

// Saves the exe's icon (64 px, with transparency) as a PNG. Returns false if it has none.
bool SaveIconPng(const std::wstring& exe, const std::wstring& png)
{
    HICON icon = nullptr;
    if (PrivateExtractIconsW(exe.c_str(), 0, 64, 64, &icon, nullptr, 1, 0) == 0 || !icon) {
        if (ExtractIconExW(exe.c_str(), 0, &icon, nullptr, 1) == 0 || !icon) return false;
    }
    ICONINFO ii{};
    bool ok = false;
    if (GetIconInfo(icon, &ii) && ii.hbmColor) {
        BITMAP bm{};
        GetObjectW(ii.hbmColor, sizeof(bm), &bm);
        const int w = bm.bmWidth, h = bm.bmHeight;
        std::vector<uint8_t> px((size_t)w * h * 4), mask((size_t)w * h * 4);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        bool anyAlpha = false;
        for (size_t i = 3; i < px.size(); i += 4) anyAlpha = anyAlpha || px[i] != 0;
        if (!anyAlpha && ii.hbmMask) {   // old icons: transparency comes from the mask
            GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
            for (size_t i = 0; i < px.size(); i += 4) px[i + 3] = mask[i] ? 0 : 255;
        }
        ReleaseDC(nullptr, dc);
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)px.size(), px.data())) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    return ok;
}

std::string Utf8(const std::wstring& w) { return ToUtf8(w); }

} // namespace

std::string GameDetector::NameFromExe(const std::wstring& exe)
{
    std::wstring n = FileName(exe);
    const std::wstring ln = Lower(n);
    for (const Known& k : kKnown)
        if (ln == k.exe && *k.name) return k.name;
    if (ln.size() > 4 && ln.compare(ln.size() - 4, 4, L".exe") == 0) n = n.substr(0, n.size() - 4);
    for (const wchar_t* suffix : { L"-Win64-Shipping", L"_x64", L"-win64", L"64" }) {
        const std::wstring sfx = suffix;
        if (n.size() > sfx.size() && Lower(n.substr(n.size() - sfx.size())) == Lower(sfx)) n = n.substr(0, n.size() - sfx.size());
    }
    return Utf8(n);
}

GameDetector::~GameDetector() { Stop(); }

void GameDetector::Start(const std::wstring& cacheDir)
{
    Stop();
    cacheDir_ = cacheDir;
    quit_ = false;
    rescan_ = true;
    thread_ = std::thread([this] { Run(); });
}

void GameDetector::Stop()
{
    quit_ = true;
    if (thread_.joinable()) thread_.join();
}

void GameDetector::Rescan() { rescan_ = true; }

void GameDetector::SetManual(const std::vector<GameInfo>& games)
{
    std::lock_guard<std::mutex> lock(m_);
    manual_ = games;
}

std::vector<GameInfo> GameDetector::Games() const
{
    std::lock_guard<std::mutex> lock(m_);
    std::vector<GameInfo> all = manual_;
    std::set<std::string> ids;
    for (const GameInfo& g : all) ids.insert(g.id);
    for (const GameInfo& g : scanned_)
        if (ids.insert(g.id).second) all.push_back(g);
    std::sort(all.begin(), all.end(), [](const GameInfo& a, const GameInfo& b) { return ToLowerAscii(a.name) < ToLowerAscii(b.name); });
    return all;
}

GameInfo GameDetector::Active() const
{
    std::lock_guard<std::mutex> lock(m_);
    return active_;
}

bool GameDetector::PollEvent(GameEvent& out)
{
    std::lock_guard<std::mutex> lock(m_);
    if (events_.empty()) return false;
    out = events_.front();
    events_.pop_front();
    return true;
}

void GameDetector::Run()
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!quit_) {
        if (rescan_.exchange(false)) {
            scanning_ = true;
            ScanLibraries();
            scanning_ = false;
        }
        Watch();
        // Icons: a few per pass, so a big library doesn't hold anything up.
        int budget = 6;
        std::vector<GameInfo> list;
        {
            std::lock_guard<std::mutex> lock(m_);
            list = scanned_;
            list.insert(list.end(), manual_.begin(), manual_.end());
        }
        for (GameInfo& g : list) {
            if (!g.icon.empty() || budget <= 0) continue;
            const std::string icon = IconFor(g);
            --budget;
            std::lock_guard<std::mutex> lock(m_);
            for (auto* v : { &scanned_, &manual_ })
                for (GameInfo& x : *v)
                    if (x.id == g.id) x.icon = ToWide(icon);
        }
        for (int i = 0; i < 25 && !quit_ && !rescan_; ++i) Sleep(100);   // 2.5 s
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

std::string GameDetector::IconFor(GameInfo& g)
{
    const std::wstring dir = JoinPath(cacheDir_, L"GameIcons");
    EnsureDir(dir);
    std::wstring safe;
    for (char c : g.id) safe += (std::isalnum((unsigned char)c) ? (wchar_t)c : L'_');
    const std::wstring png = JoinPath(dir, safe + L".png");
    if (FileExists(png)) return ToUtf8(png);
    std::wstring exe = g.exe;
    if (exe.empty() && !g.installDir.empty()) exe = GuessExe(g.installDir);
    if (exe.empty() || !SaveIconPng(exe, png)) return std::string(" ");   // " ": tried, no icon (don't retry every pass)
    return ToUtf8(png);
}

void GameDetector::ScanLibraries()
{
    std::vector<GameInfo> found;
    std::set<std::wstring> dirs;
    auto add = [&](GameInfo g) {
        if (g.name.empty()) return;
        if (!g.installDir.empty()) {
            if (!DirExists(g.installDir) || !dirs.insert(NormDir(g.installDir)).second) return;
        }
        found.push_back(std::move(g));
    };

    // Steam: every library folder, every app manifest.
    std::wstring steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!steam.empty()) {
        for (wchar_t& c : steam)
            if (c == L'/') c = L'\\';
        std::vector<std::wstring> libs = { steam };
        const std::string vdf = ReadText(steam + L"\\steamapps\\libraryfolders.vdf");
        static const std::regex pathRe("\"path\"\\s+\"([^\"]+)\"");
        for (std::sregex_iterator it(vdf.begin(), vdf.end(), pathRe), end; it != end; ++it) {
            std::string p = (*it)[1].str();
            std::string unesc;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                unesc += p[i];
            }
            libs.push_back(ToWide(unesc));
        }
        static const std::regex kv("\"(appid|name|installdir)\"\\s+\"([^\"]*)\"");
        std::set<std::wstring> seenLibs;
        for (const std::wstring& lib : libs) {
            if (!seenLibs.insert(NormDir(lib)).second) continue;
            for (const std::wstring& acf : ListFiles(lib + L"\\steamapps", L"appmanifest_*.acf")) {
                const std::string t = ReadText(acf);
                std::map<std::string, std::string> v;
                for (std::sregex_iterator it(t.begin(), t.end(), kv), end; it != end; ++it)
                    if (!v.count((*it)[1].str())) v[(*it)[1].str()] = (*it)[2].str();
                const std::string& name = v["name"];
                if (name.empty() || name.find("Steamworks") != std::string::npos || name.find("Proton") != std::string::npos ||
                    name.find("Steam Linux Runtime") != std::string::npos || name.find("Redistributable") != std::string::npos || name == "SteamVR")
                    continue;
                GameInfo g;
                g.id = "steam:" + v["appid"];
                g.name = name;
                g.launcher = "Steam";
                g.installDir = lib + L"\\steamapps\\common\\" + ToWide(v["installdir"]);
                add(g);
            }
        }
    }

    // Epic: the launcher's manifests.
    wchar_t pd[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    for (const std::wstring& item : ListFiles(std::wstring(pd) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests", L"*.item")) {
        auto j = nlohmann::json::parse(ReadText(item), nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (j.value("bIsIncompleteInstall", false)) continue;
        GameInfo g;
        g.name = j.value("DisplayName", std::string());
        g.id = "epic:" + j.value("AppName", g.name);
        g.launcher = "Epic";
        g.installDir = ToWide(j.value("InstallLocation", std::string()));
        const std::string launch = j.value("LaunchExecutable", std::string());
        if (!launch.empty()) g.exe = g.installDir + L"\\" + ToWide(launch);
        bool isGame = true;
        if (j.contains("AppCategories") && j["AppCategories"].is_array()) {
            isGame = false;
            for (const auto& c : j["AppCategories"])
                if (c.is_string() && c.get<std::string>() == "games") isGame = true;
        }
        if (isGame) add(g);
    }

    // Battle.net / EA / Ubisoft / Riot: their games' uninstall entries.
    struct Pub {
        const wchar_t* publisher;
        const char* launcher;
    };
    static const Pub pubs[] = { { L"Blizzard Entertainment", "Battle.net" }, { L"Electronic Arts", "EA" }, { L"Ubisoft", "Ubisoft" },
                                { L"Riot Games", "Riot" } };
    static const wchar_t* notGames[] = { L"Battle.net", L"EA app", L"Origin", L"Ubisoft Connect", L"Riot Client", L"Riot Vanguard", L"Uplay" };
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER })
        for (const wchar_t* base : { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" }) {
            HKEY k;
            if (RegOpenKeyExW(root, base, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
            wchar_t sub[256];
            for (DWORD i = 0;; ++i) {
                DWORD len = 256;
                if (RegEnumKeyExW(k, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                const std::wstring key = std::wstring(base) + L"\\" + sub;
                const std::wstring pub = RegString(root, key.c_str(), L"Publisher");
                if (pub.empty()) continue;
                const Pub* match = nullptr;
                for (const Pub& p : pubs)
                    if (pub.find(p.publisher) != std::wstring::npos) match = &p;
                if (!match) continue;
                const std::wstring name = RegString(root, key.c_str(), L"DisplayName");
                bool skip = name.empty();
                for (const wchar_t* ng : notGames) skip = skip || name.find(ng) != std::wstring::npos;
                if (skip) continue;
                GameInfo g;
                g.name = Utf8(name);
                g.id = std::string(match->launcher) + ":" + Utf8(sub);
                g.launcher = match->launcher;
                g.installDir = RegString(root, key.c_str(), L"InstallLocation");
                while (!g.installDir.empty() && (g.installDir.back() == L'\\' || g.installDir.back() == L'"')) g.installDir.pop_back();
                if (!g.installDir.empty() && g.installDir.front() == L'"') g.installDir.erase(0, 1);
                if (!g.installDir.empty()) add(g);
            }
            RegCloseKey(k);
        }

    Log("Games: %d installed games found", (int)found.size());
    std::lock_guard<std::mutex> lock(m_);
    // Keep icons already made.
    for (GameInfo& g : found)
        for (const GameInfo& o : scanned_)
            if (o.id == g.id) g.icon = o.icon;
    scanned_ = std::move(found);
}

void GameDetector::Watch()
{
    // Still playing? The game stays active until its process exits (alt-tab / minimise don't end it).
    {
        unsigned long pid;
        {
            std::lock_guard<std::mutex> lock(m_);
            pid = activePid_;
        }
        if (pid) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            DWORD code = 0;
            const bool alive = h && GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            if (h) CloseHandle(h);
            if (alive) return;
            std::lock_guard<std::mutex> lock(m_);
            events_.push_back({ GameEventType::Stopped, active_ });
            Log("Games: %s closed", active_.name.c_str());
            active_ = GameInfo{};
            activePid_ = 0;
        }
    }
    // Large visible windows: candidates. The foreground one first.
    struct Cand {
        HWND w;
        DWORD pid;
    };
    std::vector<Cand> cands;
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            if (!IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER)) return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked) return TRUE;
            RECT r{};
            GetWindowRect(w, &r);
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return TRUE;
            const double area = (double)(r.right - r.left) * (r.bottom - r.top);
            const double mon = (double)(mi.rcMonitor.right - mi.rcMonitor.left) * (mi.rcMonitor.bottom - mi.rcMonitor.top);
            if (mon <= 0 || area < mon * 0.6) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid && pid != GetCurrentProcessId()) ((std::vector<Cand>*)lp)->push_back({ w, pid });
            return TRUE;
        },
        (LPARAM)&cands);
    if (cands.empty()) return;
    HWND fg = GetForegroundWindow();
    std::stable_partition(cands.begin(), cands.end(), [&](const Cand& c) { return c.w == fg; });

    std::vector<GameInfo> games;
    {
        std::lock_guard<std::mutex> lock(m_);
        games = manual_;
        games.insert(games.end(), scanned_.begin(), scanned_.end());
    }
    for (const Cand& c : cands) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.pid);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD len = MAX_PATH * 2;
        const bool got = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
        CloseHandle(h);
        if (!got) continue;
        const std::wstring exe = path, lexe = Lower(exe), lname = Lower(FileName(exe));
        if (IsHelperExe(lname)) continue;
        GameInfo hit;
        for (const GameInfo& g : games) {
            const bool byDir = !g.installDir.empty() && lexe.rfind(NormDir(g.installDir), 0) == 0;
            const bool byExe = !g.exe.empty() && Lower(FileName(g.exe)) == lname;
            if (byDir || byExe) {
                hit = g;
                break;
            }
        }
        if (hit.id.empty())
            for (const Known& k : kKnown)
                if (lname == k.exe && *k.name) {
                    hit.id = ToUtf8(lname);
                    hit.name = k.name;
                    hit.launcher = "Built-in";
                }
        if (hit.id.empty()) continue;
        hit.exe = exe;   // the exe actually running (also the best icon source)
        std::lock_guard<std::mutex> lock(m_);
        active_ = hit;
        activePid_ = c.pid;
        events_.push_back({ GameEventType::Started, hit });
        Log("Games: %s started (%s)", hit.name.c_str(), hit.launcher.c_str());
        return;
    }
}

} // namespace cs
