#include "core/Hotkeys.h"
#include "core/Log.h"
#include "core/Util.h"

#include <windows.h>

namespace cs {

void Hotkeys::Apply(void* hwnd, const std::array<Hotkey, kHotkeyCount>& keys)
{
    UnregisterAll();
    hwnd_ = hwnd;
    keys_ = keys;
    if (!suspended_) RegisterAll();
}

void Hotkeys::RegisterAll()
{
    for (int i = 0; i < kHotkeyCount; ++i) {
        registered_[i] = false;
        const Hotkey& k = keys_[i];
        if (!k.IsSet()) {
            status_[i] = HotkeyStatus::Unbound;
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < i; ++j)
            if (keys_[j] == k) duplicate = true;
        if (duplicate) {
            status_[i] = HotkeyStatus::Duplicate;
            continue;
        }
        if (RegisterHotKey((HWND)hwnd_, IdFor((HotkeyAction)i), k.mods | MOD_NOREPEAT, k.vk)) {
            registered_[i] = true;
            status_[i] = HotkeyStatus::Ok;
        } else {
            status_[i] = HotkeyStatus::InUse;
            Log("Hotkey %s is taken by another app (error %lu)", ToString(k).c_str(), GetLastError());
        }
    }
}

void Hotkeys::UnregisterAll()
{
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (registered_[i]) UnregisterHotKey((HWND)hwnd_, IdFor((HotkeyAction)i));
        registered_[i] = false;
    }
}

void Hotkeys::Suspend(bool suspended)
{
    if (suspended == suspended_) return;
    suspended_ = suspended;
    if (suspended) UnregisterAll();
    else RegisterAll();
}

bool Hotkeys::ActionFromId(int id, HotkeyAction& out)
{
    int i = id - 0x4300;
    if (i < 0 || i >= kHotkeyCount) return false;
    out = (HotkeyAction)i;
    return true;
}

std::string Hotkeys::KeyName(uint32_t vk)
{
    if (vk == 0) return "None";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_TAB: return "Tab";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_SNAPSHOT: return "Print Screen";
    case VK_PAUSE: return "Pause";
    case VK_SCROLL: return "Scroll Lock";
    case VK_CAPITAL: return "Caps Lock";
    case VK_NUMLOCK: return "Num Lock";
    case VK_APPS: return "Menu";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num .";
    case VK_DIVIDE: return "Num /";
    case VK_LBUTTON: return "Left mouse";
    case VK_RBUTTON: return "Right mouse";
    case VK_MBUTTON: return "Middle mouse";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_MEDIA_PLAY_PAUSE: return "Play/Pause";
    case VK_MEDIA_NEXT_TRACK: return "Next track";
    case VK_MEDIA_PREV_TRACK: return "Previous track";
    case VK_MEDIA_STOP: return "Media stop";
    case VK_VOLUME_MUTE: return "Mute";
    case VK_VOLUME_DOWN: return "Volume down";
    case VK_VOLUME_UP: return "Volume up";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    default: break;
    }
    // Punctuation keys depend on the keyboard layout: ask Windows which character they type.
    UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF;
    if (ch > 32 && ch < 0x7F) return std::string(1, (char)ch);
    if (ch >= 0x7F) return ToUtf8(std::wstring(1, (wchar_t)ch));
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t name[64] = {};
    if (scan && GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0) return ToUtf8(name);
    char buf[16];
    snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

std::string Hotkeys::ToString(const Hotkey& h)
{
    if (!h.IsSet()) return "Not set";
    std::string s;
    if (h.mods & MOD_CONTROL) s += "Ctrl + ";
    if (h.mods & MOD_ALT) s += "Alt + ";
    if (h.mods & MOD_SHIFT) s += "Shift + ";
    if (h.mods & MOD_WIN) s += "Win + ";
    return s + KeyName(h.vk);
}

} // namespace cs
