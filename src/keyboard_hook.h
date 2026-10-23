#pragma once

#include "common.h"
#include "config.h"

#include <mutex>
#include <thread>
#include <vector>

namespace of {

// Global hotkeys via WH_KEYBOARD_LL on a dedicated thread. Unlike RegisterHotKey this can
// override Explorer's Win+key shortcuts. Matched binds are posted to `target` as
// `message` with wParam = bind index.
class KeyboardHook {
public:
    bool Start(HWND target, UINT message);
    void Stop();
    void SetBinds(const std::vector<Bind>& binds, bool essentialOnly);

private:
    struct Entry {
        UINT mods;
        UINT vk;
        size_t index;
    };

    static LRESULT CALLBACK Proc(int code, WPARAM wp, LPARAM lp);
    bool Swallow(WPARAM wp, const KBDLLHOOKSTRUCT& k);
    bool ReleaseMasked(DWORD vk, bool& used);
    void ThreadMain(HANDLE ready);

    std::thread thread_;
    DWORD threadId_ = 0;
    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
    UINT message_ = 0;

    std::mutex mutex_;
    std::vector<Entry> entries_;

    // Only touched on the hook thread.
    bool pressed_[256] = {};
    bool winUsed_ = false;
    bool altUsed_ = false;
};

}  // namespace of
