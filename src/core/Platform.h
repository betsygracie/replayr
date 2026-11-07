#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Thin wrappers over Win32/Shell calls, so UI code never includes <windows.h>.
namespace cs::platform {

void OpenPath(const std::wstring& path);                 // open with the default app
void OpenUrl(const std::wstring& url);
void RevealInExplorer(const std::wstring& path);         // Explorer with the file selected
bool PickFolder(void* ownerHwnd, std::wstring& inOut);
bool PickFile(void* ownerHwnd, std::wstring& inOut, const wchar_t* filterName, const wchar_t* filterSpec);
bool RecycleFile(const std::wstring& path);              // to the Recycle Bin (undoable)
bool RenameFile(const std::wstring& from, const std::wstring& to);
bool DeleteFileNow(const std::wstring& path);            // permanent (for partial/temp files)

bool SetStartWithWindows(bool enable);
bool IsStartWithWindowsEnabled();

std::wstring ForegroundAppName(void* ownHwnd);           // "Valorant", "Minecraft"... or "Desktop"
uint32_t WindowsBuildNumber();                           // e.g. 22631
uint64_t DiskFreeBytes(const std::wstring& path);
bool IsKeyDown(uint32_t vk);                             // global (works while a game has focus)

struct MonitorDesc {
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
};
std::vector<MonitorDesc> EnumerateMonitors();

void PrimaryWorkArea(int& x, int& y, int& w, int& h);
float SystemDpiScale();
bool RectOnAnyMonitor(int x, int y, int w, int h);
void ShowErrorBox(const std::wstring& text);
bool CommandLineHas(const wchar_t* flag);

enum class Sound { ClipSaved, RecordStart, RecordStop, Screenshot, Error };
void PlaySoundCue(Sound s);                              // short synthesized cues, no asset files
void PlayUiSound(int kind);                              // 0 hover tick, 1 click (very quiet)

// Clipboard: files (paste into Explorer, Discord, chat apps) and images (paste as a picture).
bool CopyFilesToClipboard(void* ownerHwnd, const std::vector<std::wstring>& paths);
bool CopyImageToClipboard(void* ownerHwnd, const std::wstring& imagePath);
bool CopyTextToClipboard(void* ownerHwnd, const std::string& utf8);

// Data embedded in the exe as an RCDATA resource (fonts). Null if missing.
const void* EmbeddedResource(const wchar_t* name, size_t* size);
// Keeps a window out of screen captures and screenshots (Windows 10 2004+; no-op before).
void ExcludeFromCapture(void* hwnd, bool exclude);
void AttachConsoleWindow();                              // --console (debug builds)

} // namespace cs::platform
