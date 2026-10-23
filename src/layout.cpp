#include "layout.h"

#include <algorithm>
#include <cmath>

namespace of {

std::vector<RECT> ComputeScrollingLayout(const RECT& area, size_t count, double columnWidth, int focused,
                                         int& scroll) {
    std::vector<RECT> out;
    if (count == 0) {
        scroll = 0;
        return out;
    }
    const int view = Width(area);
    // Columns widen to fill the view until there are too many to fit at `columnWidth`.
    const int column = std::max({1, (int)std::lround(view * columnWidth), view / (int)count});
    const int strip = column * (int)count;
    if (focused >= 0 && focused < (int)count) {
        int left = column * focused;
        if (left < scroll) scroll = left;
        if (left + column > scroll + view) scroll = left + column - view;
    }
    scroll = std::clamp(scroll, 0, std::max(0, strip - view));
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int left = area.left + column * (int)i - scroll;
        out.push_back({left, area.top, left + column, area.bottom});
    }
    if (strip <= view) out.back().right = area.right;  // absorb the rounding remainder
    return out;
}

std::vector<RECT> ComputeLayout(LayoutKind kind, const RECT& area, size_t count, double ratio) {
    std::vector<RECT> out;
    if (count == 0) return out;
    out.reserve(count);

    switch (kind) {
        case LayoutKind::Monocle:
            out.assign(count, area);
            break;

        case LayoutKind::Niri: {
            int scroll = 0;
            out = ComputeScrollingLayout(area, count, ratio, -1, scroll);
            break;
        }

        case LayoutKind::MasterStack: {
            if (count == 1) {
                out.push_back(area);
                break;
            }
            int split = area.left + (int)std::lround(Width(area) * ratio);
            out.push_back({area.left, area.top, split, area.bottom});
            int stack = (int)count - 1;
            int h = Height(area);
            for (int i = 0; i < stack; ++i) {
                int top = area.top + h * i / stack;
                int bottom = area.top + h * (i + 1) / stack;
                out.push_back({split, top, area.right, bottom});
            }
            break;
        }

        case LayoutKind::Dwindle: {
            RECT r = area;
            for (size_t i = 0; i < count; ++i) {
                if (i + 1 == count) {
                    out.push_back(r);
                    break;
                }
                if (Width(r) >= Height(r)) {
                    int s = r.left + (int)std::lround(Width(r) * ratio);
                    out.push_back({r.left, r.top, s, r.bottom});
                    r.left = s;
                } else {
                    int s = r.top + (int)std::lround(Height(r) * ratio);
                    out.push_back({r.left, r.top, r.right, s});
                    r.top = s;
                }
            }
            break;
        }
    }
    return out;
}

}  // namespace of
