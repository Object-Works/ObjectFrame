#pragma once

#include "animator.h"
#include "border_overlay.h"
#include "common.h"
#include "config.h"
#include "keyboard_hook.h"

#include <shellapi.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace of {

struct Workspace {
    std::vector<HWND> tiled;
    std::vector<HWND> floating;
    LayoutKind layout = LayoutKind::Dwindle;
    double masterRatio = 0.55;
    double dwindleRatio = 0.5;
    double columnWidth = 0.5;  // niri
    int scroll = 0;            // niri strip offset in pixels
    HWND lastFocused = nullptr;
    HWND fullscreen = nullptr;
};

struct Monitor {
    HMONITOR handle = nullptr;
    RECT full{};
    RECT work{};
    std::vector<Workspace> workspaces;
    int active = 0;

    Workspace& Active() { return workspaces[active]; }
};

struct WindowInfo {
    int monitor = 0;
    int workspace = 0;
    bool floating = false;
};

class WindowManager {
public:
    static WindowManager& Instance();

    bool Init(HINSTANCE instance, const std::wstring& configPath);
    int Run();
    void Shutdown();
    void RestoreAllWindows();

private:
    WindowManager() = default;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild,
                                      DWORD thread, DWORD time);

    // Window tracking
    void OnWinEvent(DWORD event, HWND hwnd);
    void OnForeground(HWND hwnd);
    void OnMoveSizeEnd(HWND hwnd);
    bool TryManage(HWND hwnd, bool retile);
    void Unmanage(HWND hwnd, bool retile);
    void Attach(HWND hwnd, int monitor, int workspace, bool floating, HWND after);
    void Detach(HWND hwnd);
    void PruneDead();
    void Rescan();

    // Layout
    void Retile(int monitor, bool forceAnimate = false, const RECT* clip = nullptr);
    void RetileAll();
    void MoveWindowTo(HWND hwnd, const RECT& rect, bool bringToTop, bool animate, const RECT* clip = nullptr);
    // Direction workspaces slide in from when going forward (towards higher indices).
    POINT SlideOffset(int monitor, bool forward) const;
    // Area to clip a workspace sliding by `offset` to, or null if it never touches another monitor.
    const RECT* SlideClip(int monitor, POINT offset, LayoutKind layout) const;
    // Makes a workspace window visible again at its home shifted by `move`, starting `start` further
    // away when animating. Returns that resting position.
    RECT EnterWindow(HWND hwnd, POINT move, POINT start, bool animate, const RECT* clip);

    // Linked workspaces
    bool LinkedActive() const;
    int MouseMonitor() const;
    int WorkspaceOwner(int workspace) const;
    void Reindex(int monitor, int workspace);
    void NormalizeLinked();
    void LinkedSwitch(int anchorMonitor, int workspace);
    void UpdateFocusBorder(HWND foreground);
    void ApplyAllBorders();
    void ApplyDecorations(HWND hwnd);
    void SyncOverlay(HWND hwnd);
    void SyncAllOverlays();
    bool OverlaysEnabled() const;
    int BorderInset(const Monitor& monitor) const;

    // Commands
    void Dispatch(const std::wstring& command);
    void FocusDirection(Direction dir);
    void MoveDirection(Direction dir);
    void SwitchWorkspace(int monitor, int index);
    void MoveToWorkspace(int index, bool follow);
    void SetLayout(LayoutKind kind);
    void CycleLayout();
    void ToggleFloat();
    void ToggleFullscreen();
    void Resize(double delta);
    void Promote();
    void FocusMonitor(int monitor);
    void MoveToMonitor(Direction dir);
    void CloseFocused();
    void Exec(const std::wstring& command);
    void Reload();
    void TogglePause();
    void OpenSettings();
    void Quit();

    // Helpers
    HWND CurrentWindow();
    HWND PickFocus(const Workspace& ws) const;
    void FocusWindow(HWND hwnd, bool moveCursor = true);
    void UpdateHoverTimer();
    void OnHoverTick();
    void FocusDesktop(int monitor);
    void ExpectFocus(HWND hwnd);
    HWND FindNeighbor(const RECT& from, Direction dir, const std::vector<HWND>& candidates, HWND exclude) const;
    int MonitorInDirection(int from, Direction dir) const;
    int MonitorIndex(HMONITOR handle) const;
    void EnumerateMonitors();
    void OnDisplayChange();
    Workspace MakeWorkspace() const;
    bool IsManaged(HWND hwnd) const { return windows_.count(hwnd) != 0; }

    // Hotkeys, tray, persistence
    void ApplyHotkeys();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayTip();
    void ShowTrayMenu();
    void Notify(const std::wstring& title, const std::wstring& text);
    std::wstring HiddenListPath() const;
    void SaveHiddenList();
    void RestoreHiddenFromPreviousRun();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    std::vector<HWINEVENTHOOK> hooks_;
    KeyboardHook keyboard_;
    Animator animator_;

    Config config_;
    std::wstring configPath_;

    std::vector<Monitor> monitors_;
    std::unordered_map<HWND, WindowInfo> windows_;
    std::unordered_map<HWND, RECT> tileRects_;
    std::unordered_map<HWND, std::unique_ptr<BorderOverlay>> overlays_;
    std::deque<std::wstring> pendingCommands_;

    int focusedMonitor_ = 0;
    HWND activeBorder_ = nullptr;
    HWND expectedFocus_ = nullptr;
    ULONGLONG suppressFocusUntil_ = 0;
    // Focus follows mouse
    POINT hoverPos_{LONG_MIN, LONG_MIN};
    HWND hoverWindow_ = nullptr;
    ULONGLONG hoverSince_ = 0;
    bool paused_ = false;
    bool selfElevated_ = false;
    bool shutdown_ = false;
    NOTIFYICONDATAW nid_{};
};

}  // namespace of
