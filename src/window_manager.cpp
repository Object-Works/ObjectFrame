#include "window_manager.h"

#include "layout.h"
#include "resource.h"
#include "window_utils.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <filesystem>

namespace of {

namespace {

constexpr UINT WM_APP_WINEVENT = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_COMMAND = WM_APP + 3;
constexpr UINT WM_APP_HOTKEY = WM_APP + 4;
constexpr UINT_PTR kDisplayTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;
constexpr UINT_PTR kHoverTimer = 3;
constexpr UINT_PTR kPulseTimer = 4;
constexpr UINT kHoverPollMs = 30;
constexpr UINT kPulseDelayMs = 40;  // coalesces bursts of changes (e.g. title updates) into one message
constexpr ULONGLONG kHoverDelayMs = 60;  // how long the cursor must rest on a window before it's focused
constexpr ULONGLONG kFocusSuppressMs = 400;

enum TrayCommand : UINT {
    kTrayPause = 100,
    kTrayRetile,
    kTrayReload,
    kTrayEditConfig,
    kTrayOpenLog,
    kTrayQuit,
};

WindowManager* g_instance = nullptr;

void EraseValue(std::vector<HWND>& v, HWND h) { v.erase(std::remove(v.begin(), v.end(), h), v.end()); }

}  // namespace

WindowManager& WindowManager::Instance() {
    static WindowManager instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool WindowManager::Init(HINSTANCE instance, const std::wstring& configPath) {
    g_instance = this;
    instance_ = instance;
    configPath_ = configPath;
    selfElevated_ = IsCurrentProcessElevated();

    std::vector<std::wstring> warnings;
    if (!LoadConfigFile(configPath_, config_, warnings)) {
        Log(L"config not readable (%s), using built-in defaults", configPath_.c_str());
        warnings.clear();
        LoadConfigText(DefaultConfigText(), config_, warnings);
    }
    for (auto& w : warnings) Log(L"config: %s", w.c_str());

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_OFWM), IMAGE_ICON,
                                             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_OFWM), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (!RegisterClassExW(&wc)) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"ObjectFrame", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                            instance, nullptr);
    if (!hwnd_) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    animator_.Init(hwnd_, kAnimationTimer);
    UpdateHoverTimer();
    animator_.SetDuration(config_.animationDuration);
    animator_.shouldHide = [this](HWND h) {
        auto it = windows_.find(h);
        if (it == windows_.end() || it->second.workspace == monitors_[it->second.monitor].active) return false;
        // Hiding can make Windows activate some other window; keep the focus we chose.
        if (GetForegroundWindow() == expectedFocus_) suppressFocusUntil_ = GetTickCount64() + kFocusSuppressMs;
        return true;
    };
    animator_.onFinished = [this](HWND h) {
        if (IsManaged(h)) SyncOverlay(h);
    };

    RestoreHiddenFromPreviousRun();
    EnumerateMonitors();
    if (LinkedActive()) {
        for (int j = 0; j < (int)monitors_.size(); ++j) monitors_[j].active = j;
    }
    AddTrayIcon();
    if (!keyboard_.Start(hwnd_, WM_APP_HOTKEY)) {
        Log(L"keyboard hook could not be installed: error %lu", GetLastError());
        Notify(L"ObjectFrame", L"Keyboard hook could not be installed; hotkeys are disabled.");
    }
    ApplyHotkeys();

    auto hook = [this](DWORD min, DWORD max) {
        HWINEVENTHOOK h = SetWinEventHook(min, max, nullptr, WinEventProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
        else Log(L"SetWinEventHook(0x%X) failed", min);
    };
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    hook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);
    hook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    hook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE);

    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<WindowManager*>(lp)->TryManage(h, false);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    RetileAll();

    HWND fg = GetForegroundWindow();
    if (auto it = windows_.find(fg); it != windows_.end()) {
        focusedMonitor_ = it->second.monitor;
        monitors_[focusedMonitor_].Active().lastFocused = fg;
    } else {
        focusedMonitor_ = std::max(0, MonitorIndex(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)));
    }
    UpdateFocusBorder(fg);

    for (auto& cmd : config_.execOnce) Exec(cmd);
    OnStateChanged();

    Log(L"ObjectFrame started: %zu monitor(s), %zu window(s), elevated=%d", monitors_.size(), windows_.size(),
        selfElevated_ ? 1 : 0);
    if (!warnings.empty()) {
        Notify(L"ObjectFrame", std::to_wstring(warnings.size()) + L" config warning(s). See ofwm.log.");
    }
    return true;
}

int WindowManager::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Shutdown();
    return (int)msg.wParam;
}

void WindowManager::Shutdown() {
    if (shutdown_) return;
    shutdown_ = true;
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    keyboard_.Stop();
    overlays_.clear();
    RestoreAllWindows();
    SendPulse(L"of=0\n");
    RemoveTrayIcon();
    std::error_code ec;
    std::filesystem::remove(HiddenListPath(), ec);
    Log(L"ObjectFrame stopped");
}

void WindowManager::RestoreAllWindows() {
    animator_.FinishAll(false);
    animator_.UnclipAll();
    for (auto& [h, info] : windows_) {
        if (!IsWindow(h)) continue;
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        ResetBorderColor(h);
        SetCornerPreference(h, 0);
        SetDwmTransitions(h, true);
    }
}

void WindowManager::Quit() {
    Shutdown();
    DestroyWindow(hwnd_);
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowManager::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WindowManager* self = g_instance;
    if (!self || !self->hwnd_) return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == self->taskbarCreatedMsg_ && msg != 0) {
        self->AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_APP_WINEVENT:
            if (!self->shutdown_) self->OnWinEvent((DWORD)wp, (HWND)lp);
            return 0;

        case WM_APP_HOTKEY: {
            if ((size_t)wp < self->config_.binds.size()) self->Dispatch(self->config_.binds[wp].command);
            return 0;
        }

        case WM_APP_COMMAND:
            while (!self->pendingCommands_.empty()) {
                std::wstring cmd = std::move(self->pendingCommands_.front());
                self->pendingCommands_.pop_front();
                self->Dispatch(cmd);
            }
            return 0;

        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == kCopyDataMagic && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
                std::wstring cmd(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
                while (!cmd.empty() && cmd.back() == L'\0') cmd.pop_back();
                self->pendingCommands_.push_back(std::move(cmd));
                PostMessageW(hwnd, WM_APP_COMMAND, 0, 0);
                return TRUE;
            }
            return FALSE;
        }

        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) self->ShowTrayMenu();
            else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + self->configPath_ + L"\"").c_str(), nullptr,
                              SW_SHOWNORMAL);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            SetTimer(hwnd, kDisplayTimer, 1000, nullptr);
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) SetTimer(hwnd, kDisplayTimer, 500, nullptr);
            break;

        case WM_TIMER:
            if (wp == kAnimationTimer) {
                self->animator_.Tick();
            } else if (wp == kHoverTimer) {
                self->OnHoverTick();
            } else if (wp == kPulseTimer) {
                self->PublishPulse();
            } else if (wp == kDisplayTimer) {
                KillTimer(hwnd, kDisplayTimer);
                self->OnDisplayChange();
            }
            return 0;

        case WM_QUERYENDSESSION:
            return TRUE;

        case WM_ENDSESSION:
            if (wp) self->Shutdown();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CALLBACK WindowManager::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                          DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!g_instance) return;
    // Out-of-context callbacks run on our UI thread, so windows_ can be read here; this keeps the
    // very chatty location events for unmanaged windows out of the queue.
    if (event == EVENT_OBJECT_LOCATIONCHANGE && !g_instance->IsManaged(hwnd)) return;
    if (g_instance->hwnd_) {
        PostMessageW(g_instance->hwnd_, WM_APP_WINEVENT, event, reinterpret_cast<LPARAM>(hwnd));
    }
}

void WindowManager::OnWinEvent(DWORD event, HWND h) {
    if (event == EVENT_OBJECT_DESTROY) {
        if (IsManaged(h)) Unmanage(h, true);
        return;
    }
    if (paused_) return;

    switch (event) {
        case EVENT_OBJECT_SHOW:
            if (auto it = windows_.find(h); it != windows_.end()) {
                // An app re-showed itself while its workspace is hidden: keep it hidden.
                const auto& info = it->second;
                if (info.workspace != monitors_[info.monitor].active && IsWindowVisible(h)) {
                    ShowWindowAsync(h, SW_HIDE);
                }
            } else {
                TryManage(h, true);
            }
            break;

        case EVENT_OBJECT_UNCLOAKED:
        case EVENT_SYSTEM_MINIMIZEEND:
        case EVENT_OBJECT_NAMECHANGE:
            if (!IsManaged(h)) TryManage(h, true);
            if (event == EVENT_OBJECT_NAMECHANGE && h == GetForegroundWindow()) RequestPulseUpdate();
            break;

        case EVENT_OBJECT_HIDE:
            if (auto it = windows_.find(h); it != windows_.end()) {
                // Hides of windows on inactive workspaces are our own doing.
                const auto& info = it->second;
                if (info.workspace == monitors_[info.monitor].active && !IsWindowVisible(h)) Unmanage(h, true);
            }
            break;

        case EVENT_OBJECT_CLOAKED:
            if (auto it = windows_.find(h); it != windows_.end()) {
                const auto& info = it->second;
                if (info.workspace == monitors_[info.monitor].active && IsCloaked(h)) Unmanage(h, true);
            }
            break;

        case EVENT_SYSTEM_MINIMIZESTART:
            if (IsManaged(h)) Unmanage(h, true);
            break;

        case EVENT_SYSTEM_FOREGROUND:
            OnForeground(h);
            break;

        case EVENT_SYSTEM_MOVESIZESTART:
            animator_.Cancel(h);  // the user grabbed it mid-animation
            break;

        case EVENT_SYSTEM_MOVESIZEEND:
            OnMoveSizeEnd(h);
            break;

        case EVENT_OBJECT_LOCATIONCHANGE:
            SyncOverlay(h);
            break;
    }
}

void WindowManager::OnForeground(HWND h) {
    if (paused_ || !h) return;

    if (GetTickCount64() < suppressFocusUntil_) {
        // Windows likes to activate arbitrary windows while we hide/show workspaces.
        if (h != expectedFocus_) return;
        suppressFocusUntil_ = 0;
    }

    auto it = windows_.find(h);
    if (it == windows_.end() && TryManage(h, true)) it = windows_.find(h);

    if (it != windows_.end()) {
        WindowInfo info = it->second;
        Monitor& mon = monitors_[info.monitor];
        if (info.workspace != mon.active) {
            // A hidden window activated itself (e.g. from a notification); follow it.
            SwitchWorkspace(info.monitor, info.workspace);
        }
        focusedMonitor_ = info.monitor;
        Workspace& ws = mon.workspaces[info.workspace];
        bool refocused = ws.lastFocused != h;
        ws.lastFocused = h;
        if (ws.layout == LayoutKind::Monocle && !info.floating) {
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        // Scroll a niri strip to whatever got focused (click, Alt+Tab, ...).
        if (ws.layout == LayoutKind::Niri && !info.floating && refocused && info.workspace == mon.active) {
            Retile(info.monitor);
        }
    } else if (!IsShellWindowClass(GetWindowClass(h))) {
        int m = MonitorIndex(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
        if (m >= 0) focusedMonitor_ = m;
    }
    UpdateFocusBorder(h);
    OnStateChanged();
}

void WindowManager::OnMoveSizeEnd(HWND h) {
    if (paused_) return;
    auto it = windows_.find(h);
    if (it == windows_.end() || it->second.floating) return;
    WindowInfo info = it->second;
    Monitor& mon = monitors_[info.monitor];
    Workspace& ws = mon.workspaces[info.workspace];

    RECT now = GetVisibleRect(h);
    auto tile = tileRects_.find(h);
    RECT before = tile != tileRects_.end() ? tile->second : now;
    bool resized = std::abs(Width(now) - Width(before)) > 4 || std::abs(Height(now) - Height(before)) > 4;

    if (resized) {
        // Translate a mouse resize of the first tile into a new split ratio.
        if (ws.tiled.size() > 1 && ws.tiled.front() == h) {
            RECT area = mon.work;
            if (ws.layout == LayoutKind::MasterStack) {
                ws.masterRatio = std::clamp((now.right - area.left) / (double)Width(area), 0.1, 0.9);
            } else if (ws.layout == LayoutKind::Dwindle) {
                ws.dwindleRatio = Width(area) >= Height(area)
                                      ? std::clamp((now.right - area.left) / (double)Width(area), 0.1, 0.9)
                                      : std::clamp((now.bottom - area.top) / (double)Height(area), 0.1, 0.9);
            }
        }
        Retile(info.monitor);
        return;
    }

    // Moved: swap with the tile under the cursor (possibly on another monitor).
    POINT pt;
    GetCursorPos(&pt);
    int target = MonitorIndex(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    if (target < 0) target = info.monitor;
    Workspace& tws = monitors_[target].Active();
    HWND drop = nullptr;
    for (HWND o : tws.tiled) {
        if (o == h) continue;
        auto r = tileRects_.find(o);
        if (r != tileRects_.end() && PtInRect(&r->second, pt)) {
            drop = o;
            break;
        }
    }

    if (target == info.monitor) {
        if (drop) {
            auto a = std::find(ws.tiled.begin(), ws.tiled.end(), h);
            auto b = std::find(ws.tiled.begin(), ws.tiled.end(), drop);
            if (a != ws.tiled.end() && b != ws.tiled.end()) std::iter_swap(a, b);
        }
        Retile(info.monitor);
    } else {
        Detach(h);
        auto pos = drop ? std::find(tws.tiled.begin(), tws.tiled.end(), drop) : tws.tiled.end();
        tws.tiled.insert(pos, h);
        windows_[h] = {target, monitors_[target].active, false};
        tws.lastFocused = h;
        focusedMonitor_ = target;
        Retile(info.monitor);
        Retile(target);
    }
    UpdateFocusBorder(GetForegroundWindow());
}

// ---------------------------------------------------------------------------
// Window tracking
// ---------------------------------------------------------------------------

bool WindowManager::TryManage(HWND h, bool retile) {
    if (!h || h == hwnd_ || IsManaged(h)) return false;
    if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return false;
    if (GetAncestor(h, GA_ROOT) != h) return false;

    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (style & WS_CHILD) return false;
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    if (IsCloaked(h)) return false;

    std::wstring cls = GetWindowClass(h);
    if (IsShellWindowClass(cls)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if (!selfElevated_ && IsProcessElevated(pid)) return false;

    std::wstring exe = GetProcessExeName(pid);
    std::wstring title = GetWindowTitle(h);

    bool floating = !(style & WS_THICKFRAME);
    bool forced = false;
    for (const auto& rule : config_.rules) {
        const std::wstring& subject = rule.field == WindowRule::Field::Class ? cls
                                      : rule.field == WindowRule::Field::Exe ? exe
                                                                             : title;
        if (!std::regex_search(subject, rule.re)) continue;
        if (rule.action == WindowRule::Action::Ignore) return false;
        floating = rule.action == WindowRule::Action::Float;
        forced = true;
        break;
    }
    if (!forced && !(style & (WS_CAPTION | WS_THICKFRAME))) return false;  // splash screens, popups

    int m = MonitorIndex(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST));
    if (m < 0) m = 0;
    Monitor& mon = monitors_[m];
    Attach(h, m, mon.active, floating, mon.Active().lastFocused);

    if (GetForegroundWindow() == h) activeBorder_ = h;
    ApplyDecorations(h);
    Log(L"manage %p class=%s exe=%s title=\"%s\"%s", h, cls.c_str(), exe.c_str(), title.c_str(),
        floating ? L" [float]" : L"");
    if (retile) Retile(m);
    else SyncOverlay(h);
    return true;
}

void WindowManager::Unmanage(HWND h, bool retile) {
    auto it = windows_.find(h);
    if (it == windows_.end()) return;
    WindowInfo info = it->second;
    Detach(h);
    overlays_.erase(h);
    animator_.Cancel(h);
    if (IsWindow(h)) {
        ResetBorderColor(h);
        SetCornerPreference(h, 0);
        SetDwmTransitions(h, true);
    }
    if (activeBorder_ == h) activeBorder_ = nullptr;
    Log(L"unmanage %p", h);
    if (retile && info.workspace == monitors_[info.monitor].active) Retile(info.monitor);
}

void WindowManager::Attach(HWND h, int m, int w, bool floating, HWND after) {
    Workspace& ws = monitors_[m].workspaces[w];
    auto& list = floating ? ws.floating : ws.tiled;
    auto it = std::find(list.begin(), list.end(), after);
    if (it != list.end()) list.insert(it + 1, h);
    else list.push_back(h);
    windows_[h] = {m, w, floating};
}

void WindowManager::Detach(HWND h) {
    auto it = windows_.find(h);
    if (it == windows_.end()) return;
    Workspace& ws = monitors_[it->second.monitor].workspaces[it->second.workspace];
    EraseValue(ws.tiled, h);
    EraseValue(ws.floating, h);
    if (ws.lastFocused == h) ws.lastFocused = nullptr;
    if (ws.fullscreen == h) ws.fullscreen = nullptr;
    windows_.erase(it);
    tileRects_.erase(h);
}

void WindowManager::PruneDead() {
    std::vector<HWND> dead;
    for (auto& [h, info] : windows_) {
        if (!IsWindow(h)) dead.push_back(h);
    }
    for (HWND h : dead) {
        Detach(h);
        overlays_.erase(h);
        animator_.Cancel(h);
        if (activeBorder_ == h) activeBorder_ = nullptr;
    }
}

void WindowManager::Rescan() {
    PruneDead();
    std::vector<HWND> gone;
    for (auto& [h, info] : windows_) {
        if (info.workspace == monitors_[info.monitor].active && (!IsWindowVisible(h) || IsIconic(h))) {
            gone.push_back(h);
        }
    }
    for (HWND h : gone) Unmanage(h, false);
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<WindowManager*>(lp)->TryManage(h, false);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    RetileAll();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void WindowManager::Retile(int m, bool forceAnimate, const RECT* clip) {
    if (m < 0 || m >= (int)monitors_.size()) return;
    bool animate = forceAnimate || config_.animateMove;
    PruneDead();
    Monitor& mon = monitors_[m];
    Workspace& ws = mon.Active();

    std::vector<HWND> tiles;
    for (HWND h : ws.tiled) {
        if (h != ws.fullscreen) tiles.push_back(h);
    }

    int gi = config_.gapsIn;
    RECT area = Inset(mon.work, config_.gapsOut - gi / 2);
    std::vector<RECT> rects;
    const RECT* tileClip = clip;
    if (ws.layout == LayoutKind::Niri) {
        auto focused = std::find(tiles.begin(), tiles.end(), ws.lastFocused);
        int index = focused == tiles.end() ? -1 : (int)(focused - tiles.begin());
        rects = ComputeScrollingLayout(area, tiles.size(), ws.columnWidth, index, ws.scroll);
        // Columns scrolled out of view would otherwise show up on the neighbouring monitor.
        tileClip = &mon.work;
    } else {
        double ratio = ws.layout == LayoutKind::MasterStack ? ws.masterRatio : ws.dwindleRatio;
        rects = ComputeLayout(ws.layout, area, tiles.size(), ratio);
    }

    HWND top = ws.layout == LayoutKind::Monocle ? (ws.lastFocused ? ws.lastFocused : tiles.empty() ? nullptr : tiles[0])
                                                : nullptr;
    // Thick borders are drawn outside the window, so shrink tiles to keep them inside the tile.
    int inset = gi / 2 + BorderInset(mon);
    for (size_t i = 0; i < tiles.size(); ++i) {
        RECT r = Inset(rects[i], inset);
        tileRects_[tiles[i]] = r;
        MoveWindowTo(tiles[i], r, tiles[i] == top, animate, tileClip);
    }
    if (ws.fullscreen) {
        tileRects_[ws.fullscreen] = mon.full;
        MoveWindowTo(ws.fullscreen, mon.full, true, animate, clip);
    }
    SyncAllOverlays();
}

void WindowManager::RetileAll() {
    for (int m = 0; m < (int)monitors_.size(); ++m) Retile(m);
}

void WindowManager::MoveWindowTo(HWND h, const RECT& r, bool top, bool animate, const RECT* clip) {
    animator_.MoveTo(h, r, top, clip, animate && IsWindowVisible(h));
}

const RECT* WindowManager::SlideClip(int m, POINT offset, LayoutKind layout) const {
    const Monitor& mon = monitors_[m];
    if (layout == LayoutKind::Niri) return &mon.work;
    RECT swept = mon.full;
    OffsetRect(&swept, offset.x, offset.y);
    for (int i = 0; i < (int)monitors_.size(); ++i) {
        RECT overlap;
        if (i != m && IntersectRect(&overlap, &swept, &monitors_[i].full)) return &mon.full;
    }
    return nullptr;
}

RECT WindowManager::EnterWindow(HWND h, POINT move, POINT start, bool animate, const RECT* clip) {
    RECT home;
    if (animator_.Target(h, home)) {
        // Still sliding out and visible; it turns around from where it is.
        if (!animate) animator_.Settle(h);
        OffsetRect(&home, move.x, move.y);
        return home;
    }
    if (!animate) start = {};
    RECT insets{};
    bool parked = animator_.Unpark(h, home, insets);
    if (parked) OffsetRect(&home, move.x, move.y);
    if (IsWindowVisible(h)) {
        if (!parked) {
            home = GetVisibleRect(h);
            OffsetRect(&home, move.x, move.y);
        }
        return home;
    }
    // Position it (and clip it) before showing it, so it never appears anywhere but its start point.
    UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    RECT frame;
    if (parked) {
        frame = home;
        OffsetRect(&frame, start.x, start.y);
        SetWindowPos(h, nullptr, frame.left - insets.left, frame.top - insets.top, 0, 0, flags);
    } else {
        // Hidden windows report no frame bounds, so the window rect stands in for the frame here.
        GetWindowRect(h, &frame);
        OffsetRect(&frame, move.x + start.x, move.y + start.y);
        if (move.x || move.y || start.x || start.y) SetWindowPos(h, nullptr, frame.left, frame.top, 0, 0, flags);
    }
    animator_.Clip(h, frame, clip, &insets);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetDwmTransitions(h, true);
    if (!parked) {
        home = GetVisibleRect(h);
        OffsetRect(&home, -start.x, -start.y);
    }
    return home;
}

POINT WindowManager::SlideOffset(int m, bool forward) const {
    // Slide along an axis with no neighbouring monitor so windows never show up on another screen.
    const RECT& f = monitors_[m].full;
    bool verticalFree = true, horizontalFree = true;
    for (int i = 0; i < (int)monitors_.size(); ++i) {
        if (i == m) continue;
        const RECT& o = monitors_[i].full;
        if (o.left < f.right && o.right > f.left) verticalFree = false;
        if (o.top < f.bottom && o.bottom > f.top) horizontalFree = false;
    }
    int sign = forward ? 1 : -1;
    switch (config_.workspaceSlide) {
        // The offset is where the incoming workspace starts, i.e. opposite to the motion.
        case SlideDirection::Up: return {0, sign * Height(f)};
        case SlideDirection::Down: return {0, -sign * Height(f)};
        case SlideDirection::Left: return {sign * Width(f), 0};
        case SlideDirection::Right: return {-sign * Width(f), 0};
        case SlideDirection::Auto: break;
    }
    if (!verticalFree && horizontalFree) return {sign * Width(f), 0};
    return {0, sign * Height(f)};
}

void WindowManager::UpdateFocusBorder(HWND fg) {
    HWND previous = activeBorder_;
    activeBorder_ = IsManaged(fg) ? fg : nullptr;
    if (previous == activeBorder_) {
        if (activeBorder_) SyncOverlay(activeBorder_);  // re-stack beneath the raised window
        return;
    }
    if (previous && IsManaged(previous)) ApplyDecorations(previous);
    if (activeBorder_) ApplyDecorations(activeBorder_);
    if (previous) SyncOverlay(previous);
    if (activeBorder_) SyncOverlay(activeBorder_);
}

void WindowManager::ApplyAllBorders() {
    HWND fg = GetForegroundWindow();
    activeBorder_ = IsManaged(fg) ? fg : nullptr;
    for (auto& [h, info] : windows_) ApplyDecorations(h);
    SyncAllOverlays();
}

void WindowManager::ApplyDecorations(HWND h) {
    if (config_.borders) SetBorderColor(h, h == activeBorder_ ? config_.borderActive : config_.borderInactive);
    else ResetBorderColor(h);
    SetCornerPreference(h, NativeCornerPreference(config_.borderRadius));
}

bool WindowManager::OverlaysEnabled() const {
    return (config_.borders && config_.borderWidth > 0) ||
           (config_.glow && config_.glowSize > 0 && config_.glowIntensity > 0);
}

int WindowManager::BorderInset(const Monitor& monitor) const {
    if (!config_.borders || config_.borderWidth <= 0) return 0;
    return MulDiv(config_.borderWidth, GetMonitorDpi(monitor.handle), 96);
}

void WindowManager::SyncOverlay(HWND h) {
    auto it = windows_.find(h);
    bool show = OverlaysEnabled() && !paused_ && it != windows_.end() && IsWindow(h) && !IsIconic(h);
    if (show) {
        const WindowInfo& info = it->second;
        const Monitor& mon = monitors_[info.monitor];
        const Workspace& ws = mon.workspaces[info.workspace];
        // Clipped windows are partly off their monitor, and the glow would show on the neighbour.
        show = info.workspace == mon.active && ws.fullscreen != h && !IsZoomed(h) && !animator_.IsClipped(h) &&
               // In monocle only the top window's decoration is visible anyway.
               !(ws.layout == LayoutKind::Monocle && !info.floating && ws.lastFocused && ws.lastFocused != h);
    }

    auto overlay = overlays_.find(h);
    if (!show) {
        if (overlay != overlays_.end()) overlay->second->Hide();
        return;
    }
    if (overlay == overlays_.end()) overlay = overlays_.emplace(h, std::make_unique<BorderOverlay>()).first;
    BorderStyle style = MakeBorderStyle(config_, h == activeBorder_);
    // A rounded frame painted on a square window covers its corners. Follow the square edge instead.
    if (WindowHasSquareCorners(h)) style.radius = style.windowRadius = 0;
    overlay->second->Show(h, style);
}

void WindowManager::SyncAllOverlays() {
    for (auto& [h, info] : windows_) SyncOverlay(h);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void WindowManager::Dispatch(const std::wstring& line) {
    std::wstring s = Trim(line);
    if (s.empty()) return;
    size_t sp = s.find_first_of(L" \t");
    std::wstring verb = ToLower(sp == std::wstring::npos ? s : s.substr(0, sp));
    std::wstring arg = sp == std::wstring::npos ? L"" : Trim(s.substr(sp + 1));
    Log(L"dispatch: %s", s.c_str());

    if (verb == L"pause" || verb == L"togglepause") return TogglePause();
    if (verb == L"quit" || verb == L"exit") return Quit();
    if (verb == L"reload") return Reload();
    if (verb == L"exec") return Exec(arg);
    if (verb == L"settings") {
        Log(L"settings: there is no settings window; edit the config file and dispatch reload");
        return;
    }
    if (verb == L"pulse") {
        // Sent by ObjectPulse when it starts: resend the full state even if it didn't change.
        pulseState_.clear();
        return PublishPulse();
    }
    if (paused_) return;

    Direction dir;
    int count = config_.workspaceCount;
    auto parseWorkspace = [&](int& out) -> bool {
        int current = monitors_[LinkedActive() ? MouseMonitor() : focusedMonitor_].active;
        auto a = ToLower(arg);
        if (a == L"next" || a == L"e+1") out = (current + 1) % count;
        else if (a == L"prev" || a == L"e-1") out = (current + count - 1) % count;
        else {
            int n = _wtoi(arg.c_str());
            if (n < 1 || n > count) return false;
            out = n - 1;
        }
        return true;
    };

    int index = 0;
    if (verb == L"focus" || verb == L"movefocus") {
        if (ParseDirection(arg, dir)) FocusDirection(dir);
    } else if (verb == L"move" || verb == L"movewindow" || verb == L"swap") {
        if (ParseDirection(arg, dir)) MoveDirection(dir);
    } else if (verb == L"workspace") {
        if (parseWorkspace(index)) SwitchWorkspace(focusedMonitor_, index);
    } else if (verb == L"movetoworkspace") {
        if (parseWorkspace(index)) MoveToWorkspace(index, true);
    } else if (verb == L"movetoworkspacesilent") {
        if (parseWorkspace(index)) MoveToWorkspace(index, false);
    } else if (verb == L"layout") {
        LayoutKind kind;
        if (ToLower(arg) == L"next") CycleLayout();
        else if (ParseLayout(arg, kind)) SetLayout(kind);
    } else if (verb == L"togglefloat" || verb == L"togglefloating") {
        ToggleFloat();
    } else if (verb == L"fullscreen") {
        ToggleFullscreen();
    } else if (verb == L"resize") {
        Resize(_wtof(arg.c_str()));
    } else if (verb == L"promote") {
        Promote();
    } else if (verb == L"focusmonitor") {
        if (ParseDirection(arg, dir)) {
            int m = MonitorInDirection(focusedMonitor_, dir);
            if (m >= 0) FocusMonitor(m);
        } else {
            int n = _wtoi(arg.c_str());
            if (n >= 1 && n <= (int)monitors_.size()) FocusMonitor(n - 1);
        }
    } else if (verb == L"movetomonitor") {
        if (ParseDirection(arg, dir)) MoveToMonitor(dir);
    } else if (verb == L"close" || verb == L"killactive") {
        CloseFocused();
    } else if (verb == L"retile") {
        Rescan();
    } else {
        Log(L"unknown command: %s", s.c_str());
    }
    OnStateChanged();
}

HWND WindowManager::CurrentWindow() {
    HWND fg = GetForegroundWindow();
    if (auto it = windows_.find(fg); it != windows_.end()) {
        focusedMonitor_ = it->second.monitor;
        return fg;
    }
    HWND last = monitors_[focusedMonitor_].Active().lastFocused;
    return IsManaged(last) ? last : nullptr;
}

HWND WindowManager::PickFocus(const Workspace& ws) const {
    if (ws.lastFocused && IsManaged(ws.lastFocused) && IsWindow(ws.lastFocused)) return ws.lastFocused;
    if (!ws.tiled.empty()) return ws.tiled.front();
    if (!ws.floating.empty()) return ws.floating.front();
    return nullptr;
}

void WindowManager::UpdateHoverTimer() {
    if (config_.focusFollowsMouse) SetTimer(hwnd_, kHoverTimer, kHoverPollMs, nullptr);
    else KillTimer(hwnd_, kHoverTimer);
    hoverWindow_ = nullptr;
}

void WindowManager::OnHoverTick() {
    POINT p;
    if (!GetCursorPos(&p)) return;
    bool moved = p.x != hoverPos_.x || p.y != hoverPos_.y;
    hoverPos_ = p;
    // Only react to the mouse itself moving, so keyboard focus changes aren't undone by a cursor
    // that happens to rest over another window.
    if (moved) {
        HWND under = WindowFromPoint(p);
        HWND root = under ? GetAncestor(under, GA_ROOT) : nullptr;
        if (root != hoverWindow_) {
            hoverWindow_ = root;
            hoverSince_ = GetTickCount64();
        }
    }
    if (!hoverWindow_ || GetTickCount64() - hoverSince_ < kHoverDelayMs) return;
    HWND h = hoverWindow_;

    if (paused_ || animator_.Busy() || GetTickCount64() < suppressFocusUntil_) return;
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) return;
    hoverWindow_ = nullptr;

    HWND fg = GetForegroundWindow();
    if (h == fg) return;
    auto it = windows_.find(h);
    if (it == windows_.end() || !IsWindowVisible(h)) return;
    if (it->second.workspace != monitors_[it->second.monitor].active) return;

    // Leave menus, drags, dialogs, the Start menu and other popups alone.
    if (fg && !IsManaged(fg)) {
        std::wstring cls = GetWindowClass(fg);
        if (fg != GetShellWindow() && cls != L"Progman" && cls != L"WorkerW") return;
    }
    if (fg) {
        GUITHREADINFO gti{sizeof(gti)};
        if (GetGUIThreadInfo(GetWindowThreadProcessId(fg, nullptr), &gti) &&
            (gti.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE | GUI_SYSTEMMENUMODE))) {
            return;
        }
    }
    FocusWindow(h, false);
}

void WindowManager::FocusWindow(HWND h, bool moveCursor) {
    auto it = windows_.find(h);
    if (it == windows_.end()) return;
    WindowInfo info = it->second;
    Workspace& ws = monitors_[info.monitor].workspaces[info.workspace];
    bool refocused = ws.lastFocused != h;
    ws.lastFocused = h;
    focusedMonitor_ = info.monitor;
    if (ws.layout == LayoutKind::Niri && !info.floating && refocused && info.workspace == monitors_[info.monitor].active) {
        Retile(info.monitor);  // scroll it into view
    }

    ForceForeground(h);
    if (ws.layout == LayoutKind::Monocle || info.floating) {
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (config_.cursorFollowsFocus && moveCursor) {
        RECT r;
        if (!animator_.Target(h, r)) r = GetVisibleRect(h);
        POINT c = Center(r);
        SetCursorPos(c.x, c.y);
    }
    UpdateFocusBorder(h);
}

void WindowManager::FocusDesktop(int m) {
    focusedMonitor_ = m;
    ForceForeground(GetShellWindow());
    POINT c = Center(monitors_[m].work);
    SetCursorPos(c.x, c.y);
    UpdateFocusBorder(nullptr);
}

void WindowManager::ExpectFocus(HWND h) {
    expectedFocus_ = h ? h : GetShellWindow();
    suppressFocusUntil_ = GetTickCount64() + kFocusSuppressMs;
}

HWND WindowManager::FindNeighbor(const RECT& from, Direction dir, const std::vector<HWND>& candidates,
                                 HWND exclude) const {
    HWND best = nullptr;
    long long bestScore = LLONG_MAX;
    POINT fc = Center(from);
    for (HWND c : candidates) {
        if (c == exclude) continue;
        auto it = tileRects_.find(c);
        if (it == tileRects_.end()) continue;
        const RECT& r = it->second;
        POINT cc = Center(r);
        long long gap = 0, overlap = 0, perp = 0;
        switch (dir) {
            case Direction::Left:
                if (cc.x >= fc.x) continue;
                gap = from.left - r.right;
                overlap = std::min(from.bottom, r.bottom) - std::max(from.top, r.top);
                perp = std::abs(cc.y - fc.y);
                break;
            case Direction::Right:
                if (cc.x <= fc.x) continue;
                gap = r.left - from.right;
                overlap = std::min(from.bottom, r.bottom) - std::max(from.top, r.top);
                perp = std::abs(cc.y - fc.y);
                break;
            case Direction::Up:
                if (cc.y >= fc.y) continue;
                gap = from.top - r.bottom;
                overlap = std::min(from.right, r.right) - std::max(from.left, r.left);
                perp = std::abs(cc.x - fc.x);
                break;
            case Direction::Down:
                if (cc.y <= fc.y) continue;
                gap = r.top - from.bottom;
                overlap = std::min(from.right, r.right) - std::max(from.left, r.left);
                perp = std::abs(cc.x - fc.x);
                break;
        }
        long long score = (overlap > 0 ? 0 : 1'000'000'000LL) + std::max(gap, 0LL) * 1000 + perp;
        if (score < bestScore) {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

int WindowManager::MonitorInDirection(int from, Direction dir) const {
    if (from < 0 || from >= (int)monitors_.size()) return -1;
    const RECT& f = monitors_[from].full;
    POINT fc = Center(f);
    int best = -1;
    long long bestScore = LLONG_MAX;
    for (int i = 0; i < (int)monitors_.size(); ++i) {
        if (i == from) continue;
        POINT c = Center(monitors_[i].full);
        long long primary = 0, perp = 0;
        switch (dir) {
            case Direction::Left: primary = fc.x - c.x; perp = std::abs(c.y - fc.y); break;
            case Direction::Right: primary = c.x - fc.x; perp = std::abs(c.y - fc.y); break;
            case Direction::Up: primary = fc.y - c.y; perp = std::abs(c.x - fc.x); break;
            case Direction::Down: primary = c.y - fc.y; perp = std::abs(c.x - fc.x); break;
        }
        if (primary <= 0) continue;
        long long score = primary + perp * 2;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

void WindowManager::FocusDirection(Direction dir) {
    HWND cur = CurrentWindow();
    Workspace& cws = monitors_[focusedMonitor_].Active();

    if (cur && cws.layout == LayoutKind::Monocle && !windows_[cur].floating && cws.tiled.size() > 1) {
        auto it = std::find(cws.tiled.begin(), cws.tiled.end(), cur);
        if (it != cws.tiled.end()) {
            size_t i = it - cws.tiled.begin();
            size_t n = cws.tiled.size();
            bool forward = dir == Direction::Right || dir == Direction::Down;
            FocusWindow(cws.tiled[forward ? (i + 1) % n : (i + n - 1) % n]);
            return;
        }
    }

    if (cur) {
        auto rect = tileRects_.find(cur);
        RECT from = rect != tileRects_.end() ? rect->second : GetVisibleRect(cur);
        if (HWND n = FindNeighbor(from, dir, cws.tiled, cur)) {
            FocusWindow(n);
            return;
        }
    }

    int m = MonitorInDirection(focusedMonitor_, dir);
    if (m >= 0) FocusMonitor(m);
}

void WindowManager::FocusMonitor(int m) {
    if (m < 0 || m >= (int)monitors_.size()) return;
    HWND target = PickFocus(monitors_[m].Active());
    ExpectFocus(target);
    if (target) FocusWindow(target);
    else FocusDesktop(m);
    focusedMonitor_ = m;
}

void WindowManager::MoveDirection(Direction dir) {
    HWND h = CurrentWindow();
    if (!h) return;
    WindowInfo info = windows_[h];

    if (info.floating) {
        RECT r;
        if (!animator_.Target(h, r)) r = GetVisibleRect(h);  // key repeat continues from the destination
        int step = 60;
        int dx = dir == Direction::Left ? -step : dir == Direction::Right ? step : 0;
        int dy = dir == Direction::Up ? -step : dir == Direction::Down ? step : 0;
        OffsetRect(&r, dx, dy);
        MoveWindowTo(h, r, true, config_.animateMove);
        return;
    }

    Workspace& ws = monitors_[info.monitor].workspaces[info.workspace];
    auto self = std::find(ws.tiled.begin(), ws.tiled.end(), h);
    if (self == ws.tiled.end()) return;

    HWND neighbor = nullptr;
    if (ws.layout == LayoutKind::Monocle) {
        size_t i = self - ws.tiled.begin();
        bool forward = dir == Direction::Right || dir == Direction::Down;
        if (forward && i + 1 < ws.tiled.size()) neighbor = ws.tiled[i + 1];
        if (!forward && i > 0) neighbor = ws.tiled[i - 1];
    } else {
        auto rect = tileRects_.find(h);
        RECT from = rect != tileRects_.end() ? rect->second : GetVisibleRect(h);
        neighbor = FindNeighbor(from, dir, ws.tiled, h);
    }

    if (neighbor) {
        std::iter_swap(self, std::find(ws.tiled.begin(), ws.tiled.end(), neighbor));
        Retile(info.monitor);
        UpdateFocusBorder(h);
        return;
    }

    int m = MonitorInDirection(info.monitor, dir);
    if (m < 0) return;
    Detach(h);
    Workspace& tws = monitors_[m].Active();
    if (dir == Direction::Right || dir == Direction::Down) tws.tiled.insert(tws.tiled.begin(), h);
    else tws.tiled.push_back(h);
    windows_[h] = {m, monitors_[m].active, false};
    Retile(info.monitor);
    Retile(m);
    FocusWindow(h);
}

void WindowManager::MoveToMonitor(Direction dir) {
    HWND h = CurrentWindow();
    if (!h) return;
    WindowInfo info = windows_[h];
    int m = MonitorInDirection(info.monitor, dir);
    if (m < 0) return;

    Detach(h);
    Attach(h, m, monitors_[m].active, info.floating, monitors_[m].Active().lastFocused);
    if (info.floating) {
        RECT r;
        if (!animator_.Target(h, r)) r = GetVisibleRect(h);
        const RECT& src = monitors_[info.monitor].work;
        const RECT& dst = monitors_[m].work;
        OffsetRect(&r, dst.left - src.left, dst.top - src.top);
        MoveWindowTo(h, r, true, config_.animateMove);
    }
    Retile(info.monitor);
    Retile(m);
    FocusWindow(h);
}

void WindowManager::SwitchWorkspace(int m, int index) {
    if (LinkedActive()) return LinkedSwitch(MouseMonitor(), index);
    if (m < 0 || m >= (int)monitors_.size()) return;
    Monitor& mon = monitors_[m];
    if (index < 0 || index >= (int)mon.workspaces.size() || index == mon.active) return;

    const int previous = mon.active;
    Workspace& oldWs = mon.workspaces[previous];
    mon.active = index;
    Workspace& newWs = mon.Active();
    focusedMonitor_ = m;

    const bool animate = config_.animateWorkspace;
    // The new workspace comes in from `in`; the old one leaves the opposite way.
    const POINT in = animate ? SlideOffset(m, index > previous) : POINT{};
    const POINT out{-in.x, -in.y};
    const RECT* enterClip = SlideClip(m, in, newWs.layout);
    const RECT* leaveClip = SlideClip(m, out, oldWs.layout);

    for (HWND h : newWs.tiled) EnterWindow(h, {}, in, animate, enterClip);
    for (HWND h : newWs.floating) {
        RECT home = EnterWindow(h, {}, in, animate, enterClip);
        animator_.MoveTo(h, home, false, enterClip, animate);
    }
    Retile(m, animate, enterClip);

    // Move focus before hiding, otherwise Windows activates some random window.
    HWND target = PickFocus(newWs);
    ExpectFocus(target);
    if (target) FocusWindow(target);
    else FocusDesktop(m);

    auto leave = [&](HWND h) {
        if (animate && IsWindowVisible(h)) {
            animator_.SlideOut(h, out, leaveClip);
        } else {
            animator_.Settle(h);
            ShowWindowAsync(h, SW_HIDE);
        }
    };
    for (HWND h : oldWs.tiled) leave(h);
    for (HWND h : oldWs.floating) leave(h);

    SaveHiddenList();
    OnStateChanged();
}

// ---------------------------------------------------------------------------
// Linked workspaces: monitor j (left to right) shows workspace (base + j) mod N, so the
// workspaces form one row that scrolls across all monitors. Workspace k's contents always live
// in the slot k of a single monitor: the one showing it, if any.
// ---------------------------------------------------------------------------

bool WindowManager::LinkedActive() const {
    return config_.linkedWorkspaces && monitors_.size() > 1 && config_.workspaceCount >= (int)monitors_.size();
}

int WindowManager::MouseMonitor() const {
    POINT pt;
    GetCursorPos(&pt);
    int m = MonitorIndex(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
    return m < 0 ? focusedMonitor_ : m;
}

int WindowManager::WorkspaceOwner(int k) const {
    for (int m = 0; m < (int)monitors_.size(); ++m) {
        if (monitors_[m].active == k) return m;
    }
    for (int m = 0; m < (int)monitors_.size(); ++m) {
        const Workspace& ws = monitors_[m].workspaces[k];
        if (!ws.tiled.empty() || !ws.floating.empty()) return m;
    }
    return -1;
}

void WindowManager::Reindex(int m, int k) {
    for (HWND h : monitors_[m].workspaces[k].tiled) windows_[h] = {m, k, false};
    for (HWND h : monitors_[m].workspaces[k].floating) windows_[h] = {m, k, true};
}

void WindowManager::NormalizeLinked() {
    if (!LinkedActive()) return;
    animator_.FinishAll(true);
    const int M = (int)monitors_.size(), N = config_.workspaceCount;
    // Monitor j keeps showing what it shows now, renumbered as workspace j.
    for (int j = 0; j < M; ++j) {
        Monitor& mon = monitors_[j];
        if (mon.active == j) continue;
        std::swap(mon.workspaces[mon.active], mon.workspaces[j]);
        Reindex(j, mon.active);
        Reindex(j, j);
        mon.active = j;
    }
    // Gather every workspace's windows on a single monitor.
    for (int k = 0; k < N; ++k) {
        int owner = k < M ? k : WorkspaceOwner(k);
        if (owner < 0) continue;
        Workspace& dst = monitors_[owner].workspaces[k];
        for (int m = 0; m < M; ++m) {
            if (m == owner) continue;
            Workspace& src = monitors_[m].workspaces[k];
            dst.tiled.insert(dst.tiled.end(), src.tiled.begin(), src.tiled.end());
            dst.floating.insert(dst.floating.end(), src.floating.begin(), src.floating.end());
            src.tiled.clear();
            src.floating.clear();
            src.lastFocused = src.fullscreen = nullptr;
        }
        Reindex(owner, k);
    }
    for (auto& [h, info] : windows_) {
        bool shown = info.workspace == monitors_[info.monitor].active;
        if (shown && !IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        else if (!shown && IsWindowVisible(h)) ShowWindowAsync(h, SW_HIDE);
    }
    RetileAll();
    SaveHiddenList();
}

void WindowManager::LinkedSwitch(int anchor, int k) {
    const int M = (int)monitors_.size(), N = config_.workspaceCount;
    if (anchor < 0 || anchor >= M || k < 0 || k >= N) return;
    std::vector<int> before(M), after(M);
    bool changed = false;
    for (int j = 0; j < M; ++j) {
        before[j] = monitors_[j].active;
        after[j] = ((k + j - anchor) % N + N) % N;
        changed |= before[j] != after[j];
    }
    if (!changed) return;
    // Shortest way round decides which way everything moves.
    int shift = ((after[anchor] - before[anchor]) % N + N) % N;
    if (shift > N / 2) shift -= N;
    const bool forward = shift > 0;

    RECT strip = monitors_[0].full;
    for (auto& mon : monitors_) UnionRect(&strip, &strip, &mon.full);

    // Pull each workspace's contents onto the monitor that will show it.
    std::vector<int> source(M);
    for (int j = 0; j < M; ++j) {
        int owner = WorkspaceOwner(after[j]);
        source[j] = owner < 0 ? j : owner;
    }
    for (int j = 0; j < M; ++j) {
        if (source[j] != j) std::swap(monitors_[j].workspaces[after[j]], monitors_[source[j]].workspaces[after[j]]);
    }
    for (int j = 0; j < M; ++j) {
        monitors_[j].active = after[j];
        Reindex(j, after[j]);
    }

    const bool animate = config_.animateWorkspace;
    for (int j = 0; j < M; ++j) {
        const RECT& to = monitors_[j].work;
        const RECT& from = monitors_[source[j]].work;
        POINT move{to.left - from.left, to.top - from.top};
        // Workspaces that weren't on screen come in from beyond the outermost monitor.
        const RECT& full = monitors_[j].full;
        POINT start{forward ? strip.right - full.left : strip.left - full.right, 0};
        Workspace& ws = monitors_[j].Active();
        for (HWND h : ws.tiled) EnterWindow(h, move, start, animate, nullptr);
        for (HWND h : ws.floating) {
            RECT home = EnterWindow(h, move, start, animate, nullptr);
            animator_.MoveTo(h, home, false, nullptr, animate);
        }
    }
    for (int j = 0; j < M; ++j) Retile(j, animate);

    HWND target = PickFocus(monitors_[anchor].Active());
    ExpectFocus(target);
    if (target) FocusWindow(target);
    else FocusDesktop(anchor);

    // Workspaces that are no longer shown leave past the outermost monitor.
    for (int j = 0; j < M; ++j) {
        if (std::find(after.begin(), after.end(), before[j]) != after.end()) continue;
        const RECT& full = monitors_[j].full;
        POINT out{forward ? strip.left - full.right : strip.right - full.left, 0};
        Workspace& ws = monitors_[j].workspaces[before[j]];
        const RECT* clip = ws.layout == LayoutKind::Niri ? &monitors_[j].work : nullptr;
        auto leave = [&](HWND h) {
            if (animate && IsWindowVisible(h)) {
                animator_.SlideOut(h, out, clip);
            } else {
                animator_.Settle(h);
                ShowWindowAsync(h, SW_HIDE);
            }
        };
        for (HWND h : ws.tiled) leave(h);
        for (HWND h : ws.floating) leave(h);
    }

    SaveHiddenList();
    OnStateChanged();
}

void WindowManager::MoveToWorkspace(int index, bool follow) {
    HWND h = CurrentWindow();
    if (!h) return;
    WindowInfo info = windows_[h];
    Monitor& mon = monitors_[info.monitor];
    if (info.workspace == index || index < 0 || index >= (int)mon.workspaces.size()) return;

    if (LinkedActive()) {
        int owner = WorkspaceOwner(index);
        if (owner >= 0 && owner != info.monitor && monitors_[owner].active == index) {
            // The workspace is on another monitor right now, so the window just moves there.
            Detach(h);
            Attach(h, owner, index, info.floating, monitors_[owner].Active().lastFocused);
            if (info.floating) {
                RECT r;
                if (!animator_.Target(h, r)) r = GetVisibleRect(h);
                OffsetRect(&r, monitors_[owner].work.left - mon.work.left, monitors_[owner].work.top - mon.work.top);
                MoveWindowTo(h, r, true, config_.animateMove);
            }
            Retile(info.monitor);
            Retile(owner);
            if (follow) {
                FocusWindow(h);
            } else {
                HWND next = PickFocus(mon.Active());
                ExpectFocus(next);
                if (next) FocusWindow(next);
                else FocusDesktop(info.monitor);
            }
            return;
        }
        if (owner >= 0 && owner != info.monitor) {
            // Keep the workspace's windows together on the monitor that holds them.
            Detach(h);
            Attach(h, owner, index, info.floating, monitors_[owner].workspaces[index].lastFocused);
            monitors_[owner].workspaces[index].lastFocused = h;
            if (follow) return LinkedSwitch(MouseMonitor(), index);
            HWND next = PickFocus(mon.Active());
            ExpectFocus(next);
            if (next) FocusWindow(next);
            else FocusDesktop(info.monitor);
            animator_.Settle(h);
            ShowWindowAsync(h, SW_HIDE);
            Retile(info.monitor);
            SaveHiddenList();
            return;
        }
    }

    Detach(h);
    Attach(h, info.monitor, index, info.floating, mon.workspaces[index].lastFocused);
    mon.workspaces[index].lastFocused = h;

    if (follow) {
        SwitchWorkspace(info.monitor, index);
        return;
    }

    HWND next = PickFocus(mon.Active());
    ExpectFocus(next);
    if (next) FocusWindow(next);
    else FocusDesktop(info.monitor);
    if (config_.animateWorkspace && IsWindowVisible(h)) {
        POINT offset = SlideOffset(info.monitor, index > info.workspace);
        animator_.SlideOut(h, offset, SlideClip(info.monitor, offset, mon.Active().layout));
    } else {
        animator_.Settle(h);
        ShowWindowAsync(h, SW_HIDE);
    }
    Retile(info.monitor);
    SaveHiddenList();
}

void WindowManager::SetLayout(LayoutKind kind) {
    monitors_[focusedMonitor_].Active().layout = kind;
    Retile(focusedMonitor_);
}

void WindowManager::CycleLayout() {
    Workspace& ws = monitors_[focusedMonitor_].Active();
    switch (ws.layout) {
        case LayoutKind::Dwindle: ws.layout = LayoutKind::MasterStack; break;
        case LayoutKind::MasterStack: ws.layout = LayoutKind::Monocle; break;
        case LayoutKind::Monocle: ws.layout = LayoutKind::Niri; break;
        case LayoutKind::Niri: ws.layout = LayoutKind::Dwindle; break;
    }
    Retile(focusedMonitor_);
}

void WindowManager::ToggleFloat() {
    HWND h = CurrentWindow();
    if (!h) return;
    WindowInfo info = windows_[h];
    Monitor& mon = monitors_[info.monitor];
    Workspace& ws = mon.workspaces[info.workspace];

    Detach(h);
    if (info.floating) {
        Attach(h, info.monitor, info.workspace, false, ws.tiled.empty() ? nullptr : ws.tiled.back());
    } else {
        Attach(h, info.monitor, info.workspace, true, nullptr);
        int w = Width(mon.work) * 3 / 5, hgt = Height(mon.work) * 3 / 5;
        RECT r{mon.work.left + (Width(mon.work) - w) / 2, mon.work.top + (Height(mon.work) - hgt) / 2, 0, 0};
        r.right = r.left + w;
        r.bottom = r.top + hgt;
        MoveWindowTo(h, r, true, config_.animateMove);
    }
    ws.lastFocused = h;
    Retile(info.monitor);
}

void WindowManager::ToggleFullscreen() {
    HWND h = CurrentWindow();
    if (!h) return;
    WindowInfo info = windows_[h];
    if (info.floating) return;
    Workspace& ws = monitors_[info.monitor].workspaces[info.workspace];
    ws.fullscreen = ws.fullscreen == h ? nullptr : h;
    Retile(info.monitor);
}

void WindowManager::Resize(double delta) {
    Workspace& ws = monitors_[focusedMonitor_].Active();
    if (ws.layout == LayoutKind::MasterStack) ws.masterRatio = std::clamp(ws.masterRatio + delta, 0.1, 0.9);
    else if (ws.layout == LayoutKind::Dwindle) ws.dwindleRatio = std::clamp(ws.dwindleRatio + delta, 0.1, 0.9);
    else if (ws.layout == LayoutKind::Niri) ws.columnWidth = std::clamp(ws.columnWidth + delta, 0.2, 1.0);
    Retile(focusedMonitor_);
}

void WindowManager::Promote() {
    HWND h = CurrentWindow();
    if (!h || windows_[h].floating) return;
    Workspace& ws = monitors_[windows_[h].monitor].Active();
    auto it = std::find(ws.tiled.begin(), ws.tiled.end(), h);
    if (it == ws.tiled.end() || ws.tiled.size() < 2) return;
    if (it == ws.tiled.begin()) {
        std::iter_swap(ws.tiled.begin(), ws.tiled.begin() + 1);
    } else {
        ws.tiled.erase(it);
        ws.tiled.insert(ws.tiled.begin(), h);
    }
    Retile(windows_[h].monitor);
}

void WindowManager::CloseFocused() {
    HWND h = GetForegroundWindow();
    if (!h || h == hwnd_ || IsShellWindowClass(GetWindowClass(h))) return;
    PostMessageW(h, WM_CLOSE, 0, 0);
}

void WindowManager::Exec(const std::wstring& command) {
    if (command.empty()) return;
    AllowSetForegroundWindow(ASFW_ANY);

    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    const wchar_t* cwd = (n > 0 && n < MAX_PATH) ? home : nullptr;

    std::wstring buf = command;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }

    // Fall back to the shell for documents, URLs, and App Paths entries.
    std::wstring file, params;
    if (command[0] == L'"') {
        size_t end = command.find(L'"', 1);
        file = command.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
        if (end != std::wstring::npos) params = Trim(command.substr(end + 1));
    } else {
        size_t sp = command.find(L' ');
        file = command.substr(0, sp);
        if (sp != std::wstring::npos) params = Trim(command.substr(sp + 1));
    }
    auto result = (INT_PTR)ShellExecuteW(nullptr, L"open", file.c_str(), params.empty() ? nullptr : params.c_str(),
                                         cwd, SW_SHOWNORMAL);
    if (result <= 32) Log(L"exec failed: %s", command.c_str());
}

void WindowManager::Reload() {
    Config fresh;
    std::vector<std::wstring> warnings;
    if (!LoadConfigFile(configPath_, fresh, warnings)) {
        Notify(L"ObjectFrame", L"Could not read " + configPath_);
        return;
    }
    for (auto& w : warnings) Log(L"config: %s", w.c_str());

    fresh.workspaceCount = config_.workspaceCount;  // workspace vectors are sized at startup
    bool wasLinked = LinkedActive();
    // Workspaces copy these defaults when created, so push changed ones to the existing ones.
    for (Monitor& mon : monitors_) {
        for (Workspace& ws : mon.workspaces) {
            if (fresh.defaultLayout != config_.defaultLayout) ws.layout = fresh.defaultLayout;
            if (fresh.masterRatio != config_.masterRatio) ws.masterRatio = fresh.masterRatio;
            if (fresh.dwindleRatio != config_.dwindleRatio) ws.dwindleRatio = fresh.dwindleRatio;
            if (fresh.niriColumnWidth != config_.niriColumnWidth) ws.columnWidth = fresh.niriColumnWidth;
        }
    }
    config_ = std::move(fresh);
    animator_.SetDuration(config_.animationDuration);
    UpdateHoverTimer();
    ApplyHotkeys();
    ApplyAllBorders();
    if (LinkedActive() && !wasLinked) NormalizeLinked();
    RetileAll();
    RequestPulseUpdate(L"reload");
    Notify(L"ObjectFrame", warnings.empty() ? L"Config reloaded"
                                            : L"Config reloaded with " + std::to_wstring(warnings.size()) +
                                                  L" warning(s). See ofwm.log.");
}

void WindowManager::TogglePause() {
    paused_ = !paused_;
    if (paused_) animator_.FinishAll(true);
    ApplyHotkeys();
    if (paused_) SyncAllOverlays();
    else Rescan();
    OnStateChanged();
    Notify(L"ObjectFrame", paused_ ? L"Paused" : L"Resumed");
}

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------

Workspace WindowManager::MakeWorkspace() const {
    Workspace ws;
    ws.layout = config_.defaultLayout;
    ws.masterRatio = config_.masterRatio;
    ws.dwindleRatio = config_.dwindleRatio;
    ws.columnWidth = config_.niriColumnWidth;
    return ws;
}

int WindowManager::MonitorIndex(HMONITOR handle) const {
    for (int i = 0; i < (int)monitors_.size(); ++i) {
        if (monitors_[i].handle == handle) return i;
    }
    return -1;
}

void WindowManager::EnumerateMonitors() {
    monitors_.clear();
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* self = reinterpret_cast<WindowManager*>(lp);
            MONITORINFO mi{sizeof(mi)};
            if (!GetMonitorInfoW(hm, &mi)) return TRUE;
            Monitor m;
            m.handle = hm;
            m.full = mi.rcMonitor;
            m.work = mi.rcWork;
            m.workspaces.assign(self->config_.workspaceCount, self->MakeWorkspace());
            self->monitors_.push_back(std::move(m));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    std::sort(monitors_.begin(), monitors_.end(), [](const Monitor& a, const Monitor& b) {
        return a.full.left != b.full.left ? a.full.left < b.full.left : a.full.top < b.full.top;
    });
    if (monitors_.empty()) {
        Monitor m;
        m.handle = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &m.work, 0);
        m.full = m.work;
        m.workspaces.assign(config_.workspaceCount, MakeWorkspace());
        monitors_.push_back(std::move(m));
    }
}

void WindowManager::OnDisplayChange() {
    Log(L"display configuration changed");
    animator_.FinishAll(true);
    auto old = std::move(monitors_);
    EnumerateMonitors();

    std::vector<bool> carried(old.size(), false);
    for (auto& nm : monitors_) {
        for (size_t i = 0; i < old.size(); ++i) {
            if (!carried[i] && old[i].handle == nm.handle) {
                nm.workspaces = std::move(old[i].workspaces);
                nm.active = old[i].active;
                carried[i] = true;
                break;
            }
        }
    }

    windows_.clear();
    tileRects_.clear();
    for (int m = 0; m < (int)monitors_.size(); ++m) {
        for (int w = 0; w < (int)monitors_[m].workspaces.size(); ++w) {
            for (HWND h : monitors_[m].workspaces[w].tiled) windows_[h] = {m, w, false};
            for (HWND h : monitors_[m].workspaces[w].floating) windows_[h] = {m, w, true};
        }
    }

    // Windows from monitors that disappeared go to the nearest remaining monitor.
    for (size_t i = 0; i < old.size(); ++i) {
        if (carried[i]) continue;
        for (int w = 0; w < (int)old[i].workspaces.size(); ++w) {
            auto& ows = old[i].workspaces[w];
            auto adopt = [&](HWND h, bool floating) {
                if (!IsWindow(h)) return;
                int m = std::max(0, MonitorIndex(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)));
                int tw = std::min(w, (int)monitors_[m].workspaces.size() - 1);
                Attach(h, m, tw, floating, nullptr);
                if (tw == monitors_[m].active) ShowWindow(h, SW_SHOWNOACTIVATE);
                else ShowWindowAsync(h, SW_HIDE);
            };
            for (HWND h : ows.tiled) adopt(h, false);
            for (HWND h : ows.floating) adopt(h, true);
        }
    }

    if (focusedMonitor_ >= (int)monitors_.size()) focusedMonitor_ = 0;
    NormalizeLinked();
    RetileAll();
    SaveHiddenList();
    OnStateChanged();
}

// ---------------------------------------------------------------------------
// Hotkeys, tray, persistence
// ---------------------------------------------------------------------------

void WindowManager::ApplyHotkeys() { keyboard_.SetBinds(config_.binds, paused_); }

void WindowManager::AddTrayIcon() {
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_OFWM), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    wcscpy_s(nid_.szTip, L"ObjectFrame");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    UpdateTrayTip();
}

void WindowManager::RemoveTrayIcon() {
    if (nid_.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid_);
    nid_.hWnd = nullptr;
}

void WindowManager::UpdateTrayTip() {
    if (!nid_.hWnd || monitors_.empty()) return;
    const Monitor& mon = monitors_[std::clamp(focusedMonitor_, 0, (int)monitors_.size() - 1)];
    swprintf_s(nid_.szTip, L"ObjectFrame | monitor %d | workspace %d | %s%s", focusedMonitor_ + 1, mon.active + 1,
               LayoutName(mon.workspaces[mon.active].layout), paused_ ? L" | paused" : L"");
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void WindowManager::OnStateChanged() {
    UpdateTrayTip();
    RequestPulseUpdate();
}

// ---------------------------------------------------------------------------
// ObjectPulse
// ---------------------------------------------------------------------------

void WindowManager::RequestPulseUpdate(const wchar_t* event) {
    if (event) pulseEvent_ = event;
    if (pulsePending_ || !hwnd_) return;
    pulsePending_ = true;
    SetTimer(hwnd_, kPulseTimer, kPulseDelayMs, nullptr);
}

void WindowManager::PublishPulse() {
    KillTimer(hwnd_, kPulseTimer);
    pulsePending_ = false;
    if (shutdown_ || !FindWindowW(kPulseWindowClass, nullptr)) {
        pulseState_.clear();
        pulseEvent_.clear();
        return;
    }
    std::wstring state = PulseState();
    if (state == pulseState_ && pulseEvent_.empty()) return;
    pulseState_ = state;
    if (!pulseEvent_.empty()) state += L"event=" + pulseEvent_ + L"\n";
    pulseEvent_.clear();
    SendPulse(state);
}

std::wstring WindowManager::PulseState() const {
    if (monitors_.empty()) return L"of=1\n";
    const int fm = std::clamp(focusedMonitor_, 0, (int)monitors_.size() - 1);
    const Monitor& mon = monitors_[fm];
    const Workspace& ws = mon.workspaces[mon.active];
    const bool linked = LinkedActive();

    // Linked workspaces span all monitors; independent ones belong to the focused monitor.
    std::wstring occupied, visible;
    for (int k = 0; k < (int)mon.workspaces.size(); ++k) {
        bool used = false, shown = false;
        for (int m = 0; m < (int)monitors_.size(); ++m) {
            if (!linked && m != fm) continue;
            const Workspace& w = monitors_[m].workspaces[k];
            used |= !w.tiled.empty() || !w.floating.empty();
            shown |= monitors_[m].active == k;
        }
        auto add = [k](std::wstring& list) { list += (list.empty() ? L"" : L",") + std::to_wstring(k + 1); };
        if (used) add(occupied);
        if (shown) add(visible);
    }

    HWND fg = GetForegroundWindow();
    bool app = fg && fg != hwnd_ && !IsShellWindowClass(GetWindowClass(fg));
    auto info = windows_.find(fg);
    std::wstring title, exe;
    if (app) {
        title = GetWindowTitle(fg);
        std::replace(title.begin(), title.end(), L'\n', L' ');
        std::replace(title.begin(), title.end(), L'\r', L' ');
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        exe = GetProcessExeName(pid);
    }

    wchar_t accent[16];
    swprintf_s(accent, L"0x%02X%02X%02X", GetRValue(config_.borderActive), GetGValue(config_.borderActive),
               GetBValue(config_.borderActive));

    std::wstring s = L"of=1\n";
    s += L"monitor=" + std::to_wstring(fm + 1) + L"\n";
    s += L"monitors=" + std::to_wstring(monitors_.size()) + L"\n";
    s += L"workspace=" + std::to_wstring(mon.active + 1) + L"\n";
    s += L"workspaces=" + std::to_wstring(mon.workspaces.size()) + L"\n";
    s += L"occupied=" + occupied + L"\n";
    s += L"visible=" + visible + L"\n";
    s += L"layout=" + std::wstring(LayoutName(ws.layout)) + L"\n";
    // What every monitor shows, in monitor order, for displays that sit on each monitor.
    std::wstring monitorWorkspaces, monitorLayouts;
    for (const Monitor& m : monitors_) {
        const wchar_t* sep = monitorWorkspaces.empty() ? L"" : L",";
        monitorWorkspaces += sep + std::to_wstring(m.active + 1);
        monitorLayouts += sep + std::wstring(LayoutName(m.workspaces[m.active].layout));
    }
    s += L"monitor_workspaces=" + monitorWorkspaces + L"\n";
    s += L"monitor_layouts=" + monitorLayouts + L"\n";
    s += L"windows=" + std::to_wstring(ws.tiled.size() + ws.floating.size()) + L"\n";
    s += std::wstring(L"paused=") + (paused_ ? L"1" : L"0") + L"\n";
    s += std::wstring(L"managed=") + (info != windows_.end() ? L"1" : L"0") + L"\n";
    s += std::wstring(L"floating=") + (info != windows_.end() && info->second.floating ? L"1" : L"0") + L"\n";
    s += std::wstring(L"fullscreen=") + (app && ws.fullscreen == fg ? L"1" : L"0") + L"\n";
    s += L"hwnd=" + std::to_wstring(app ? (unsigned long long)(UINT_PTR)fg : 0ull) + L"\n";
    s += L"exe=" + exe + L"\n";
    s += L"title=" + title + L"\n";
    s += std::wstring(L"accent=") + accent + L"\n";
    return s;
}

void WindowManager::SendPulse(const std::wstring& text) {
    HWND pulse = FindWindowW(kPulseWindowClass, nullptr);
    if (!pulse) return;
    COPYDATASTRUCT cds{};
    cds.dwData = kPulseCopyDataMagic;
    cds.cbData = (DWORD)((text.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(text.c_str());
    DWORD_PTR result = 0;
    SendMessageTimeoutW(pulse, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd_), reinterpret_cast<LPARAM>(&cds),
                        SMTO_ABORTIFHUNG, 200, &result);
}

void WindowManager::Notify(const std::wstring& title, const std::wstring& text) {
    if (!nid_.hWnd) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void WindowManager::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (paused_ ? MF_CHECKED : 0), kTrayPause, L"一時停止 (Pause)");
    AppendMenuW(menu, MF_STRING, kTrayRetile, L"再タイル (Retile)");
    AppendMenuW(menu, MF_STRING, kTrayReload, L"設定を再読み込み (Reload config)");
    AppendMenuW(menu, MF_STRING, kTrayEditConfig, L"設定ファイルを開く (Edit config)");
    AppendMenuW(menu, MF_STRING, kTrayOpenLog, L"ログを開く (Open log)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayQuit, L"終了 (Quit)");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    std::wstring logPath = std::filesystem::path(configPath_).parent_path().wstring() + L"\\ofwm.log";
    switch (cmd) {
        case kTrayPause: TogglePause(); break;
        case kTrayRetile: Rescan(); break;
        case kTrayReload: Reload(); break;
        case kTrayEditConfig:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + configPath_ + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
            break;
        case kTrayOpenLog:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + logPath + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
            break;
        case kTrayQuit: Quit(); break;
    }
}

std::wstring WindowManager::HiddenListPath() const {
    return std::filesystem::path(configPath_).parent_path().wstring() + L"\\hidden.txt";
}

void WindowManager::SaveHiddenList() {
    // Lets the next run un-hide windows if this process is killed while workspaces are hidden.
    FILE* f = nullptr;
    if (_wfopen_s(&f, HiddenListPath().c_str(), L"w, ccs=UTF-8") != 0 || !f) return;
    for (auto& [h, info] : windows_) {
        if (info.workspace != monitors_[info.monitor].active) {
            fwprintf(f, L"%llu\t%s\n", (unsigned long long)(UINT_PTR)h, GetWindowClass(h).c_str());
        }
    }
    fclose(f);
}

void WindowManager::RestoreHiddenFromPreviousRun() {
    FILE* f = nullptr;
    std::wstring path = HiddenListPath();
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") != 0 || !f) return;
    wchar_t line[512];
    int restored = 0;
    while (fgetws(line, 512, f)) {
        wchar_t* tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = L'\0';
        std::wstring cls = Trim(tab + 1);
        HWND h = (HWND)(UINT_PTR)_wcstoui64(line, nullptr, 10);
        if (IsWindow(h) && !IsWindowVisible(h) && GetWindowClass(h) == cls) {
            ShowWindow(h, SW_SHOWNOACTIVATE);
            ++restored;
        }
    }
    fclose(f);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (restored) Log(L"restored %d window(s) hidden by a previous run", restored);
}

}  // namespace of
