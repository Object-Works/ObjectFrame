#include "keyboard_hook.h"

namespace of {

namespace {

KeyboardHook* g_hook = nullptr;

// Unassigned virtual key; tapping it between Win/Alt down and up stops Explorer from
// opening the Start menu and apps from focusing their menu bar.
constexpr WORD kMaskKey = 0xE8;
constexpr ULONG_PTR kOwnInputTag = 0x4F46574B;  // marks events we inject ourselves

bool IsDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

UINT CurrentMods() {
    UINT mods = 0;
    if (IsDown(VK_LWIN) || IsDown(VK_RWIN)) mods |= MOD_WIN;
    if (IsDown(VK_MENU)) mods |= MOD_ALT;
    if (IsDown(VK_CONTROL)) mods |= MOD_CONTROL;
    if (IsDown(VK_SHIFT)) mods |= MOD_SHIFT;
    return mods;
}

}  // namespace

bool KeyboardHook::Start(HWND target, UINT message) {
    target_ = target;
    message_ = message;
    g_hook = this;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&KeyboardHook::ThreadMain, this, ready);
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    threadId_ = GetThreadId(thread_.native_handle());
    return hook_ != nullptr;
}

void KeyboardHook::Stop() {
    if (!thread_.joinable()) return;
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
    g_hook = nullptr;
}

void KeyboardHook::SetBinds(const std::vector<Bind>& binds, bool essentialOnly) {
    std::vector<Entry> entries;
    for (size_t i = 0; i < binds.size(); ++i) {
        if (essentialOnly) {
            auto verb = ToLower(binds[i].command.substr(0, binds[i].command.find(L' ')));
            if (verb != L"pause" && verb != L"quit" && verb != L"exit" && verb != L"settings") continue;
        }
        entries.push_back({binds[i].mods, binds[i].vk, i});
    }
    std::lock_guard lock(mutex_);
    entries_ = std::move(entries);
}

void KeyboardHook::ThreadMain(HANDLE ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, GetModuleHandleW(nullptr), 0);
    SetEvent(ready);
    if (!hook_) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
}

LRESULT CALLBACK KeyboardHook::Proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_hook && g_hook->Swallow(wp, *reinterpret_cast<KBDLLHOOKSTRUCT*>(lp))) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool KeyboardHook::Swallow(WPARAM wp, const KBDLLHOOKSTRUCT& k) {
    if (k.dwExtraInfo == kOwnInputTag) return false;
    DWORD vk = k.vkCode;
    if (vk > 255) return false;
    bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;

    switch (vk) {
        case VK_LWIN:
        case VK_RWIN:
            if (down) {
                if (!IsDown(VK_LWIN) && !IsDown(VK_RWIN)) winUsed_ = false;
                return false;
            }
            return ReleaseMasked(vk, winUsed_);
        case VK_LMENU:
        case VK_RMENU:
            if (down) {
                if (!IsDown(VK_MENU)) altUsed_ = false;
                return false;
            }
            return ReleaseMasked(vk, altUsed_);
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
            return false;
    }

    if (!down) {
        if (!pressed_[vk]) return false;
        pressed_[vk] = false;
        return true;
    }
    if (pressed_[vk]) return true;  // auto-repeat of a bound key

    UINT mods = CurrentMods();
    size_t index = 0;
    bool found = false;
    {
        std::lock_guard lock(mutex_);
        for (const auto& e : entries_) {
            if (e.vk == vk && e.mods == mods) {
                index = e.index;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    pressed_[vk] = true;
    if (mods & MOD_WIN) winUsed_ = true;
    if (mods & MOD_ALT) altUsed_ = true;
    PostMessageW(target_, message_, index, 0);
    return true;
}

bool KeyboardHook::ReleaseMasked(DWORD vk, bool& used) {
    if (!used) return false;
    used = false;
    INPUT in[3] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = kMaskKey;
    in[0].ki.dwExtraInfo = kOwnInputTag;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = (WORD)vk;
    in[2].ki.dwExtraInfo = kOwnInputTag;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_RMENU) in[2].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(3, in, sizeof(INPUT));
    return true;
}

}  // namespace of
