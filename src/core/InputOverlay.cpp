#include "core/InputOverlay.h"
#include "core/Log.h"
#include "core/Platform.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace cs {

namespace {

// ---- layout ------------------------------------------------------------------------------------

const float kStep = 1.1f;   // key pitch (1 key + gap)

std::vector<OverlayKey> BuildKeyboard(float ox)
{
    std::vector<OverlayKey> k;
    auto row = [&](float y, float x, std::initializer_list<std::pair<int, const char*>> keys) {
        for (auto& kv : keys) {
            k.push_back({ ox + x, y, 1.0f, 1.0f, kv.first, kv.second });
            x += kStep;
        }
    };
    k.push_back({ ox + 0.0f, 0.0f, 1.0f, 1.0f, VK_ESCAPE, "Esc" });
    row(0.0f, kStep, { { '1', "1" }, { '2', "2" }, { '3', "3" }, { '4', "4" }, { '5', "5" } });
    k.push_back({ ox + 0.0f, 1.1f, 1.5f, 1.0f, VK_TAB, "Tab" });
    row(1.1f, 1.6f, { { 'Q', "Q" }, { 'W', "W" }, { 'E', "E" }, { 'R', "R" }, { 'T', "T" } });
    k.push_back({ ox + 0.0f, 2.2f, 1.75f, 1.0f, VK_CAPITAL, "Caps" });
    row(2.2f, 1.85f, { { 'A', "A" }, { 'S', "S" }, { 'D', "D" }, { 'F', "F" }, { 'G', "G" } });
    k.push_back({ ox + 0.0f, 3.3f, 2.25f, 1.0f, VK_SHIFT, "Shift" });
    row(3.3f, 2.35f, { { 'Z', "Z" }, { 'X', "X" }, { 'C', "C" }, { 'V', "V" } });
    k.push_back({ ox + 0.0f, 4.4f, 1.5f, 1.0f, VK_CONTROL, "Ctrl" });
    k.push_back({ ox + 1.6f, 4.4f, 1.25f, 1.0f, VK_MENU, "Alt" });
    k.push_back({ ox + 2.95f, 4.4f, 4.3f, 1.0f, VK_SPACE, "" });
    return k;
}

std::vector<OverlayKey> BuildMouse(float ox)
{
    return {
        { ox + 0.0f, 0.0f, 3.0f, 5.4f, -1, "" },              // body
        { ox + 0.0f, 0.0f, 1.5f, 2.3f, VK_LBUTTON, "" },
        { ox + 1.5f, 0.0f, 1.5f, 2.3f, VK_RBUTTON, "" },
        { ox + 1.2f, 0.45f, 0.6f, 1.3f, VK_MBUTTON, "" },
        { ox + 0.12f, 2.75f, 0.42f, 0.8f, VK_XBUTTON2, "" },
        { ox + 0.12f, 3.65f, 0.42f, 0.8f, VK_XBUTTON1, "" },
    };
}

struct Layouts {
    std::vector<OverlayKey> all, kb, mouse;
    float allW = 0, kbW = 0, mouseW = 0, h = 5.4f;
    Layouts()
    {
        kb = BuildKeyboard(0.0f);
        kbW = 7.25f;
        mouse = BuildMouse(0.0f);
        mouseW = 3.0f;
        all = BuildKeyboard(0.0f);
        for (const OverlayKey& m : BuildMouse(kbW + 0.6f)) all.push_back(m);
        allW = kbW + 0.6f + mouseW;
    }
};
const Layouts& L()
{
    static Layouts l;
    return l;
}

bool InLayout(int vk)
{
    for (const OverlayKey& k : L().all)
        if (k.vk == vk) return true;
    return false;
}

// ---- state shared with the capture thread ------------------------------------------------------

struct CaptureBitmap {
    int w = 0, h = 0, corner = 2;
    std::vector<uint8_t> px;   // premultiplied BGRA, sized for a 1080p frame
    uint64_t version = 0;
};
std::mutex g_capMutex;
std::shared_ptr<const CaptureBitmap> g_capture;

} // namespace

const std::vector<OverlayKey>& OverlayLayout(int show, float& w, float& h)
{
    const Layouts& l = L();
    h = l.h;
    if (show == 1) {
        w = l.kbW;
        return l.kb;
    }
    if (show == 2) {
        w = l.mouseW;
        return l.mouse;
    }
    w = l.allW;
    return l.all;
}

const char* OverlayKeyName(int vk)
{
    static char buf[16];
    switch (vk) {
    case VK_RETURN: return "Enter";
    case VK_BACK: return "Backspace";
    case VK_UP: return "\xE2\x86\x91";
    case VK_DOWN: return "\xE2\x86\x93";
    case VK_LEFT: return "\xE2\x86\x90";
    case VK_RIGHT: return "\xE2\x86\x92";
    case VK_DELETE: return "Del";
    case VK_INSERT: return "Ins";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PgUp";
    case VK_NEXT: return "PgDn";
    case VK_LWIN:
    case VK_RWIN: return "Win";
    case VK_OEM_3: return "`";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_1: return ";";
    case VK_OEM_7: return "'";
    case VK_OEM_4: return "[";
    case VK_OEM_6: return "]";
    case VK_OEM_5: return "\\";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(buf, sizeof(buf), "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = (char)vk;
        buf[1] = 0;
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof(buf), "Num %d", vk - VK_NUMPAD0);
        return buf;
    }
    return nullptr;   // not worth showing
}

struct InputOverlay::Impl {
    std::thread thread;
    std::atomic<bool> quit { false };
    mutable std::mutex m;
    InputOverlayConfig cfg;
    bool cfgChanged = true;
    bool down[256] = {};
    struct Extra {
        int vk;
        std::chrono::steady_clock::time_point at;
    };
    std::deque<Extra> extras;
    std::chrono::steady_clock::time_point wheelAt {};
    int wheelDir = 0;
    std::atomic<bool> dirty { true };
    HWND hwnd = nullptr;
    uint64_t version = 0;

    // Rendering
    Gdiplus::PrivateFontCollection* fonts = nullptr;
    HBITMAP dib = nullptr;
    void* dibBits = nullptr;
    int dibW = 0, dibH = 0;
    bool shownOnScreen = false;
    int affinity = -1;

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        Impl* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_INPUT && self) {
            self->OnRawInput((HRAWINPUT)lp);
            return 0;
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    }

    void OnRawInput(HRAWINPUT hri)
    {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(hri, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
        std::lock_guard<std::mutex> lock(m);
        if (ri.header.dwType == RIM_TYPEKEYBOARD) {
            int vk = ri.data.keyboard.VKey;
            if (vk <= 0 || vk >= 255) return;
            if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
            if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
            if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
            const bool up = (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (!up && !down[vk] && !InLayout(vk) && OverlayKeyName(vk)) {
                extras.push_back({ vk, std::chrono::steady_clock::now() });
                while (extras.size() > 3) extras.pop_front();
            }
            down[vk] = !up;
        } else if (ri.header.dwType == RIM_TYPEMOUSE) {
            const USHORT f = ri.data.mouse.usButtonFlags;
            if (f & RI_MOUSE_LEFT_BUTTON_DOWN) down[VK_LBUTTON] = true;
            if (f & RI_MOUSE_LEFT_BUTTON_UP) down[VK_LBUTTON] = false;
            if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) down[VK_RBUTTON] = true;
            if (f & RI_MOUSE_RIGHT_BUTTON_UP) down[VK_RBUTTON] = false;
            if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) down[VK_MBUTTON] = true;
            if (f & RI_MOUSE_MIDDLE_BUTTON_UP) down[VK_MBUTTON] = false;
            if (f & RI_MOUSE_BUTTON_4_DOWN) down[VK_XBUTTON1] = true;
            if (f & RI_MOUSE_BUTTON_4_UP) down[VK_XBUTTON1] = false;
            if (f & RI_MOUSE_BUTTON_5_DOWN) down[VK_XBUTTON2] = true;
            if (f & RI_MOUSE_BUTTON_5_UP) down[VK_XBUTTON2] = false;
            if (f & RI_MOUSE_WHEEL) {
                wheelDir = (short)ri.data.mouse.usButtonData > 0 ? 1 : -1;
                wheelAt = std::chrono::steady_clock::now();
            }
            if (!(f & ~(USHORT)0)) return;   // pure movement: nothing to redraw
        }
        dirty = true;
    }

    // A game (or anything) covering its whole monitor, that isn't Replayr.
    static bool ForegroundIsFullscreen(HMONITOR* mon)
    {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) return false;
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
        RECT wr{};
        if (!GetWindowRect(fg, &wr)) return false;
        HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hm, &mi)) return false;
        if (mon) *mon = hm;
        return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    }

    // Draws the overlay with GDI+ into premultiplied BGRA. `u` = pixels per key unit.
    std::vector<uint8_t> Render(const InputOverlayConfig& c, float u, int& W, int& H)
    {
        using namespace Gdiplus;
        float lw = 0, lh = 0;
        const std::vector<OverlayKey>& keys = OverlayLayout(c.show, lw, lh);
        const float pad = u * 0.35f, pillsH = c.show == 2 ? 0.0f : u * 1.15f;
        W = (int)std::ceil(lw * u + pad * 2.0f);
        H = (int)std::ceil(lh * u + pad * 2.0f + pillsH);
        std::vector<uint8_t> out((size_t)W * H * 4, 0);
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, out.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        const Color a1((BYTE)(c.accent[0] * 255), (BYTE)(c.accent[1] * 255), (BYTE)(c.accent[2] * 255));
        const Color a2((BYTE)std::min(255.0f, c.accent[0] * 255 + 70), (BYTE)std::min(255.0f, c.accent[1] * 120 + 40), (BYTE)std::min(255.0f, c.accent[2] * 255));
        FontFamily fam(L"Poppins SemiBold", fonts);
        FontFamily fallback(L"Segoe UI");
        const FontFamily* ff = fam.GetLastStatus() == Ok ? &fam : &fallback;
        Font big(ff, u * 0.40f, FontStyleRegular, UnitPixel), small(ff, u * 0.28f, FontStyleRegular, UnitPixel);
        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);

        auto roundRect = [](GraphicsPath& p, RectF r, float rad) {
            rad = std::min(rad, std::min(r.Width, r.Height) * 0.5f);
            const float d = rad * 2.0f;
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
            p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
            p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
            p.CloseFigure();
        };
        auto keyFace = [&](RectF r, float rad, bool on, const wchar_t* label, bool smallText) {
            if (on) {   // glow
                for (int i = 3; i >= 1; --i) {
                    GraphicsPath gp;
                    const float e = u * 0.06f * i;
                    roundRect(gp, RectF(r.X - e, r.Y - e, r.Width + e * 2, r.Height + e * 2), rad + e);
                    SolidBrush gb(Color((BYTE)(40 / i), a2.GetR(), a2.GetG(), a2.GetB()));
                    g.FillPath(&gb, &gp);
                }
            }
            GraphicsPath p;
            roundRect(p, r, rad);
            if (on) {
                LinearGradientBrush lg(PointF(r.X, r.Y - 1), PointF(r.X + r.Width, r.Y + r.Height + 1), a1, a2);
                g.FillPath(&lg, &p);
            } else {
                SolidBrush fill(Color(165, 14, 8, 32));
                g.FillPath(&fill, &p);
                LinearGradientBrush gloss(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height * 0.5f + 1), Color(34, 255, 255, 255), Color(0, 255, 255, 255));
                GraphicsPath top;
                roundRect(top, RectF(r.X, r.Y, r.Width, r.Height * 0.5f), rad);
                g.FillPath(&gloss, &top);
            }
            Pen border(on ? Color(220, 255, 255, 255) : Color(70, 255, 255, 255), std::max(1.0f, u * 0.035f));
            g.DrawPath(&border, &p);
            if (label && *label) {
                SolidBrush tb(on ? Color(255, 255, 255, 255) : Color(215, 255, 255, 255));
                g.DrawString(label, -1, smallText ? &small : &big, r, &center, &tb);
            }
        };

        std::unique_lock<std::mutex> lock(m);
        bool downCopy[256];
        memcpy(downCopy, down, sizeof(down));
        std::deque<Extra> ex = extras;
        const auto wheelAge = std::chrono::duration<float>(std::chrono::steady_clock::now() - wheelAt).count();
        const int wdir = wheelDir;
        lock.unlock();

        const float ox = pad, oy = pad + pillsH;
        // Mouse body first, so the buttons sit on it.
        for (const OverlayKey& k : keys) {
            if (k.vk != -1) continue;
            const RectF body(ox + k.x * u, oy + k.y * u, k.w * u, k.h * u);
            GraphicsPath p;
            roundRect(p, body, u * 1.45f);
            SolidBrush fill(Color(165, 14, 8, 32));
            g.FillPath(&fill, &p);
            // Buttons are clipped to the body's shape.
            g.SetClip(&p);
            for (const OverlayKey& b : keys) {
                if (b.vk != VK_LBUTTON && b.vk != VK_RBUTTON) continue;
                const RectF br(ox + b.x * u, oy + b.y * u, b.w * u, b.h * u);
                if (downCopy[b.vk]) {
                    LinearGradientBrush lg(PointF(br.X, br.Y - 1), PointF(br.X + br.Width, br.Y + br.Height + 1), a1, a2);
                    g.FillRectangle(&lg, br);
                } else {
                    LinearGradientBrush gloss(PointF(0, br.Y - 1), PointF(0, br.Y + br.Height + 1), Color(30, 255, 255, 255), Color(0, 255, 255, 255));
                    g.FillRectangle(&gloss, br);
                }
            }
            Pen line(Color(80, 255, 255, 255), std::max(1.0f, u * 0.035f));
            g.DrawLine(&line, body.X, body.Y + 2.3f * u, body.X + body.Width, body.Y + 2.3f * u);
            g.DrawLine(&line, body.X + body.Width * 0.5f, body.Y, body.X + body.Width * 0.5f, body.Y + 2.3f * u);
            g.ResetClip();
            Pen border(Color(90, 255, 255, 255), std::max(1.0f, u * 0.04f));
            g.DrawPath(&border, &p);
        }
        for (const OverlayKey& k : keys) {
            if (k.vk == -1 || k.vk == VK_LBUTTON || k.vk == VK_RBUTTON) continue;
            const RectF r(ox + k.x * u, oy + k.y * u, k.w * u, k.h * u);
            if (k.vk == VK_MBUTTON) {
                // The wheel: lights when clicked, shows an arrow when scrolled.
                const bool scrolled = wheelAge < 0.22f;
                keyFace(r, r.Width * 0.5f, downCopy[VK_MBUTTON] || scrolled, nullptr, true);
                if (scrolled) {
                    SolidBrush tb(Color(255, 255, 255, 255));
                    g.DrawString(wdir > 0 ? L"\x25B2" : L"\x25BC", -1, &small, r, &center, &tb);
                }
                continue;
            }
            std::wstring label;
            for (const char* c2 = k.label; *c2; ++c2) label += (wchar_t)*c2;
            keyFace(r, u * 0.2f, downCopy[k.vk], label.c_str(), label.size() > 1);
        }
        // Other keys as pills above the keyboard, fading out.
        if (pillsH > 0) {
            float x = ox;
            const auto now = std::chrono::steady_clock::now();
            for (const Extra& e : ex) {
                const float age = std::chrono::duration<float>(now - e.at).count();
                if (age > 1.2f) continue;
                const char* name = OverlayKeyName(e.vk);
                if (!name) continue;
                std::wstring w;
                for (const char* c2 = name; *c2; ++c2) w += (wchar_t)(unsigned char)*c2;
                if (!strcmp(name, "\xE2\x86\x91")) w = L"\x2191";
                if (!strcmp(name, "\xE2\x86\x93")) w = L"\x2193";
                if (!strcmp(name, "\xE2\x86\x90")) w = L"\x2190";
                if (!strcmp(name, "\xE2\x86\x92")) w = L"\x2192";
                RectF bounds;
                g.MeasureString(w.c_str(), -1, &big, PointF(0, 0), &bounds);
                const float pw = std::max(u * 0.95f, bounds.Width + u * 0.5f);
                const RectF r(x, pad, pw, u * 0.85f);
                keyFace(r, r.Height * 0.5f, age < 0.15f || downCopy[e.vk], w.c_str(), false);
                x += pw + u * 0.15f;
            }
        }
        // Opacity, and a fade for pills is handled by their expiry; apply the global opacity here.
        const float op = std::clamp(c.opacity, 0.2f, 1.0f);
        if (op < 0.999f)
            for (uint8_t& b : out) b = (uint8_t)(b * op);
        return out;
    }

    bool AnyAnimating()
    {
        std::lock_guard<std::mutex> lock(m);
        const auto now = std::chrono::steady_clock::now();
        for (const Extra& e : extras)
            if (std::chrono::duration<float>(now - e.at).count() < 1.3f) return true;
        return std::chrono::duration<float>(now - wheelAt).count() < 0.3f;
    }

    void Update()
    {
        InputOverlayConfig c;
        {
            std::lock_guard<std::mutex> lock(m);
            c = cfg;
        }
        HMONITOR mon = nullptr;
        const bool fullscreen = ForegroundIsFullscreen(&mon);
        const bool visible = c.enabled && (!c.gamesOnly || fullscreen);
        if (!mon) mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        static const float units[3] = { 20.0f, 28.0f, 38.0f };
        const float u = units[std::clamp(c.size, 0, 2)];

        // Clips-only: hand the capture a bitmap (sized for 1080p; it scales to the real frame).
        {
            std::shared_ptr<CaptureBitmap> cap;
            if (visible && c.mode == 2) {
                cap = std::make_shared<CaptureBitmap>();
                cap->px = Render(c, u, cap->w, cap->h);
                cap->corner = c.corner;
                cap->version = ++version;
            }
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_capture = cap;
        }
        const bool onScreen = visible && c.mode != 2;
        if (!onScreen) {
            if (shownOnScreen) ShowWindow(hwnd, SW_HIDE);
            shownOnScreen = false;
            return;
        }
        int W = 0, H = 0;
        const std::vector<uint8_t> px = Render(c, u, W, H);
        if (!dib || W != dibW || H != dibH) {
            if (dib) DeleteObject(dib);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
            dibW = W;
            dibH = H;
        }
        if (!dib) return;
        memcpy(dibBits, px.data(), px.size());
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        const RECT& r = mi.rcMonitor;
        const int margin = 24;
        POINT pos{ (c.corner == 1 || c.corner == 3) ? r.right - W - margin : r.left + margin, c.corner >= 2 ? r.bottom - H - margin : r.top + margin };
        SIZE sz{ W, H };
        POINT src{ 0, 0 };
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, dib);
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        // "Only on my screen": keep it out of captures (Windows 10 2004 and later).
        const int want = c.mode == 1 ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : 0 /* WDA_NONE */;
        if (want != affinity) {
            SetWindowDisplayAffinity(hwnd, (DWORD)want);
            affinity = want;
        }
        if (!shownOnScreen) ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
        shownOnScreen = true;
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Run()
    {
        Gdiplus::GdiplusStartupInput gin;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &gin, nullptr);
        fonts = new Gdiplus::PrivateFontCollection();
        for (const wchar_t* name : { L"FONT_SEMIBOLD" }) {
            size_t sz = 0;
            if (const void* data = platform::EmbeddedResource(name, &sz)) fonts->AddMemoryFont(data, (INT)sz);
        }
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ReplayrInputOverlay";
        RegisterClassExW(&wc);
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0,
                               0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        RAWINPUTDEVICE rid[2] = { { 0x01, 0x06, RIDEV_INPUTSINK, hwnd }, { 0x01, 0x02, RIDEV_INPUTSINK, hwnd } };
        bool registered = false;
        auto lastCheck = std::chrono::steady_clock::now();
        bool lastVisible = false;
        while (!quit) {
            bool enabled;
            {
                std::lock_guard<std::mutex> lock(m);
                enabled = cfg.enabled;
                if (cfgChanged) {
                    dirty = true;
                    cfgChanged = false;
                }
            }
            // Only listen to input while the overlay is on.
            if (enabled != registered) {
                if (enabled) RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
                else {
                    RAWINPUTDEVICE off[2] = { { 0x01, 0x06, RIDEV_REMOVE, nullptr }, { 0x01, 0x02, RIDEV_REMOVE, nullptr } };
                    RegisterRawInputDevices(off, 2, sizeof(RAWINPUTDEVICE));
                    std::lock_guard<std::mutex> lock(m);
                    memset(down, 0, sizeof(down));
                    extras.clear();
                }
                registered = enabled;
                dirty = true;
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            // Re-check the foreground window a few times a second (games starting / alt-tab).
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCheck > std::chrono::milliseconds(250)) {
                lastCheck = now;
                const bool vis = enabled && (!cfg.gamesOnly || ForegroundIsFullscreen(nullptr));
                if (vis != lastVisible) dirty = true;
                lastVisible = vis;
            }
            if (dirty.exchange(false) || (enabled && AnyAnimating())) Update();
        }
        {
            std::lock_guard<std::mutex> lock(g_capMutex);
            g_capture.reset();
        }
        if (dib) DeleteObject(dib);
        DestroyWindow(hwnd);
        delete fonts;
        Gdiplus::GdiplusShutdown(token);
    }
};

InputOverlay::InputOverlay() : d_(std::make_unique<Impl>()) {}

InputOverlay::~InputOverlay()
{
    d_->quit = true;
    if (d_->thread.joinable()) d_->thread.join();
}

void InputOverlay::Start()
{
    d_->thread = std::thread([this] { d_->Run(); });
}

void InputOverlay::SetConfig(const InputOverlayConfig& c)
{
    std::lock_guard<std::mutex> lock(d_->m);
    if (memcmp(&c, &d_->cfg, sizeof(c)) == 0) return;
    d_->cfg = c;
    d_->cfgChanged = true;
}

bool InputOverlay::IsDown(int vk) const
{
    std::lock_guard<std::mutex> lock(d_->m);
    return vk > 0 && vk < 256 && d_->down[vk];
}

std::vector<int> InputOverlay::RecentExtraKeys() const
{
    std::lock_guard<std::mutex> lock(d_->m);
    std::vector<int> v;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& e : d_->extras)
        if (std::chrono::duration<float>(now - e.at).count() < 1.2f) v.push_back(e.vk);
    return v;
}

void InputOverlay::DrawIntoCapture(void* hdcPtr, int frameW, int frameH, int, int, int, int)
{
    std::shared_ptr<const CaptureBitmap> cap;
    {
        std::lock_guard<std::mutex> lock(g_capMutex);
        cap = g_capture;
    }
    if (!cap || cap->px.empty() || !hdcPtr) return;
    // Cached DIB on the capture thread, rebuilt when the overlay changes.
    thread_local HDC mem = nullptr;
    thread_local HBITMAP bmp = nullptr;
    thread_local uint64_t have = 0;
    thread_local void* bits = nullptr;
    if (!mem) mem = CreateCompatibleDC(nullptr);
    if (have != cap->version) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = cap->w;
        bi.bmiHeader.biHeight = -cap->h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return;
        memcpy(bits, cap->px.data(), cap->px.size());
        SelectObject(mem, bmp);
        have = cap->version;
    }
    const float k = frameH / 1080.0f;
    const int w = (int)(cap->w * k), h = (int)(cap->h * k), margin = (int)(24 * k);
    const int x = (cap->corner == 1 || cap->corner == 3) ? frameW - w - margin : margin;
    const int y = cap->corner >= 2 ? frameH - h - margin : margin;
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend((HDC)hdcPtr, x, y, w, h, mem, 0, 0, cap->w, cap->h, bf);
}

} // namespace cs
