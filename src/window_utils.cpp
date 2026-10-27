#include "window_utils.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

namespace of {

namespace {
// Windows 11 (22000+) attributes; older SDKs don't define them.
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr COLORREF kDwmColorDefault = 0xFFFFFFFF;
}  // namespace

std::wstring GetWindowClass(HWND hwnd) {
    wchar_t buf[256];
    int n = GetClassNameW(hwnd, buf, 256);
    return std::wstring(buf, n > 0 ? n : 0);
}

std::wstring GetWindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring s(len + 1, L'\0');
    int n = GetWindowTextW(hwnd, s.data(), len + 1);
    s.resize(n > 0 ? n : 0);
    return s;
}

std::wstring GetProcessExeName(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = (DWORD)std::size(buf);
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size)) {
        name.assign(buf, size);
        size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
    }
    CloseHandle(proc);
    return name;
}

bool IsCloaked(HWND hwnd) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool IsShellWindowClass(const std::wstring& cls) {
    static const wchar_t* kClasses[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
        L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow",
        L"TopLevelWindowForOverflowXamlIsland",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
        L"NotifyIconOverflowWindow",
    };
    for (auto* c : kClasses) {
        if (cls == c) return true;
    }
    return false;
}

static bool TokenIsElevated(HANDLE process, bool fallback) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return fallback;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool result = fallback;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        result = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return result;
}

bool IsCurrentProcessElevated() { return TokenIsElevated(GetCurrentProcess(), false); }

bool IsProcessElevated(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return true;  // can't inspect it, so we likely can't move it either
    bool elevated = TokenIsElevated(proc, true);
    CloseHandle(proc);
    return elevated;
}

RECT GetVisibleRect(HWND hwnd) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        GetWindowRect(hwnd, &r);
    }
    return r;
}

static void PlaceOnce(HWND hwnd, const RECT& target, bool bringToTop, bool async) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    RECT fr = GetVisibleRect(hwnd);
    int l = fr.left - wr.left;
    int t = fr.top - wr.top;
    int r = wr.right - fr.right;
    int b = wr.bottom - fr.bottom;
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | (bringToTop ? 0 : SWP_NOZORDER);
    if (async) flags |= SWP_ASYNCWINDOWPOS;
    SetWindowPos(hwnd, bringToTop ? HWND_TOP : nullptr, target.left - l, target.top - t,
                 Width(target) + l + r, Height(target) + t + b, flags);
}

void PlaceWindow(HWND hwnd, const RECT& target, bool bringToTop) {
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    HMONITOR before = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    HMONITOR after = MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST);
    if (before != after) {
        // Crossing monitors can change DPI and therefore the invisible border size;
        // move synchronously first, then correct with the new frame metrics.
        PlaceOnce(hwnd, target, bringToTop, false);
    }
    PlaceOnce(hwnd, target, bringToTop, true);
}

void PlaceWindowFrame(HWND hwnd, const RECT& target, bool bringToTop, bool async) {
    PlaceOnce(hwnd, target, bringToTop, async);
}

RECT GetFrameInsets(HWND hwnd) {
    RECT wr{}, vr = GetVisibleRect(hwnd);
    GetWindowRect(hwnd, &wr);
    return {vr.left - wr.left, vr.top - wr.top, wr.right - vr.right, wr.bottom - vr.bottom};
}

bool ClipWindow(HWND hwnd, const RECT& frame, const RECT& clip, const RECT& insets, const RECT* currentOuter) {
    RECT outer{frame.left - insets.left, frame.top - insets.top, frame.right + insets.right,
               frame.bottom + insets.bottom};
    RECT shown{};
    IntersectRect(&shown, &outer, &clip);
    OffsetRect(&shown, -outer.left, -outer.top);
    if (currentOuter) {
        RECT now{};
        IntersectRect(&now, currentOuter, &clip);
        OffsetRect(&now, -currentOuter->left, -currentOuter->top);
        IntersectRect(&shown, &shown, &now);
    } else {
        RECT inside{};
        if (IntersectRect(&inside, &frame, &clip) && EqualRect(&inside, &frame)) {
            UnclipWindow(hwnd);
            return false;
        }
    }
    HRGN rgn = IsRectEmpty(&shown) ? CreateRectRgn(0, 0, 0, 0) : CreateRectRgnIndirect(&shown);
    if (!SetWindowRgn(hwnd, rgn, TRUE)) DeleteObject(rgn);  // on success the system owns `rgn`
    return true;
}

void UnclipWindow(HWND hwnd) {
    RECT box;
    if (GetWindowRgnBox(hwnd, &box) != ERROR) SetWindowRgn(hwnd, nullptr, TRUE);
}

void SetBorderColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &color, sizeof(color));
}

void ResetBorderColor(HWND hwnd) {
    COLORREF color = kDwmColorDefault;
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &color, sizeof(color));
}

void SetDwmTransitions(HWND hwnd, bool enabled) {
    BOOL disabled = enabled ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled));
}

void SetCornerPreference(HWND hwnd, int preference) {
    DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &preference, sizeof(preference));
}

bool WindowHasSquareCorners(HWND hwnd) {
    if (!IsWindow(hwnd) || IsZoomed(hwnd)) return true;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return true;
    // DWM only rounds a top-level frame. A borderless popup stays square.
    if ((style & (WS_CAPTION | WS_THICKFRAME | WS_DLGFRAME | WS_BORDER)) == 0) return true;
    int pref = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, kDwmCornerPreference, &pref, sizeof(pref))) && pref == 1) return true;
    return false;
}

UINT GetMonitorDpi(HMONITOR monitor) {
    UINT x = 96, y = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) return 96;
    return x;
}

void ForceForeground(HWND hwnd) {
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (GetForegroundWindow() == hwnd) return;
    if (SetForegroundWindow(hwnd)) return;
    // The foreground lock is lifted for the process that generated the last input event.
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
    SetForegroundWindow(hwnd);
}

}  // namespace of
