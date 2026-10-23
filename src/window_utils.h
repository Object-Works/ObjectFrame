#pragma once

#include "common.h"

#include <string>

namespace of {

std::wstring GetWindowClass(HWND hwnd);
std::wstring GetWindowTitle(HWND hwnd);
std::wstring GetProcessExeName(DWORD pid);

bool IsCloaked(HWND hwnd);
bool IsShellWindowClass(const std::wstring& cls);
bool IsCurrentProcessElevated();
bool IsProcessElevated(DWORD pid);

// Rect of the window as drawn, excluding the invisible resize borders.
RECT GetVisibleRect(HWND hwnd);

// Places the visible frame of `hwnd` exactly at `target`.
void PlaceWindow(HWND hwnd, const RECT& target, bool bringToTop);
// Single reposition for animation frames; skips PlaceWindow's monitor-crossing fix-up.
void PlaceWindowFrame(HWND hwnd, const RECT& target, bool bringToTop, bool async);

// Distance from each edge of the window rect to the visible frame (the invisible resize borders).
RECT GetFrameInsets(HWND hwnd);
// Makes only the part of the window inside `clip` visible, for a window whose visible frame is
// (or is about to be) at `frame`. Removes the region when the frame is entirely inside.
// Returns whether a region is set. With `currentOuter`, the region is valid both at the window's
// current position and at `frame`, so it can be applied before moving.
bool ClipWindow(HWND hwnd, const RECT& frame, const RECT& clip, const RECT& insets,
                const RECT* currentOuter = nullptr);
void UnclipWindow(HWND hwnd);

void SetBorderColor(HWND hwnd, COLORREF color);
void ResetBorderColor(HWND hwnd);

// DWMWCP_* value: 0 default, 1 square, 2 round, 3 small round.
void SetCornerPreference(HWND hwnd, int preference);
// Show/hide/minimize animations drawn by DWM.
void SetDwmTransitions(HWND hwnd, bool enabled);
UINT GetMonitorDpi(HMONITOR monitor);

void ForceForeground(HWND hwnd);

}  // namespace of
