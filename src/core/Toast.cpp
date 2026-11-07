#include "core/Toast.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Util.h"
#include "ui/Icons.h"

#include <windows.h>
#include <objidl.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// gdiplus.h uses min/max, which NOMINMAX removes.
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

namespace cs {

static const wchar_t* kToastClass = L"ReplayrToast";

struct ToastItem {
    HWND hwnd = nullptr;
    ToastKind kind = ToastKind::Info;
    double start = 0.0;
    double lifetime = 3.0;
    int w = 0, h = 0;
    float y = 0.0f;           // animated offset from the corner
    bool positioned = false;
    RECT work{};
    bool dying = false;
    double dieStart = 0.0;
};

struct Toasts::Impl {
    HINSTANCE hinst = nullptr;
    ULONG_PTR gdiplusToken = 0;
    ToastStyle style;
    std::vector<ToastItem> items;   // newest last
    bool timerRunning = false;
    UINT_PTR timerId = 0;          // SetTimer(NULL, ...) picks its own id
    std::wstring iconFamily;
    std::unique_ptr<Gdiplus::PrivateFontCollection> fonts;   // Poppins from the exe's resources
    bool havePoppins = false;

    void Tick();
    void LoadFonts();
    void Render(ToastItem& it, const std::wstring& title, const std::wstring& body, const std::wstring& icon, const std::wstring& image, float s);
};

static Toasts::Impl* g_toasts = nullptr;

static LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static VOID CALLBACK ToastTimerProc(HWND, UINT, UINT_PTR, DWORD)
{
    if (g_toasts) g_toasts->Tick();
}

Toasts::Toasts() : impl_(std::make_unique<Impl>()) {}
Toasts::~Toasts() { Shutdown(); }

bool Toasts::Init(void* hinstance)
{
    impl_->hinst = (HINSTANCE)hinstance;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&impl_->gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        Log("GDI+ failed to start; toasts disabled");
        impl_->gdiplusToken = 0;
        return false;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastWndProc;
    wc.hInstance = impl_->hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kToastClass;
    RegisterClassExW(&wc);

    Gdiplus::FontFamily fluent(L"Segoe Fluent Icons");
    impl_->iconFamily = fluent.IsAvailable() ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    impl_->LoadFonts();
    g_toasts = impl_.get();
    return true;
}

void Toasts::Shutdown()
{
    if (!impl_) return;
    if (impl_->timerRunning) {
        KillTimer(nullptr, impl_->timerId);
        impl_->timerRunning = false;
    }
    for (auto& it : impl_->items)
        if (it.hwnd) DestroyWindow(it.hwnd);
    impl_->items.clear();
    if (g_toasts == impl_.get()) g_toasts = nullptr;
    impl_->fonts.reset();   // before GDI+ shuts down
    if (impl_->gdiplusToken) {
        Gdiplus::GdiplusShutdown(impl_->gdiplusToken);
        impl_->gdiplusToken = 0;
    }
}

void Toasts::SetStyle(const ToastStyle& style) { impl_->style = style; }

static Gdiplus::Color ToColor(const float c[3], BYTE a)
{
    return Gdiplus::Color(a, (BYTE)(Clamp(c[0], 0.0f, 1.0f) * 255.0f), (BYTE)(Clamp(c[1], 0.0f, 1.0f) * 255.0f),
                          (BYTE)(Clamp(c[2], 0.0f, 1.0f) * 255.0f));
}

static void AddRoundRect(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad)
{
    float d = rad * 2.0f;
    d = std::min(d, std::min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

void Toasts::Impl::LoadFonts()
{
    fonts = std::make_unique<Gdiplus::PrivateFontCollection>();
    for (const wchar_t* name : { L"FONT_REGULAR", L"FONT_SEMIBOLD" }) {
        size_t size = 0;
        if (const void* data = platform::EmbeddedResource(name, &size)) havePoppins = fonts->AddMemoryFont(data, (INT)size) == Gdiplus::Ok || havePoppins;
    }
}

void Toasts::Impl::Render(ToastItem& it, const std::wstring& title, const std::wstring& body, const std::wstring& icon, const std::wstring& image, float s)
{
    using namespace Gdiplus;
    const int W = it.w, H = it.h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));

        const float pad = 8.0f * s;
        RectF r(pad, pad, W - 2.0f * pad, H - 2.0f * pad);
        const float rad = 14.0f * s;

        // Soft shadow.
        for (int i = 4; i >= 1; --i) {
            GraphicsPath sp;
            RectF sr = r;
            sr.Inflate(i * 1.6f * s, i * 1.6f * s);
            sr.Y += 2.0f * s;
            AddRoundRect(sp, sr, rad + i * 1.6f * s);
            SolidBrush sb(Color((BYTE)(14 + (4 - i) * 6), 0, 0, 0));
            g.FillPath(&sb, &sp);
        }

        GraphicsPath bodyPath;
        AddRoundRect(bodyPath, r, rad);
        LinearGradientBrush bodyBrush(PointF(0, r.Y), PointF(0, r.Y + r.Height), Color(244, 52, 26, 96), Color(244, 26, 12, 56));
        g.FillPath(&bodyBrush, &bodyPath);

        // Gloss on the upper half.
        Region clip(&bodyPath);
        g.SetClip(&clip);
        LinearGradientBrush gloss(PointF(0, r.Y - 1), PointF(0, r.Y + r.Height * 0.55f), Color(40, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&gloss, RectF(r.X, r.Y, r.Width, r.Height * 0.55f));
        g.ResetClip();

        Pen border(Color(64, 255, 255, 255), std::max(1.0f, s));
        g.DrawPath(&border, &bodyPath);

        // Accent colours by kind.
        Color c1 = ToColor(style.accent, 255), c2 = ToColor(style.accent2, 255);
        if (it.kind == ToastKind::Recording || it.kind == ToastKind::Error) {
            c1 = Color(255, 255, 77, 109);
            c2 = Color(255, 255, 145, 90);
        }

        // Thumbnail of what was saved, when there is one; otherwise an icon bubble.
        std::unique_ptr<Bitmap> thumb;
        if (!image.empty()) {
            thumb.reset(Bitmap::FromFile(image.c_str()));
            if (thumb && thumb->GetLastStatus() != Ok) thumb.reset();
        }
        const float bubble = 36.0f * s;
        RectF br(r.X + 14.0f * s, r.Y + (r.Height - bubble) * 0.5f, bubble, bubble);
        if (thumb) {
            const float th = r.Height - 20.0f * s, tw = th * 16.0f / 9.0f;
            RectF tr(r.X + 10.0f * s, r.Y + 10.0f * s, tw, th);
            GraphicsPath tp;
            AddRoundRect(tp, tr, 8.0f * s);
            Region tclip(&tp);
            g.SetClip(&tclip);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            // Cover-fit the image into the 16:9 box.
            const float iw = (float)thumb->GetWidth(), ih = (float)thumb->GetHeight();
            const float k = std::max(tw / iw, th / ih);
            const float dw = iw * k, dh = ih * k;
            g.DrawImage(thumb.get(), RectF(tr.X + (tw - dw) * 0.5f, tr.Y + (th - dh) * 0.5f, dw, dh));
            g.ResetClip();
            Pen tpen(Color(60, 255, 255, 255), std::max(1.0f, s));
            g.DrawPath(&tpen, &tp);
            br = RectF(tr.X, tr.Y, tw, th);
        } else {
        GraphicsPath bubblePath;
        bubblePath.AddEllipse(br);
        LinearGradientBrush bubbleBrush(PointF(br.X, br.Y), PointF(br.X + br.Width, br.Y + br.Height), c1, c2);
        g.FillPath(&bubbleBrush, &bubblePath);
        Region bclip(&bubblePath);
        g.SetClip(&bclip);
        LinearGradientBrush bubbleGloss(PointF(0, br.Y), PointF(0, br.Y + br.Height * 0.6f), Color(90, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&bubbleGloss, RectF(br.X, br.Y, br.Width, br.Height * 0.6f));
        g.ResetClip();

        StringFormat center;
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        SolidBrush white(Color(255, 255, 255, 255));
        if (!icon.empty()) {
            Font iconFont(iconFamily.c_str(), 15.0f * s, FontStyleRegular, UnitPixel);
            RectF ir = br;
            ir.Y += 1.0f * s;
            g.DrawString(icon.c_str(), -1, &iconFont, ir, &center, &white);
        }
        }

        StringFormat left;
        left.SetFormatFlags(StringFormatFlagsNoWrap);
        left.SetTrimming(StringTrimmingEllipsisCharacter);
        left.SetLineAlignment(StringAlignmentCenter);
        const float tx = br.X + br.Width + 12.0f * s;
        const float tw = r.X + r.Width - tx - 12.0f * s;
        FontFamily poppins(L"Poppins", havePoppins ? fonts.get() : nullptr);
        const bool usePoppins = havePoppins && poppins.GetLastStatus() == Ok;
        Font titleFont(usePoppins ? &poppins : nullptr, 14.0f * s, FontStyleBold, UnitPixel);
        Font titleFallback(L"Segoe UI Semibold", 14.5f * s, FontStyleRegular, UnitPixel);
        Font bodyFont(usePoppins ? &poppins : nullptr, 12.0f * s, FontStyleRegular, UnitPixel);
        Font bodyFallback(L"Segoe UI", 12.5f * s, FontStyleRegular, UnitPixel);
        Font& tf = usePoppins && titleFont.GetLastStatus() == Ok ? titleFont : titleFallback;
        Font& bf = usePoppins && bodyFont.GetLastStatus() == Ok ? bodyFont : bodyFallback;
        SolidBrush titleBrush(Color(255, 255, 255, 255));
        SolidBrush bodyBrushText(Color(185, 255, 255, 255));
        if (body.empty()) {
            g.DrawString(title.c_str(), -1, &tf, RectF(tx, r.Y, tw, r.Height), &left, &titleBrush);
        } else {
            g.DrawString(title.c_str(), -1, &tf, RectF(tx, r.Y + r.Height * 0.5f - 21.0f * s, tw, 20.0f * s), &left, &titleBrush);
            g.DrawString(body.c_str(), -1, &bf, RectF(tx, r.Y + r.Height * 0.5f + 1.0f * s, tw, 18.0f * s), &left, &bodyBrushText);
        }
    }

    POINT src{ 0, 0 };
    SIZE size{ W, H };
    POINT pos{ -32000, -32000 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };
    UpdateLayeredWindow(it.hwnd, screen, &pos, &size, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Toasts::Show(ToastKind kind, const std::string& title, const std::string& body, const char* iconUtf8, const std::wstring& imagePath)
{
    Impl& d = *impl_;
    if (!d.gdiplusToken) return;

    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    const float s = dpiX / 96.0f;

    // Keep at most 3 on screen.
    int alive = 0;
    for (auto& it : d.items)
        if (!it.dying) ++alive;
    for (auto& it : d.items) {
        if (alive < 3) break;
        if (!it.dying) {
            it.dying = true;
            it.dieStart = NowSeconds();
            --alive;
        }
    }

    ToastItem it;
    it.kind = kind;
    it.start = NowSeconds();
    it.lifetime = (kind == ToastKind::Error) ? 4.5 : 3.0;
    it.w = (int)((imagePath.empty() ? 360.0f : 390.0f) * s);
    it.h = (int)(84.0f * s);
    it.work = mi.rcWork;
    it.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kToastClass, L"", WS_POPUP, -32000, -32000, it.w, it.h, nullptr, nullptr, d.hinst, nullptr);
    if (!it.hwnd) return;

    std::wstring icon = iconUtf8 ? ToWide(iconUtf8) : std::wstring();
    if (icon.empty()) {
        switch (kind) {
        case ToastKind::Success: icon = ToWide(ICON_CHECK); break;
        case ToastKind::Recording: icon = ToWide(ICON_RECORD); break;
        case ToastKind::Error: icon = ToWide(ICON_WARNING); break;
        default: icon = ToWide(ICON_INFO); break;
        }
    }
    d.Render(it, ToWide(title), ToWide(body), icon, imagePath, s);
    ShowWindow(it.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(it.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    d.items.push_back(it);

    if (!d.timerRunning) {
        d.timerId = SetTimer(nullptr, 0, 15, ToastTimerProc);
        d.timerRunning = d.timerId != 0;
    }
    d.Tick();
}

void Toasts::Impl::Tick()
{
    const double now = NowSeconds();
    const int corner = style.corner;
    const bool right = (corner == 1 || corner == 3);
    const bool bottom = (corner >= 2);

    // Newest toast sits at the corner, older ones stack away from it.
    float stackOffset = 0.0f;
    for (int i = (int)items.size() - 1; i >= 0; --i) {
        ToastItem& it = items[i];
        double age = now - it.start;
        if (!it.dying && age > it.lifetime) {
            it.dying = true;
            it.dieStart = now;
        }
        float alpha;
        float slide;
        if (it.dying) {
            double t = (now - it.dieStart) / 0.30;
            alpha = (float)std::max(0.0, 1.0 - t);
            slide = (float)std::min(1.0, t) * 0.5f;
        } else {
            // Springs in from the edge: a damped spring (ratio 0.6) that overshoots a little and settles.
            const double w0 = 17.3, z = 0.6, wd = w0 * std::sqrt(1.0 - z * z);
            const double pos = 1.0 - std::exp(-z * w0 * age) * (std::cos(wd * age) + z * w0 / wd * std::sin(wd * age));
            alpha = (float)std::min(1.0, age / 0.15);
            slide = age > 1.2 ? 0.0f : (float)(1.0 - pos);
        }
        float target = stackOffset;
        if (!it.positioned) {
            it.y = target;
            it.positioned = true;
        }
        it.y += (target - it.y) * 0.25f;
        if (!it.dying) stackOffset += it.h * 0.92f;

        const int margin = (int)(12.0f * (it.h / 84.0f));
        int x = right ? it.work.right - it.w - margin : it.work.left + margin;
        x += (int)((right ? 1.0f : -1.0f) * slide * 60.0f);
        int y = bottom ? (int)(it.work.bottom - it.h - margin - it.y) : (int)(it.work.top + margin + it.y);
        POINT pos{ x, y };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, (BYTE)(Clamp(alpha, 0.0f, 1.0f) * 255.0f), AC_SRC_ALPHA };
        UpdateLayeredWindow(it.hwnd, nullptr, &pos, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    }

    // Remove finished toasts.
    for (size_t i = 0; i < items.size();) {
        if (items[i].dying && now - items[i].dieStart > 0.32) {
            DestroyWindow(items[i].hwnd);
            items.erase(items.begin() + (long)i);
        } else {
            ++i;
        }
    }
    if (items.empty() && timerRunning) {
        KillTimer(nullptr, timerId);
        timerRunning = false;
    }
}

} // namespace cs
