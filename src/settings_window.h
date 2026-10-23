#pragma once

#include "common.h"

#include <functional>
#include <string>

namespace of {

// Native settings editor. Runs on the window manager's UI thread; the message loop
// must route messages through IsDialogMessage(SettingsWindow::Handle(), ...).
class SettingsWindow {
public:
    static void Open(HINSTANCE instance, const std::wstring& configPath, std::function<void()> onSaved);
    static void Close();
    static HWND Handle();
};

}  // namespace of
