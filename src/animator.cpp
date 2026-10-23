#include "animator.h"

#include "window_utils.h"

#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace of {

namespace {

double EaseOutCubic(double p) {
    double q = 1.0 - p;
    return 1.0 - q * q * q;
}

RECT Lerp(const RECT& a, const RECT& b, double t) {
    auto mix = [t](LONG x, LONG y) { return (LONG)std::lround(x + (y - x) * t); };
    return {mix(a.left, b.left), mix(a.top, b.top), mix(a.right, b.right), mix(a.bottom, b.bottom)};
}

}  // namespace

void Animator::Init(HWND timerWindow, UINT_PTR timerId) {
    timerWindow_ = timerWindow;
    timerId_ = timerId;
}

Animator::Anim Animator::Make(HWND h, const RECT& from, const RECT& to, const RECT& home, bool top, bool slideOut,
                              const RECT* clip) {
    Anim a;
    a.from = from;
    a.to = to;
    a.home = home;
    a.top = top;
    a.slideOut = slideOut;
    a.start = Clock::now();
    a.insets = GetFrameInsets(h);
    a.hasClip = clip != nullptr;
    if (clip) a.clip = *clip;
    return a;
}

void Animator::MoveTo(HWND h, const RECT& to, bool top, const RECT* clip, bool animate) {
    if (!IsWindow(h)) return;
    auto it = anims_.find(h);
    if (animate && it != anims_.end() && !it->second.slideOut && EqualRect(&it->second.to, &to)) {
        it->second.top |= top;
        it->second.hasClip = clip != nullptr;
        if (clip) it->second.clip = *clip;
        return;
    }
    RECT from = GetVisibleRect(h);
    if (!animate || IsZoomed(h) || IsIconic(h) || EqualRect(&from, &to) || durationMs_ <= 0) {
        if (it != anims_.end()) anims_.erase(it);
        PlaceWindow(h, to, top);
        Clip(h, to, clip);
        return;
    }
    if (!clip && clipped_.erase(h)) UnclipWindow(h);
    anims_[h] = Make(h, from, to, to, top, false, clip);
    StartTimer();
}

void Animator::SlideOut(HWND h, POINT offset, const RECT* clip) {
    if (!IsWindow(h)) return;
    RECT home;
    if (!Target(h, home)) home = GetVisibleRect(h);
    RECT to = home;
    OffsetRect(&to, offset.x, offset.y);
    if (!clip && clipped_.erase(h)) UnclipWindow(h);
    anims_[h] = Make(h, GetVisibleRect(h), to, home, false, true, clip);
    parked_.erase(h);
    StartTimer();
}

bool Animator::Target(HWND h, RECT& out) const {
    auto it = anims_.find(h);
    if (it == anims_.end()) return false;
    out = it->second.slideOut ? it->second.home : it->second.to;
    return true;
}

bool Animator::Unpark(HWND h, RECT& home, RECT& insets) {
    auto it = parked_.find(h);
    if (it == parked_.end()) return false;
    home = it->second.home;
    insets = it->second.insets;
    parked_.erase(it);
    return true;
}

bool Animator::PeekParked(HWND h, RECT& home) const {
    auto it = parked_.find(h);
    if (it == parked_.end()) return false;
    home = it->second.home;
    return true;
}

void Animator::Clip(HWND h, const RECT& frame, const RECT* clip, const RECT* insets) {
    if (!clip) {
        if (clipped_.erase(h)) UnclipWindow(h);
        return;
    }
    if (ClipWindow(h, frame, *clip, insets ? *insets : GetFrameInsets(h))) clipped_.insert(h);
    else clipped_.erase(h);
}

void Animator::Cancel(HWND h) {
    anims_.erase(h);
    parked_.erase(h);
    if (clipped_.erase(h) && IsWindow(h)) UnclipWindow(h);
}

void Animator::Settle(HWND h) {
    auto it = anims_.find(h);
    if (it == anims_.end()) return;
    Anim a = it->second;
    anims_.erase(it);
    if (IsWindow(h)) Finish(h, a, false);
}

void Animator::FinishAll(bool hide) {
    auto anims = std::move(anims_);
    anims_.clear();
    for (auto& [h, a] : anims) {
        if (IsWindow(h)) Finish(h, a, hide);
    }
    StopTimer();
}

void Animator::UnclipAll() {
    for (HWND h : clipped_) {
        if (IsWindow(h)) UnclipWindow(h);
    }
    clipped_.clear();
}

void Animator::Frame(HWND h, const Anim& a, const RECT& r) {
    if (!a.hasClip) {
        PlaceWindowFrame(h, r, a.top, true);
        return;
    }
    // The region is relative to the window, so move synchronously to keep the two in step, and
    // narrow it first so neither position shows outside the clip while the move lands.
    RECT now;
    if (GetWindowRect(h, &now) && ClipWindow(h, r, a.clip, a.insets, &now)) clipped_.insert(h);
    PlaceWindowFrame(h, r, a.top, IsHungAppWindow(h) != FALSE);
    Clip(h, r, &a.clip, &a.insets);
}

void Animator::Finish(HWND h, const Anim& a, bool hide) {
    if (!a.slideOut) {
        PlaceWindow(h, a.to, a.top);
        Clip(h, a.to, a.hasClip ? &a.clip : nullptr, &a.insets);
    } else if (hide && shouldHide && shouldHide(h)) {
        // Hide first, then move the hidden window home so it reappears there. The DWM hide
        // transition would otherwise draw a fading copy of the window at its home position.
        SetDwmTransitions(h, false);
        int x = a.home.left - a.insets.left, y = a.home.top - a.insets.top;
        UINT flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (IsHungAppWindow(h)) {
            ShowWindowAsync(h, SW_HIDE);
            SetWindowPos(h, nullptr, x, y, 0, 0, flags | SWP_ASYNCWINDOWPOS);
        } else {
            // Synchronous, so a quick switch back can't overtake a still-queued hide or move.
            ShowWindow(h, SW_HIDE);
            SetWindowPos(h, nullptr, x, y, 0, 0, flags);
        }
        parked_[h] = {a.home, a.insets};
        // Keep the region of windows that were clipped at home (e.g. off-screen niri columns).
        Clip(h, a.home, a.hasClip ? &a.clip : nullptr, &a.insets);
    } else {
        PlaceWindow(h, a.home, false);
        Clip(h, a.home, a.hasClip ? &a.clip : nullptr, &a.insets);
    }
    if (onFinished) onFinished(h);
}

void Animator::Tick() {
    auto now = Clock::now();
    std::vector<std::pair<HWND, Anim>> done;
    for (auto it = anims_.begin(); it != anims_.end();) {
        HWND h = it->first;
        Anim& a = it->second;
        if (!IsWindow(h)) {
            clipped_.erase(h);
            it = anims_.erase(it);
            continue;
        }
        double elapsed = std::chrono::duration<double, std::milli>(now - a.start).count();
        double p = std::clamp(elapsed / std::max(1, durationMs_), 0.0, 1.0);
        if (p >= 1.0) {
            done.emplace_back(h, a);
            it = anims_.erase(it);
            continue;
        }
        Frame(h, a, Lerp(a.from, a.to, EaseOutCubic(p)));
        a.top = false;
        ++it;
    }
    // Finishing can call back into the window manager, so do it after iterating.
    for (auto& [h, a] : done) Finish(h, a, true);
    if (anims_.empty()) StopTimer();
}

void Animator::StartTimer() {
    if (timerRunning_ || !timerWindow_) return;
    // The default 15.6 ms tick makes animations visibly choppy.
    timeBeginPeriod(1);
    SetTimer(timerWindow_, timerId_, USER_TIMER_MINIMUM, nullptr);
    timerRunning_ = true;
}

void Animator::StopTimer() {
    if (!timerRunning_) return;
    KillTimer(timerWindow_, timerId_);
    timeEndPeriod(1);
    timerRunning_ = false;
}

}  // namespace of
