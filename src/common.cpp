#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <share.h>

namespace of {

namespace {
FILE* g_log = nullptr;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::vector<std::wstring> SplitWhitespace(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (iswspace(c)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool ParseDirection(const std::wstring& s, Direction& out) {
    auto l = ToLower(Trim(s));
    if (l == L"left" || l == L"l") out = Direction::Left;
    else if (l == L"right" || l == L"r") out = Direction::Right;
    else if (l == L"up" || l == L"u") out = Direction::Up;
    else if (l == L"down" || l == L"d") out = Direction::Down;
    else return false;
    return true;
}

void InitLog(const std::wstring& path) {
    if (g_log) fclose(g_log);
    g_log = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    swprintf_s(line, L"[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
    OutputDebugStringW(line);
    if (g_log) {
        fputws(line, g_log);
        fflush(g_log);
    }
}

}  // namespace of
