// Tray icon click routing, fallback order and classic toolbar mouse gestures.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

namespace atrium::tray {
enum class Action { Left, Right, Double, DoubleContinuation };
enum class Route { Invoke, ContextMenu, LegacyDefault, Toolbar };
enum class Outcome { Observed, AcceptedUnconfirmed, Unavailable };
inline constexpr int32_t kProviderTimeout = static_cast<int32_t>(0x80131505u); // UIA_E_TIMEOUT
struct Attempt {
    Route route;
    int32_t status;
    bool observed;
};
struct DispatchResult {
    Outcome outcome = Outcome::Unavailable;
    std::vector<Attempt> attempts;
};
enum class MouseEvent { Move, LeftDown, LeftUp, LeftDouble, RightDown, RightUp };
struct Posting {
    bool complete = false, actionQueued = false;
    size_t queued = 0;
};
template<class Post> Posting PostClassicGesture(Action action, Post post)
{
    std::vector<MouseEvent> events{MouseEvent::Move};
    if (action == Action::DoubleContinuation)
        events.insert(events.end(), {MouseEvent::LeftDouble, MouseEvent::LeftUp});
    else if (action == Action::Right)
        events.insert(events.end(), {MouseEvent::RightDown, MouseEvent::RightUp});
    else {
        events.insert(events.end(), {MouseEvent::LeftDown, MouseEvent::LeftUp});
        if (action == Action::Double)
            events.insert(events.end(), {MouseEvent::LeftDouble, MouseEvent::LeftUp});
    }
    Posting result;
    for (MouseEvent event : events) {
        if (!post(event))
            return result;
        ++result.queued;
        if (event == MouseEvent::LeftDown || event == MouseEvent::RightDown || event == MouseEvent::LeftDouble)
            result.actionQueued = true;
    }
    result.complete = true;
    return result;
}
// An accepted asynchronous action must not be repeated through another API: it may toggle
// the UI back, or deliver a destructive second command. Missing evidence is not failure.
template<class Invoke, class Observe> DispatchResult Dispatch(Action action, bool classic, Invoke invoke, Observe observe)
{
    DispatchResult result;
    std::vector<Route> routes;
    if (action == Action::DoubleContinuation && !classic) {
        // The XAML tray has no double-click pattern, but the shell pairs a second Invoke
        // within the double-click time with the first one (#49). One attempt, no fallback.
        routes.push_back(Route::Invoke);
    } else if (action == Action::Double || action == Action::DoubleContinuation) {
        if (classic)
            routes.push_back(Route::Toolbar); // Invoke twice is NOT a double click
    } else {
        routes.push_back(action == Action::Right ? Route::ContextMenu : Route::Invoke);
        if (classic)
            routes.push_back(Route::Toolbar);
        if (action == Action::Left)
            routes.push_back(Route::LegacyDefault);
    }
    for (Route route : routes) {
        const int32_t status = invoke(route);
        const bool uncertain = status >= 0 || status == kProviderTimeout;
        const bool observed = observe(uncertain);
        result.attempts.push_back({route, status, observed});
        if (observed) {
            result.outcome = Outcome::Observed;
            break;
        }
        if (uncertain) {
            result.outcome = Outcome::AcceptedUnconfirmed;
            break;
        }
    }
    return result;
}

// Permit XAML ShowContextMenu returning S_OK without observable menu evidence (#28),
// while blocking provider timeouts, classic toolbars/S_FALSE, and already-observed outcomes.
inline bool ShouldAttemptPointerRightFallback(const DispatchResult& result, bool classic)
{
    if (classic || result.outcome == Outcome::Observed)
        return false;
    if (result.outcome == Outcome::Unavailable)
        return true;
    return !result.attempts.empty() && result.attempts.back().route == Route::ContextMenu && result.attempts.back().status == 0 &&
           !result.attempts.back().observed;
}

// Next to the click: above and to the right of it, else below or to the left; in `work`.
inline POINT PlaceMenu(POINT at, int w, int h, const RECT& work)
{
    int left = at.x + w <= work.right ? at.x : at.x - w;
    int top = at.y - h >= work.top ? at.y - h : at.y;
    left = std::clamp(left, int(work.left), std::max(int(work.left), int(work.right) - w));
    top = std::clamp(top, int(work.top), std::max(int(work.top), int(work.bottom) - h));
    return POINT{left, top};
}

inline bool ExtendsBeyond(const RECT& r, const RECT& bounds, int tolerance)
{
    return r.left < bounds.left - tolerance || r.top < bounds.top - tolerance || r.right > bounds.right + tolerance || r.bottom > bounds.bottom + tolerance;
}

// True when a newly shown menu-shaped popup should be moved next to `at`: either the app did not
// place it near the pointer (+-48 px), or it extends off-screen (native #32768 allows 8 px of
// shadow past the monitor; custom popups also check 8 px past the work area).
inline bool ShouldMoveFollowedMenu(const RECT& r, POINT at, const RECT& monitor, const RECT& work, bool nativeMenu)
{
    if (at.x >= r.left - 48 && at.x <= r.right + 48 && at.y >= r.top - 48 && at.y <= r.bottom + 48)
        return nativeMenu ? ExtendsBeyond(r, monitor, 8) : (ExtendsBeyond(r, monitor, 0) || ExtendsBeyond(r, work, 8));
    return true;
}

inline DWORD RecordPid(std::wstring_view recordId)
{
    if (!recordId.starts_with(L"uid:"))
        return 0;
    const std::wstring_view rest = recordId.substr(4);
    const size_t colon = rest.find(L':');
    if (colon == 0 || colon == std::wstring_view::npos)
        return 0;
    uint64_t pid = 0;
    for (wchar_t ch : rest.substr(0, colon)) {
        if (ch < L'0' || ch > L'9')
            return 0;
        pid = pid * 10 + static_cast<uint64_t>(ch - L'0');
        if (pid > UINT32_MAX)
            return 0;
    }
    return pid > 0 ? static_cast<DWORD>(pid) : 0;
}

inline bool ShouldObserveUnattributedMenu(bool nativeMenu, bool accessDenied, bool rightAction, bool popupStyle)
{
    return nativeMenu || (rightAction && accessDenied && popupStyle);
}

inline bool IsHoverPopupStyle(LONG_PTR style, LONG_PTR exStyle, bool nativeMenu, const RECT& rect, const RECT& monitorWork)
{
    if ((exStyle & WS_EX_APPWINDOW) != 0 || (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) != 0)
        return false;
    const LONG w = rect.right - rect.left;
    const LONG h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0)
        return false;
    const LONG workW = monitorWork.right - monitorWork.left;
    const LONG workH = monitorWork.bottom - monitorWork.top;
    if (workW > 0 && workH > 0 && (w > workW * 3 / 5 || h > workH * 3 / 4))
        return false;
    return nativeMenu || (style & WS_POPUP) != 0 || (exStyle & (WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) != 0 || (style & WS_CAPTION) != WS_CAPTION;
}
} // namespace atrium::tray
