#pragma once

#include "common.h"
#include "config.h"

#include <cstdint>

namespace of {

// All sizes are in physical pixels once Scaled() has been applied.
struct BorderStyle {
    int width = 0;
    int radius = 0;        // corner radius of the border's inner edge
    int windowRadius = 0;  // corner radius DWM actually gives the window
    COLORREF color = 0;
    int glowSize = 0;
    int glowIntensity = 0;  // percent

    bool operator==(const BorderStyle&) const = default;
    BorderStyle Scaled(UINT dpi) const;
    // Distance the decoration extends beyond the window's visible frame.
    int Margin() const;
};

// DWM only rounds windows to 0, 4 or 8 px; these pick the closest one to `radius`.
int NativeCornerRadius(int radius);
int NativeCornerPreference(int radius);  // DWM_WINDOW_CORNER_PREFERENCE
// Signed distance from (x, y) to a rounded rectangle; negative inside.
double RoundedRectDistance(double x, double y, const RECT& rect, double radius);
BorderStyle MakeBorderStyle(const Config& config, bool active);

// Renders the decoration into a premultiplied BGRA buffer covering `region`, given in
// overlay coordinates where the window occupies [margin, margin, margin+winW, margin+winH].
void RenderBorder(uint32_t* pixels, const RECT& region, int winW, int winH, const BorderStyle& style);

// Click-through layered windows drawn directly beneath a target window. The decoration is
// split into four strips so large windows don't need a full-size bitmap.
class BorderOverlay {
public:
    BorderOverlay();
    ~BorderOverlay();
    BorderOverlay(const BorderOverlay&) = delete;
    BorderOverlay& operator=(const BorderOverlay&) = delete;

    void Show(HWND target, const BorderStyle& logicalStyle);
    void Hide();

private:
    void RenderStrip(int index, const RECT& region, POINT origin, int winW, int winH, const BorderStyle& style);

    HWND strips_[4] = {};
    RECT lastRect_{};
    BorderStyle lastStyle_{};
    bool rendered_ = false;
    bool visible_ = false;
};

}  // namespace of
