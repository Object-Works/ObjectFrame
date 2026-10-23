#include "border_overlay.h"

#include "window_utils.h"

#include <algorithm>
#include <cmath>

namespace of {

namespace {

constexpr wchar_t kStripClass[] = L"ObjectFrameBorder";

LRESULT CALLBACK StripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void EnsureStripClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StripProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kStripClass;
    RegisterClassExW(&wc);
    registered = true;
}

double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

}  // namespace

BorderStyle BorderStyle::Scaled(UINT dpi) const {
    BorderStyle s = *this;
    s.width = MulDiv(width, dpi, 96);
    s.radius = MulDiv(radius, dpi, 96);
    s.windowRadius = MulDiv(windowRadius, dpi, 96);
    s.glowSize = MulDiv(glowSize, dpi, 96);
    return s;
}

int BorderStyle::Margin() const {
    return width + (glowSize > 0 && glowIntensity > 0 ? glowSize : 0);
}

int NativeCornerRadius(int radius) { return radius < 2 ? 0 : radius < 6 ? 4 : 8; }

int NativeCornerPreference(int radius) {
    switch (NativeCornerRadius(radius)) {
        case 0: return 1;  // DWMWCP_DONOTROUND
        case 4: return 3;  // DWMWCP_ROUNDSMALL
        default: return 2;  // DWMWCP_ROUND
    }
}

BorderStyle MakeBorderStyle(const Config& c, bool active) {
    BorderStyle s;
    s.width = c.borders ? c.borderWidth : 0;
    s.radius = c.borderRadius;
    s.windowRadius = NativeCornerRadius(c.borderRadius);
    s.color = active ? c.borderActive : c.borderInactive;
    if (active && c.glow) {
        s.glowSize = c.glowSize;
        s.glowIntensity = c.glowIntensity;
    }
    return s;
}

double RoundedRectDistance(double x, double y, const RECT& rect, double radius) {
    double bx = Width(rect) / 2.0, by = Height(rect) / 2.0;
    double r = std::min(radius, std::min(bx, by));
    double qx = std::fabs(x - (rect.left + bx)) - bx + r;
    double qy = std::fabs(y - (rect.top + by)) - by + r;
    double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
}

void RenderBorder(uint32_t* pixels, const RECT& region, int winW, int winH, const BorderStyle& s) {
    const int margin = s.Margin();
    const int rw = region.right - region.left;
    const RECT win{margin, margin, margin + winW, margin + winH};
    // A radius much larger than the window's own would let the window's corners poke out
    // past the border's outer edge; this is the largest radius whose outer edge still contains them.
    const double radius = std::min<double>(s.radius, s.windowRadius + 2.414 * s.width);

    const bool glow = s.glowSize > 0 && s.glowIntensity > 0;
    const double glowAlpha = s.glowIntensity / 100.0;
    const double br = GetRValue(s.color), bg = GetGValue(s.color), bb = GetBValue(s.color);

    for (int y = region.top; y < region.bottom; ++y) {
        uint32_t* row = pixels + (size_t)(y - region.top) * rw;
        for (int x = region.left; x < region.right; ++x) {
            double fx = x + 0.5, fy = y + 0.5;
            double d = RoundedRectDistance(fx, fy, win, radius) - s.width;
            // Everything the window itself covers stays transparent, so translucent (Mica/acrylic)
            // windows aren't tinted; where the window's corner is rounder than the border's,
            // the border fills the gap.
            double uncovered = 1.0 - Clamp01(0.5 - RoundedRectDistance(fx, fy, win, s.windowRadius));
            double ring = s.width > 0 ? Clamp01(0.5 - d) * uncovered : 0.0;
            double gl = 0.0;
            if (glow) {
                double t = Clamp01(d / s.glowSize);
                gl = glowAlpha * (1.0 - t * t * (3.0 - 2.0 * t)) * uncovered;
            }
            double a = ring + gl * (1.0 - ring);
            if (a <= 0.0) {
                row[x - region.left] = 0;
                continue;
            }
            auto channel = [&](double c) { return (uint32_t)std::lround(c * a); };
            row[x - region.left] =
                ((uint32_t)std::lround(a * 255.0) << 24) | (channel(br) << 16) | (channel(bg) << 8) | channel(bb);
        }
    }
}

BorderOverlay::BorderOverlay() {
    EnsureStripClass();
    for (auto& strip : strips_) {
        strip = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kStripClass,
                                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
}

BorderOverlay::~BorderOverlay() {
    for (auto strip : strips_) {
        if (strip) DestroyWindow(strip);
    }
}

void BorderOverlay::Hide() {
    if (!visible_) return;
    for (auto strip : strips_) ShowWindow(strip, SW_HIDE);
    visible_ = false;
}

void BorderOverlay::Show(HWND target, const BorderStyle& logicalStyle) {
    RECT vr = GetVisibleRect(target);
    BorderStyle s = logicalStyle.Scaled(GetMonitorDpi(MonitorFromRect(&vr, MONITOR_DEFAULTTONEAREST)));
    int margin = s.Margin();
    int winW = Width(vr), winH = Height(vr);
    if (margin <= 0 || winW <= 0 || winH <= 0) {
        Hide();
        return;
    }

    int w = winW + 2 * margin, h = winH + 2 * margin;
    int band = std::min((h + 1) / 2, margin + std::max(s.radius, s.windowRadius) + 1);
    RECT regions[4] = {
        {0, 0, w, band},
        {0, std::max(band, h - band), w, h},
        {0, band, margin + 1, h - band},
        {w - margin - 1, band, w, h - band},
    };
    bool rerender = !rendered_ || !(s == lastStyle_) || winW != Width(lastRect_) || winH != Height(lastRect_);
    POINT origin{vr.left - margin, vr.top - margin};

    for (int i = 0; i < 4; ++i) {
        const RECT& rg = regions[i];
        if (rg.right <= rg.left || rg.bottom <= rg.top) {
            ShowWindow(strips_[i], SW_HIDE);
            continue;
        }
        if (rerender) RenderStrip(i, rg, origin, winW, winH, s);
        // Inserting after the target puts the strip directly beneath it in z-order.
        SetWindowPos(strips_[i], target, origin.x + rg.left, origin.y + rg.top, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    lastRect_ = vr;
    lastStyle_ = s;
    rendered_ = true;
    visible_ = true;
}

void BorderOverlay::RenderStrip(int index, const RECT& region, POINT origin, int winW, int winH,
                                const BorderStyle& style) {
    int rw = Width(region), rh = Height(region);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = rw;
    bi.bmiHeader.biHeight = -rh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        RenderBorder(static_cast<uint32_t*>(bits), region, winW, winH, style);
        HGDIOBJ old = SelectObject(mem, bmp);
        POINT dst{origin.x + region.left, origin.y + region.top};
        SIZE size{rw, rh};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(strips_[index], screen, &dst, &size, mem, &src, 0, &blend, ULW_ALPHA);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

}  // namespace of
