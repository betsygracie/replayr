#include "core/Tray.h"
#include "core/Window.h"
#include "resource.h"

#include <windows.h>
#include <shellapi.h>

#include <cwchar>

namespace cs {

static const UINT kTrayId = 1;

bool Tray::Create(void* hwnd, void* hinstance, const std::wstring& tooltip)
{
    hwnd_ = hwnd;
    hinst_ = hinstance;
    tooltip_ = tooltip;
    if (!icon_)
        icon_ = LoadImageW((HINSTANCE)hinst_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMessage;
    nid.hIcon = (HICON)icon_;
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    return added_;
}

void Tray::Remove()
{
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void Tray::Recreate()
{
    added_ = false;
    Create(hwnd_, hinst_, tooltip_);
}

void Tray::SetTooltip(const std::wstring& tooltip)
{
    if (tooltip == tooltip_) return;
    tooltip_ = tooltip;
    if (!added_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = (HWND)hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tooltip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

TrayCommand Tray::ShowMenu(const TrayMenuState& state)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return TrayCommand::None;
    AppendMenuW(menu, MF_STRING | (state.bufferOn ? 0 : MF_GRAYED), (UINT_PTR)TrayCommand::SaveReplay, L"Save replay");
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::ToggleRecording, state.recording ? L"Stop recording" : L"Start recording");
    AppendMenuW(menu, MF_STRING | (state.bufferOn ? MF_CHECKED : 0), (UINT_PTR)TrayCommand::ToggleReplayBuffer, L"Replay buffer");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::Screenshot, L"Take screenshot");
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::ScreenshotRegion, L"Screenshot region...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::OpenApp, L"Open Replayr");
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::OpenFolder, L"Open clips folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, (UINT_PTR)TrayCommand::Quit, L"Quit");
    SetMenuDefaultItem(menu, (UINT)TrayCommand::OpenApp, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow((HWND)hwnd_);
    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, (HWND)hwnd_, nullptr);
    PostMessageW((HWND)hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return (TrayCommand)cmd;
}

} // namespace cs
