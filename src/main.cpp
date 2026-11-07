// Replayr - game clipping and screenshot app.
// Entry point: DPI awareness, single instance, COM, then hand over to ui::App.

#include "ui/App.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "AppInfo.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <objbase.h>

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int)
{
    // Per-monitor v2: crisp at 100-300% scaling and correct when dragged between monitors.
    // (The manifest sets this too; this call covers builds without it.)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One instance: a second launch just brings the running one to the front.
    // Renamed from "Clipstream": move its settings, library data and clips folder over once,
    // and drop its old start-with-Windows entry (the new one is written from the settings).
    {
        auto moveOld = [](REFKNOWNFOLDERID folder) {
            PWSTR base = nullptr;
            if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &base))) return;
            const std::wstring from = std::wstring(base) + L"\\Clipstream", to = std::wstring(base) + L"\\" CS_APP_NAME_W;
            CoTaskMemFree(base);
            if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES)
                MoveFileExW(from.c_str(), to.c_str(), 0);
        };
        moveOld(FOLDERID_RoamingAppData);
        moveOld(FOLDERID_LocalAppData);
        moveOld(FOLDERID_Videos);
        RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Clipstream");
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, CS_MUTEX_NAME);
    bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning && mutex && cs::platform::CommandLineHas(L"--updated")) {
        // Just updated: the old version is still closing. Wait for it (up to 10 s) instead of quitting.
        const DWORD w = WaitForSingleObject(mutex, 10000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) alreadyRunning = false;
    }
    if (alreadyRunning) {
        if (HWND other = FindWindowW(CS_WINDOW_CLASS, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, RegisterWindowMessageW(CS_SHOW_MESSAGE), 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    const bool startHidden = cs::platform::CommandLineHas(L"--minimized");
#ifndef NDEBUG
    // Debug builds only: a console for live logs.
    if (cs::platform::CommandLineHas(L"--console")) {
        cs::platform::AttachConsoleWindow();
        cs::LogToStdout(true);
    }
#endif
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int code = 1;
    {
        cs::ui::App app;
        if (app.Init(hinst, startHidden)) code = app.Run();
        app.Shutdown();
    }

    if (SUCCEEDED(co)) CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
