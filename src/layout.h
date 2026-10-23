#pragma once

#include "common.h"
#include "config.h"

#include <vector>

namespace of {

// Splits `area` into `count` tiles. `ratio` is the master width (master layout)
// or the size of the first half of every split (dwindle layout).
std::vector<RECT> ComputeLayout(LayoutKind kind, const RECT& area, size_t count, double ratio);

// Niri-style scrolling columns: every tile is a full-height column `columnWidth` (fraction of
// the area) wide on an endless horizontal strip. `scroll` is the strip offset in pixels and is
// adjusted so the `focused` column is fully in view.
std::vector<RECT> ComputeScrollingLayout(const RECT& area, size_t count, double columnWidth, int focused, int& scroll);

}  // namespace of
