#pragma once
#include "core/Settings.h"

#include <array>
#include <string>

namespace cs {

enum class HotkeyStatus { Unbound, Ok, Duplicate, InUse };

// Global hotkeys via RegisterHotKey: they fire while a fullscreen game has focus, cost nothing
// while idle and need no keyboard hook. (Mouse buttons can't be RegisterHotKey'd; that would need
// a WH_MOUSE_LL hook.)
class Hotkeys {
public:
    void Apply(void* hwnd, const std::array<Hotkey, kHotkeyCount>& keys);
    void UnregisterAll();
    void Suspend(bool suspended);          // unregister while the binder is capturing a key
    bool Suspended() const { return suspended_; }
    HotkeyStatus Status(HotkeyAction a) const { return status_[(int)a]; }

    static int IdFor(HotkeyAction a) { return 0x4300 + (int)a; }
    static bool ActionFromId(int id, HotkeyAction& out);
    static std::string KeyName(uint32_t vk);
    static std::string ToString(const Hotkey& h);   // "Alt + F10"

private:
    void RegisterAll();
    void* hwnd_ = nullptr;
    std::array<Hotkey, kHotkeyCount> keys_{};
    std::array<HotkeyStatus, kHotkeyCount> status_{};
    std::array<bool, kHotkeyCount> registered_{};
    bool suspended_ = false;
};

} // namespace cs
