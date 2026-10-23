#pragma once

#include "common.h"

#include <chrono>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace of {

// Moves windows over time by repositioning them on a timer. All rects are visible frames.
// A `clip` rect limits what is visible of a window (through a window region) so it never
// shows outside that area, e.g. on a neighbouring monitor.
class Animator {
public:
    void Init(HWND timerWindow, UINT_PTR timerId);
    void SetDuration(int ms) { durationMs_ = ms; }

    // Glides `hwnd` from where it is now to `to`, or places it right away if `animate` is false.
    void MoveTo(HWND hwnd, const RECT& to, bool bringToTop, const RECT* clip = nullptr, bool animate = true);
    // Slides `hwnd` away by `offset`; when it arrives it is hidden (if `shouldHide` agrees)
    // and put back at its original position so it reappears there.
    void SlideOut(HWND hwnd, POINT offset, const RECT* clip = nullptr);
    // Where an animating window is meant to end up (its original spot for slide-outs).
    bool Target(HWND hwnd, RECT& out) const;
    // For a window hidden by a finished slide-out: its home rect and its frame insets
    // (which can't be measured while it's hidden).
    bool Unpark(HWND hwnd, RECT& home, RECT& insets);
    bool PeekParked(HWND hwnd, RECT& home) const;
    // Clips a window that is not animating, e.g. before showing it at an animation's start point.
    void Clip(HWND hwnd, const RECT& frame, const RECT* clip, const RECT* insets = nullptr);
    bool IsClipped(HWND hwnd) const { return clipped_.count(hwnd) != 0; }
    bool Busy() const { return !anims_.empty(); }
    // Stops animating without moving the window, and removes any clipping.
    void Cancel(HWND hwnd);
    // Stops animating and puts the window at its target right away.
    void Settle(HWND hwnd);
    // Completes every animation immediately; slide-outs are only hidden when `hide` is set.
    void FinishAll(bool hide);
    void UnclipAll();
    void Tick();

    std::function<bool(HWND)> shouldHide;
    std::function<void(HWND)> onFinished;

private:
    using Clock = std::chrono::steady_clock;
    struct Anim {
        RECT from{}, to{}, home{};
        bool top = false;
        bool slideOut = false;
        Clock::time_point start;
        RECT insets{};
        bool hasClip = false;
        RECT clip{};
    };
    struct Parked {
        RECT home{};
        RECT insets{};
    };

    Anim Make(HWND hwnd, const RECT& from, const RECT& to, const RECT& home, bool top, bool slideOut,
              const RECT* clip);
    void Frame(HWND hwnd, const Anim& anim, const RECT& rect);
    void Finish(HWND hwnd, const Anim& anim, bool hide);
    void StartTimer();
    void StopTimer();

    HWND timerWindow_ = nullptr;
    UINT_PTR timerId_ = 0;
    bool timerRunning_ = false;
    int durationMs_ = 200;
    std::unordered_map<HWND, Anim> anims_;
    std::unordered_map<HWND, Parked> parked_;
    std::unordered_set<HWND> clipped_;
};

}  // namespace of
