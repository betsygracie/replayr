#include "media/Banner.h"
#include "core/Platform.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace cs {

namespace {

float EaseOut(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// Timing (seconds from the start of the clip).
constexpr double kIn = 1.0, kFade = 0.4, kOut = 5.4, kShineAt = 1.7, kShineLen = 0.9;   // in 1.0-1.4 s, holds 4 s, out 5.4-5.8 s

void RoundRect(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float rad)
{
    const float d = rad * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

// The first font that exists: the embedded Poppins faces, then Windows fonts, then any sans-serif.
struct FontChoice {
    const wchar_t* family;
    int style;
};
std::unique_ptr<Gdiplus::Font> PickFont(Gdiplus::PrivateFontCollection& embedded, std::initializer_list<FontChoice> choices, float px)
{
    using namespace Gdiplus;
    for (const FontChoice& c : choices) {
        for (int pass = 0; pass < 2; ++pass) {
            FontFamily fam(c.family, pass == 0 ? &embedded : nullptr);
            if (fam.GetLastStatus() != Ok || !fam.IsStyleAvailable(c.style)) continue;
            auto f = std::make_unique<Font>(&fam, px, c.style, UnitPixel);
            if (f->GetLastStatus() == Ok && f->IsAvailable()) return f;
        }
    }
    return std::make_unique<Font>(FontFamily::GenericSansSerif(), px, choices.begin()->style, UnitPixel);
}

Gdiplus::Color Col(const float c[3], BYTE a)
{
    return Gdiplus::Color(a, (BYTE)(c[0] * 255.0f), (BYTE)(c[1] * 255.0f), (BYTE)(c[2] * 255.0f));
}

} // namespace

BannerLayout BannerLayoutFor(int frameW, int frameH, int corner)
{
    BannerLayout l;
    l.w = std::max(120, (int)std::lround(frameW * 0.18));
    l.h = std::max(36, (int)std::lround(l.w * 0.30));
    l.margin = std::max(8, (int)std::lround(frameW * 0.03));
    l.pad = std::max(6, (int)std::lround(l.h * 0.35));
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    l.x = right ? frameW - l.margin - l.w : l.margin;
    l.y = bottom ? frameH - l.margin - l.h : l.margin;
    return l;
}

BannerFrame BannerAt(double t, const BannerLayout& l, BannerMode mode)
{
    BannerFrame f;
    if (t < kIn) return f;
    float in = EaseOut((float)((t - kIn) / kFade));
    float out = 1.0f;
    if (mode == BannerMode::StartFiveSeconds) {
        if (t >= kOut + kFade) return f;
        if (t > kOut) out = 1.0f - EaseOut((float)((t - kOut) / kFade));
    }
    const float vis = std::min(in, out);
    f.visible = vis > 0.001f;
    f.alpha = vis;
    f.slide = (1.0f - vis) * (float)(l.w * 0.5 + l.margin);
    if (t >= kShineAt && t <= kShineAt + kShineLen) f.shine = (float)((t - kShineAt) / kShineLen);
    return f;
}

bool RenderBanner(const BannerLayout& l, const BannerStyle& style, std::vector<uint8_t>& out)
{
    using namespace Gdiplus;
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return false;
    const int W = l.w + l.pad * 2, H = l.h + l.pad * 2;
    out.assign((size_t)W * H * 4, 0);
    bool ok = false;
    {
        PrivateFontCollection fonts;
        for (const wchar_t* name : { L"FONT_REGULAR", L"FONT_SEMIBOLD" }) {
            size_t size = 0;
            if (const void* data = platform::EmbeddedResource(name, &size)) fonts.AddMemoryFont(data, (INT)size);
        }
        Bitmap bmp(W, H, W * 4, PixelFormat32bppPARGB, out.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        const float p = (float)l.pad, w = (float)l.w, h = (float)l.h, rad = h * 0.5f;
        const RectF pill(p, p, w, h);

        // Soft shadow under the pill.
        for (int i = 6; i >= 1; --i) {
            GraphicsPath sp;
            RectF r = pill;
            const float e = p * 0.14f * i;
            r.Inflate(e * 0.6f, e * 0.6f);
            r.Y += p * 0.25f;
            RoundRect(sp, r, rad + e * 0.6f);
            SolidBrush sb(Color((BYTE)(10 + (6 - i) * 6), 6, 0, 18));
            g.FillPath(&sb, &sp);
        }

        GraphicsPath body;
        RoundRect(body, pill, rad);
        // Dark glass, about 60% opaque.
        SolidBrush fill(Color(158, 18, 10, 36));
        g.FillPath(&fill, &body);
        Region clip(&body);
        g.SetClip(&clip);
        // Accent glow from the left edge. (Gradient brushes tile outside their range, so each fill
        // stays inside it, with a pixel of slack.)
        const float gw = w * 0.45f;
        LinearGradientBrush glow(PointF(pill.X - 1.0f, 0), PointF(pill.X + gw + 1.0f, 0), Col(style.accent, 130), Col(style.accent2, 0));
        g.FillRectangle(&glow, RectF(pill.X, pill.Y, gw, h));
        // Light on the upper half.
        LinearGradientBrush gloss(PointF(0, pill.Y - 1.0f), PointF(0, pill.Y + h * 0.6f + 1.0f), Color(46, 255, 255, 255), Color(0, 255, 255, 255));
        g.FillRectangle(&gloss, RectF(pill.X, pill.Y, w, h * 0.6f));
        g.ResetClip();
        Pen border(Color(80, 255, 255, 255), std::max(1.0f, h * 0.022f));
        g.DrawPath(&border, &body);

        // Logo: a gradient ring with a gap and a white dot.
        const float lr = h * 0.24f;
        const PointF lc(pill.X + h * 0.52f, pill.Y + h * 0.5f);
        LinearGradientBrush ringBrush(PointF(lc.X - lr * 1.3f, lc.Y - lr * 1.3f), PointF(lc.X + lr * 1.3f, lc.Y + lr * 1.3f), Col(style.accent, 255), Col(style.accent2, 255));
        Pen ring(&ringBrush, lr * 0.42f);
        ring.SetStartCap(LineCapRound);
        ring.SetEndCap(LineCapRound);
        g.DrawArc(&ring, RectF(lc.X - lr, lc.Y - lr, lr * 2, lr * 2), 50.0f, 300.0f);
        SolidBrush dot(Color(255, 255, 255, 255));
        g.FillEllipse(&dot, RectF(lc.X - lr * 0.3f, lc.Y - lr * 0.3f, lr * 0.6f, lr * 0.6f));

        // Two lines of text.
        const float tx = pill.X + h * 0.95f;
        const auto small = PickFont(fonts, { { L"Poppins", FontStyleRegular }, { L"Segoe UI", FontStyleRegular }, { L"Arial", FontStyleRegular } }, h * 0.20f);
        const auto big = PickFont(fonts,
                                  { { L"Poppins SemiBold", FontStyleRegular }, { L"Poppins", FontStyleBold }, { L"Segoe UI Semibold", FontStyleRegular },
                                    { L"Segoe UI", FontStyleBold }, { L"Arial", FontStyleBold } },
                                  h * 0.30f);
        Font& sf = *small;
        Font& bf = *big;
        SolidBrush grey(Color(190, 230, 226, 240)), white(Color(255, 255, 255, 255));
        StringFormat fmt;
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(L"Clipped using", -1, &sf, PointF(tx, pill.Y + h * 0.17f), &fmt, &grey);
        g.DrawString(L"Replayr", -1, &bf, PointF(tx - h * 0.02f, pill.Y + h * 0.40f), &fmt, &white);
        ok = true;
    }
    GdiplusShutdown(token);
    return ok;
}

void CompositeBanner(uint8_t* frame, int stride, int fw, int fh, const std::vector<uint8_t>& banner, const BannerLayout& l, const BannerFrame& f,
                     int corner)
{
    if (!f.visible || banner.empty()) return;
    const int W = l.w + l.pad * 2, H = l.h + l.pad * 2;
    const bool right = corner == 1 || corner == 3;
    const int ox = l.x - l.pad + (int)std::lround(right ? f.slide : -f.slide);
    const int oy = l.y - l.pad;
    const int a = (int)std::lround(std::clamp(f.alpha, 0.0f, 1.0f) * 256.0f);
    const float shinePos = f.shine >= 0.0f ? -0.25f + f.shine * 1.5f : -10.0f;
    for (int y = 0; y < H; ++y) {
        const int fy = oy + y;
        if (fy < 0 || fy >= fh) continue;
        const uint8_t* s = &banner[(size_t)y * W * 4];
        uint8_t* d = frame + (size_t)fy * stride;
        const float vy = (float)(y - l.pad) / (float)l.h;
        for (int x = 0; x < W; ++x, s += 4) {
            const int fx = ox + x;
            if (fx < 0 || fx >= fw || s[3] == 0) continue;
            int sb = s[0] * a >> 8, sg = s[1] * a >> 8, sr = s[2] * a >> 8, sa = s[3] * a >> 8;
            // Shine: a soft diagonal band of light that sweeps across the pill once.
            if (shinePos > -5.0f && x >= l.pad && x < l.pad + l.w && y >= l.pad && y < l.pad + l.h) {
                const float u = (float)(x - l.pad) / (float)l.w - vy * 0.18f;
                const float band = std::max(0.0f, 1.0f - std::fabs(u - shinePos) / 0.09f);
                if (band > 0.0f) {
                    const int add = (int)(band * band * 0.42f * sa);
                    sb = std::min(255, sb + add);
                    sg = std::min(255, sg + add);
                    sr = std::min(255, sr + add);
                }
            }
            uint8_t* px = d + (size_t)fx * 4;
            const int inv = 255 - sa;
            px[0] = (uint8_t)std::min(255, sb + px[0] * inv / 255);
            px[1] = (uint8_t)std::min(255, sg + px[1] * inv / 255);
            px[2] = (uint8_t)std::min(255, sr + px[2] * inv / 255);
        }
    }
}

float CaptionHeightFraction(int size)
{
    static const float k[3] = { 0.045f, 0.065f, 0.09f };
    return k[std::clamp(size, 0, 2)];
}

bool RenderCaption(const std::wstring& text, int size, int frameW, int frameH, std::vector<uint8_t>& out, int& w, int& h)
{
    using namespace Gdiplus;
    if (text.empty() || frameW <= 0 || frameH <= 0) return false;
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return false;
    bool ok = false;
    {
        PrivateFontCollection fonts;
        for (const wchar_t* name : { L"FONT_REGULAR", L"FONT_SEMIBOLD" }) {
            size_t sz = 0;
            if (const void* data = platform::EmbeddedResource(name, &sz)) fonts.AddMemoryFont(data, (INT)sz);
        }
        const float px = frameH * CaptionHeightFraction(size);
        const auto font = PickFont(fonts,
                                   { { L"Poppins SemiBold", FontStyleRegular }, { L"Poppins", FontStyleBold }, { L"Segoe UI Semibold", FontStyleRegular },
                                     { L"Segoe UI", FontStyleBold }, { L"Arial", FontStyleBold } },
                                   px);
        FontFamily fam;
        font->GetFamily(&fam);
        // Measure with a path so the outline fits.
        GraphicsPath path;
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        const float maxW = frameW * 0.9f;
        path.AddString(text.c_str(), -1, &fam, font->GetStyle(), px, RectF(0, 0, maxW, frameH * 0.8f), &fmt);
        RectF bounds;
        const float outline = std::max(2.0f, px * 0.10f);
        path.GetBounds(&bounds);
        const int pad = (int)std::ceil(outline * 2.0f + px * 0.12f);
        w = std::max(1, (int)std::ceil(bounds.Width) + pad * 2);
        h = std::max(1, (int)std::ceil(bounds.Height) + pad * 2);
        out.assign((size_t)w * h * 4, 0);
        Matrix move;
        move.Translate(pad - bounds.X, pad - bounds.Y);
        path.Transform(&move);
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, out.data());
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // Soft drop shadow, then the outline, then white fill.
        {
            GraphicsPath shadow;
            shadow.AddPath(&path, FALSE);
            Matrix down;
            down.Translate(0, px * 0.06f);
            shadow.Transform(&down);
            for (int i = 3; i >= 1; --i) {
                Pen sp(Color((BYTE)(40 / i + 20), 0, 0, 0), outline * (1.0f + i));
                sp.SetLineJoin(LineJoinRound);
                g.DrawPath(&sp, &shadow);
            }
        }
        Pen pen(Color(235, 12, 6, 24), outline * 2.0f);
        pen.SetLineJoin(LineJoinRound);
        g.DrawPath(&pen, &path);
        SolidBrush white(Color(255, 255, 255, 255));
        g.FillPath(&white, &path);
        ok = true;
    }
    GdiplusShutdown(token);
    return ok;
}

void CompositeImage(uint8_t* frame, int stride, int fw, int fh, const std::vector<uint8_t>& img, int w, int h, int ox, int oy, float alpha)
{
    if (img.empty() || alpha <= 0.0f) return;
    const int a = (int)std::lround(std::clamp(alpha, 0.0f, 1.0f) * 256.0f);
    for (int y = 0; y < h; ++y) {
        const int fy = oy + y;
        if (fy < 0 || fy >= fh) continue;
        const uint8_t* s = &img[(size_t)y * w * 4];
        uint8_t* d = frame + (size_t)fy * stride;
        for (int x = 0; x < w; ++x, s += 4) {
            const int fx = ox + x;
            if (fx < 0 || fx >= fw || s[3] == 0) continue;
            const int sb = s[0] * a >> 8, sg = s[1] * a >> 8, sr = s[2] * a >> 8, sa = s[3] * a >> 8;
            uint8_t* px = d + (size_t)fx * 4;
            const int inv = 255 - sa;
            px[0] = (uint8_t)std::min(255, sb + px[0] * inv / 255);
            px[1] = (uint8_t)std::min(255, sg + px[1] * inv / 255);
            px[2] = (uint8_t)std::min(255, sr + px[2] * inv / 255);
        }
    }
}

} // namespace cs
