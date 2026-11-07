#include "core/Platform.h"
#include "core/Util.h"
#include "AppInfo.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>
#include <atomic>
#include <thread>
#include <random>

using Microsoft::WRL::ComPtr;

namespace cs::platform {

void OpenPath(const std::wstring& path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenUrl(const std::wstring& url)
{
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInExplorer(const std::wstring& path)
{
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    } else {
        OpenPath(ParentDir(path));
    }
}

static bool RunFileDialog(void* owner, std::wstring& inOut, bool folders, const wchar_t* filterName, const wchar_t* filterSpec)
{
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folders) opts |= FOS_PICKFOLDERS;
    dlg->SetOptions(opts);
    if (!folders && filterName && filterSpec) {
        COMDLG_FILTERSPEC spec[2] = { { filterName, filterSpec }, { L"All files", L"*.*" } };
        dlg->SetFileTypes(2, spec);
    }
    std::wstring start = folders ? inOut : ParentDir(inOut);
    if (!start.empty() && DirExists(start)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dlg->SetFolder(folder.Get());
    }
    if (FAILED(dlg->Show((HWND)owner))) return false;   // cancelled
    ComPtr<IShellItem> result;
    if (FAILED(dlg->GetResult(&result))) return false;
    PWSTR p = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    inOut = p;
    CoTaskMemFree(p);
    return true;
}

bool PickFolder(void* ownerHwnd, std::wstring& inOut)
{
    return RunFileDialog(ownerHwnd, inOut, true, nullptr, nullptr);
}

bool PickFile(void* ownerHwnd, std::wstring& inOut, const wchar_t* filterName, const wchar_t* filterSpec)
{
    return RunFileDialog(ownerHwnd, inOut, false, filterName, filterSpec);
}

bool RecycleFile(const std::wstring& path)
{
    std::wstring from = path;
    from.push_back(L'\0');   // SHFileOperation wants a double-null-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool RenameFile(const std::wstring& from, const std::wstring& to)
{
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED) != FALSE;
}

bool DeleteFileNow(const std::wstring& path)
{
    return DeleteFileW(path.c_str()) != FALSE;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool SetStartWithWindows(bool enable)
{
    if (!enable) {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, CS_APP_NAME_W);
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
    std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --minimized";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, CS_APP_NAME_W, REG_SZ, cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool IsStartWithWindowsEnabled()
{
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, CS_APP_NAME_W, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

static std::wstring VersionString(const std::wstring& exePath, const wchar_t* field)
{
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, size, data.data())) return {};
    struct LangCp { WORD lang, cp; }* trans = nullptr;
    UINT transLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&trans, &transLen) || transLen < sizeof(LangCp))
        return {};
    wchar_t query[128];
    swprintf(query, 128, L"\\StringFileInfo\\%04x%04x\\%ls", trans[0].lang, trans[0].cp, field);
    wchar_t* value = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), query, (void**)&value, &len) || !value || len == 0) return {};
    return std::wstring(value);
}

static std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::wstring ForegroundAppName(void* ownHwnd)
{
    HWND fg = GetForegroundWindow();
    if (!fg || fg == (HWND)ownHwnd || GetAncestor(fg, GA_ROOTOWNER) == (HWND)ownHwnd)
        return L"Desktop";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"Desktop";
    wchar_t buf[MAX_PATH * 2];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring exe;
    if (QueryFullProcessImageNameW(proc, 0, buf, &n)) exe.assign(buf, n);
    CloseHandle(proc);
    if (exe.empty()) return L"Desktop";

    std::wstring stem = FileStem(exe);
    std::wstring lower = stem;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    static const wchar_t* shell[] = { L"explorer", L"shellexperiencehost", L"searchhost", L"startmenuexperiencehost",
                                      L"applicationframehost", L"lockapp", L"textinputhost", L"searchapp" };
    for (auto* s : shell)
        if (lower == s) return L"Desktop";

    // Java games (e.g. Minecraft): the window title is the useful part.
    if (lower == L"javaw" || lower == L"java") {
        wchar_t title[256];
        int len = GetWindowTextW(fg, title, 256);
        std::wstring t(title, (size_t)std::max(0, len));
        size_t cut = t.find_first_of(L"0123456789-:|*");
        if (cut != std::wstring::npos) t.resize(cut);
        t = Trim(t);
        if (!t.empty()) return t;
    }

    std::wstring product = Trim(VersionString(exe, L"ProductName"));
    if (!product.empty() && product.size() <= 40 && product.find(L"Windows") == std::wstring::npos &&
        product.find(L"Java") == std::wstring::npos && product.find(L"Unreal") == std::wstring::npos)
        return product;
    return stem;
}

uint32_t WindowsBuildNumber()
{
    static uint32_t cached = 0;
    if (cached) return cached;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0) cached = vi.dwBuildNumber;
    return cached;
}

uint64_t DiskFreeBytes(const std::wstring& path)
{
    std::wstring p = path;
    while (!p.empty() && !DirExists(p)) p = ParentDir(p);
    if (p.empty()) return 0;
    ULARGE_INTEGER freeToCaller{};
    if (!GetDiskFreeSpaceExW(p.c_str(), &freeToCaller, nullptr, nullptr)) return 0;
    return freeToCaller.QuadPart;
}

bool IsKeyDown(uint32_t vk)
{
    return vk != 0 && (GetAsyncKeyState((int)vk) & 0x8000) != 0;
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM data)
{
    auto* list = reinterpret_cast<std::vector<MonitorDesc>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        MonitorDesc d;
        d.x = mi.rcMonitor.left;
        d.y = mi.rcMonitor.top;
        d.width = mi.rcMonitor.right - mi.rcMonitor.left;
        d.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        list->push_back(d);
    }
    return TRUE;
}

std::vector<MonitorDesc> EnumerateMonitors()
{
    std::vector<MonitorDesc> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, (LPARAM)&list);
    // Primary first, then left-to-right.
    std::stable_sort(list.begin(), list.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        if (a.primary != b.primary) return a.primary;
        return a.x < b.x;
    });
    for (size_t i = 0; i < list.size(); ++i) {
        char buf[96];
        snprintf(buf, sizeof(buf), "Display %d  (%dx%d)%s", (int)i + 1, list[i].width, list[i].height,
                 list[i].primary ? "  primary" : "");
        list[i].name = buf;
    }
    return list;
}

void PrimaryWorkArea(int& x, int& y, int& w, int& h)
{
    RECT r{ 0, 0, 1920, 1080 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    x = r.left;
    y = r.top;
    w = r.right - r.left;
    h = r.bottom - r.top;
}

float SystemDpiScale()
{
    return (float)GetDpiForSystem() / 96.0f;
}

bool RectOnAnyMonitor(int x, int y, int w, int h)
{
    RECT r{ x, y, x + w, y + h };
    // Require a reasonable chunk of the title area to be visible somewhere.
    RECT title{ x + 40, y, x + w - 40, y + 40 };
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr && MonitorFromRect(&title, MONITOR_DEFAULTTONULL) != nullptr;
}

void ShowErrorBox(const std::wstring& text)
{
    MessageBoxW(nullptr, text.c_str(), CS_APP_NAME_W, MB_OK | MB_ICONERROR);
}

bool CommandLineHas(const wchar_t* flag)
{
    const wchar_t* cmd = GetCommandLineW();
    return cmd && wcsstr(cmd, flag) != nullptr;
}

// ---- Sound cues: tiny WAVs synthesized once, played asynchronously from memory ----

static std::vector<unsigned char> MakeWav(const std::vector<short>& samples, int rate)
{
    std::vector<unsigned char> out(44 + samples.size() * 2);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&out[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&out[at], &v, 2); };
    memcpy(&out[0], "RIFF", 4);
    put32(4, (uint32_t)(36 + samples.size() * 2));
    memcpy(&out[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);              // PCM
    put16(22, 1);              // mono
    put32(24, (uint32_t)rate);
    put32(28, (uint32_t)rate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&out[36], "data", 4);
    put32(40, (uint32_t)(samples.size() * 2));
    memcpy(&out[44], samples.data(), samples.size() * 2);
    return out;
}

static std::vector<unsigned char> SynthTones(std::initializer_list<std::pair<float, float>> tones, float gap = 0.02f)
{
    const int rate = 44100;
    std::vector<short> s;
    for (auto& t : tones) {
        int n = (int)(t.second * rate);
        for (int i = 0; i < n; ++i) {
            float x = (float)i / rate;
            float env = std::min(1.0f, x / 0.005f) * std::exp(-x * 9.0f);
            float v = std::sin(6.2831853f * t.first * x) * 0.6f + std::sin(6.2831853f * t.first * 2.0f * x) * 0.15f;
            s.push_back((short)(v * env * 0.32f * 32767.0f));
        }
        for (int i = 0; i < (int)(gap * rate); ++i) s.push_back(0);
    }
    return MakeWav(s, rate);
}

static std::vector<unsigned char> SynthShutter()
{
    const int rate = 44100;
    std::vector<short> s;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (int burst = 0; burst < 2; ++burst) {
        for (int i = 0; i < (int)(0.045f * rate); ++i) {
            float x = (float)i / rate;
            float env = std::exp(-x * 90.0f);
            float v = noise(rng) * 0.5f + std::sin(6.2831853f * 2400.0f * x) * 0.3f;
            s.push_back((short)(v * env * 0.35f * 32767.0f));
        }
        for (int i = 0; i < (int)(0.035f * rate); ++i) s.push_back(0);
    }
    return MakeWav(s, rate);
}

// "Clip saved": a soft rising whoosh into a two-note bell chime (E6 -> B6) with a short airy tail.
static std::vector<unsigned char> SynthClipChime()
{
    const int rate = 44100;
    const int n = (int)(1.1f * rate);
    std::vector<float> buf((size_t)n, 0.0f);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    // Whoosh: noise through a resonant low-pass whose cutoff sweeps up.
    float lp = 0.0f, bp = 0.0f;
    const int whooshLen = (int)(0.17f * rate);
    for (int i = 0; i < whooshLen; ++i) {
        const float t = (float)i / whooshLen;
        const float cutoff = 900.0f + 5200.0f * t * t;
        const float f = 2.0f * std::sin(3.14159265f * cutoff / rate);
        const float in = noise(rng);
        lp += f * bp;
        const float hp = in - lp - 0.6f * bp;
        bp += f * hp;
        const float env = std::sin(3.14159265f * std::min(1.0f, t * 1.15f)) * (1.0f - t * 0.3f);
        buf[(size_t)i] += bp * env * 0.16f;
    }
    // Bell notes: a few partials (one slightly inharmonic for shimmer), each with its own decay.
    auto bell = [&](float freq, float start, float level) {
        const struct { float mul, amp, decay; } partials[] = { { 1.0f, 1.0f, 4.2f }, { 2.0f, 0.38f, 7.0f }, { 3.0f, 0.12f, 10.0f }, { 4.16f, 0.09f, 13.0f } };
        const int s0 = (int)(start * rate);
        for (int i = s0; i < n; ++i) {
            const float t = (float)(i - s0) / rate;
            const float attack = std::min(1.0f, t / 0.003f);
            float v = 0.0f;
            for (const auto& p : partials) v += std::sin(6.2831853f * freq * p.mul * t) * p.amp * std::exp(-t * p.decay);
            buf[(size_t)i] += v * attack * level;
        }
    };
    bell(1318.5f, 0.10f, 0.30f);   // E6
    bell(1975.5f, 0.19f, 0.26f);   // B6
    bell(2637.0f, 0.19f, 0.06f);   // E7, a soft octave on top
    // Airy tail: three short feedback delays.
    std::vector<float> wet((size_t)n, 0.0f);
    const int delays[] = { (int)(0.0297f * rate), (int)(0.0371f * rate), (int)(0.0411f * rate) };
    for (int d : delays) {
        std::vector<float> line((size_t)n, 0.0f);
        for (int i = 0; i < n; ++i) line[(size_t)i] = buf[(size_t)i] + (i >= d ? line[(size_t)(i - d)] * 0.42f : 0.0f);
        for (int i = 0; i < n; ++i) wet[(size_t)i] += line[(size_t)i] / 3.0f;
    }
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        buf[(size_t)i] = buf[(size_t)i] * 0.8f + wet[(size_t)i] * 0.28f;
        peak = std::max(peak, std::fabs(buf[(size_t)i]));
    }
    const float gain = peak > 0.0f ? 0.5f / peak : 1.0f;
    std::vector<short> out((size_t)n);
    for (int i = 0; i < n; ++i) {
        const float fadeOut = i > n - rate / 10 ? (float)(n - i) / (rate / 10) : 1.0f;
        out[(size_t)i] = (short)std::lround(std::clamp(buf[(size_t)i] * gain * fadeOut, -1.0f, 1.0f) * 32767.0f);
    }
    return MakeWav(out, rate);
}

// Quiet UI sounds on their own waveOut stream, so they never cut off a cue played with PlaySound.
static std::vector<short> SynthTick(float freq, float seconds, float amp)
{
    const int rate = 44100;
    std::vector<short> s((size_t)(seconds * rate));
    for (size_t i = 0; i < s.size(); ++i) {
        const float x = (float)i / rate;
        const float env = std::min(1.0f, x / 0.002f) * std::exp(-x * 70.0f);
        const float v = std::sin(6.2831853f * freq * x) * 0.7f + std::sin(6.2831853f * freq * 1.5f * x) * 0.3f;
        s[i] = (short)(v * env * amp * 32767.0f);
    }
    return s;
}

void PlayUiSound(int kind)
{
    static const std::vector<short> hover = SynthTick(2900.0f, 0.035f, 0.035f);
    static const std::vector<short> click = SynthTick(1500.0f, 0.06f, 0.08f);
    static std::atomic<int64_t> last { 0 };
    const int64_t now = (int64_t)GetTickCount64();
    if (kind == 0 && now - last.load() < 70) return;   // a fast sweep over many buttons shouldn't buzz
    last = now;
    const std::vector<short>* pcm = kind ? &click : &hover;
    std::thread([pcm] {
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = 44100;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2;
        wf.nAvgBytesPerSec = 88200;
        HWAVEOUT h = nullptr;
        if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
        WAVEHDR hdr{};
        hdr.lpData = (LPSTR)pcm->data();
        hdr.dwBufferLength = (DWORD)(pcm->size() * sizeof(short));
        if (waveOutPrepareHeader(h, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
            if (waveOutWrite(h, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR)
                for (int i = 0; i < 100 && !(hdr.dwFlags & WHDR_DONE); ++i) Sleep(5);
            waveOutReset(h);
            waveOutUnprepareHeader(h, &hdr, sizeof(hdr));
        }
        waveOutClose(h);
    }).detach();
}

void PlaySoundCue(Sound which)
{
    static const std::vector<unsigned char> clip = SynthClipChime();
    static const std::vector<unsigned char> recStart = SynthTones({ { 659.3f, 0.08f }, { 987.8f, 0.14f } });
    static const std::vector<unsigned char> recStop = SynthTones({ { 987.8f, 0.08f }, { 659.3f, 0.16f } });
    static const std::vector<unsigned char> shot = SynthShutter();
    static const std::vector<unsigned char> err = SynthTones({ { 311.1f, 0.12f }, { 233.1f, 0.2f } });
    const std::vector<unsigned char>* data = &clip;
    switch (which) {
    case Sound::ClipSaved: data = &clip; break;
    case Sound::RecordStart: data = &recStart; break;
    case Sound::RecordStop: data = &recStop; break;
    case Sound::Screenshot: data = &shot; break;
    case Sound::Error: data = &err; break;
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(data->data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ---- clipboard ---------------------------------------------------------------------------

bool CopyTextToClipboard(void* ownerHwnd, const std::string& utf8)
{
    const std::wstring w = ToWide(utf8);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (!mem) return false;
    memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    if (!OpenClipboard((HWND)ownerHwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

bool CopyFilesToClipboard(void* ownerHwnd, const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return false;
    // CF_HDROP: DROPFILES header followed by double-NUL-terminated wide paths.
    size_t chars = 1;
    for (const auto& p : paths) chars += p.size() + 1;
    const size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!mem) return false;
    auto* df = (DROPFILES*)GlobalLock(mem);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
    for (const auto& p : paths) {
        memcpy(dst, p.c_str(), p.size() * sizeof(wchar_t));
        dst += p.size() + 1;
    }
    GlobalUnlock(mem);
    if (!OpenClipboard((HWND)ownerHwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

bool CopyImageToClipboard(void* ownerHwnd, const std::wstring& imagePath)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0) return false;
    const size_t stride = (size_t)w * 4, pixels = stride * h;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels);
    if (!mem) return false;
    auto* bih = (BITMAPINFOHEADER*)GlobalLock(mem);
    *bih = BITMAPINFOHEADER{};
    bih->biSize = sizeof(BITMAPINFOHEADER);
    bih->biWidth = (LONG)w;
    bih->biHeight = (LONG)h;   // bottom-up
    bih->biPlanes = 1;
    bih->biBitCount = 32;
    bih->biCompression = BI_RGB;
    BYTE* bits = (BYTE*)(bih + 1);
    std::vector<BYTE> top(pixels);
    const bool read = SUCCEEDED(conv->CopyPixels(nullptr, (UINT)stride, (UINT)pixels, top.data()));
    for (UINT y = 0; read && y < h; ++y) memcpy(bits + (size_t)(h - 1 - y) * stride, top.data() + (size_t)y * stride, stride);
    GlobalUnlock(mem);
    if (!read || !OpenClipboard((HWND)ownerHwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

void ExcludeFromCapture(void* hwnd, bool exclude)
{
    if (hwnd) SetWindowDisplayAffinity((HWND)hwnd, exclude ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : 0 /* WDA_NONE */);
}

const void* EmbeddedResource(const wchar_t* name, size_t* size)
{
    HRSRC r = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return nullptr;
    if (size) *size = SizeofResource(nullptr, r);
    return LockResource(g);
}

void AttachConsoleWindow()
{
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

} // namespace cs::platform
