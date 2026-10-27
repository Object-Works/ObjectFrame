#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace of {

constexpr const wchar_t* kWindowClass = L"ObjectFrameWM";
constexpr ULONG_PTR kCopyDataMagic = 0x4F46574D;  // 'OFWM'
// ObjectPulse: receives the window manager's state as WM_COPYDATA "key=value\n" lines.
constexpr const wchar_t* kPulseWindowClass = L"ObjectPulseWnd";
constexpr ULONG_PTR kPulseCopyDataMagic = 0x4F46504C;  // 'OFPL'

enum class Direction { Left, Right, Up, Down };

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring Trim(const std::wstring& s);
std::wstring ToLower(std::wstring s);
std::vector<std::wstring> SplitWhitespace(const std::wstring& s);
bool ParseDirection(const std::wstring& s, Direction& out);

void InitLog(const std::wstring& path);
void Log(const wchar_t* fmt, ...);

inline int Width(const RECT& r) { return r.right - r.left; }
inline int Height(const RECT& r) { return r.bottom - r.top; }
inline RECT Inset(RECT r, int d) {
    r.left += d;
    r.top += d;
    r.right -= d;
    r.bottom -= d;
    return r;
}
inline POINT Center(const RECT& r) { return {r.left + Width(r) / 2, r.top + Height(r) / 2}; }

}  // namespace of
