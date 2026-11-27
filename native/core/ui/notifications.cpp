#include "ui/notifications.h"

#include "clock.h"
#include "log.h"
#include "shell/displays.h"
#include "shell/games.h"
#include "ui/notification_layout.h"
#include "win32_path.h"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace atrium {
namespace {

constexpr int kMaxNotices = 50;
constexpr ULONGLONG kMergeMs = 120000;
constexpr UINT_PTR kSweepTimer = 2, kHoldTimer = 3;
constexpr UINT kHoldMs = 250;

bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

} // namespace

Notifications::Notifications(HINSTANCE instance)
{
    popup_.surface = std::make_unique<Surface>(instance, L"Atrium Notification", false);
    center_.surface = std::make_unique<Surface>(instance, L"Atrium Notifications", true);
    for (Mode mode : {Mode::Popup, Mode::Center}) {
        Surface& s = *ViewOf(mode).surface;
        s.paint = [this, mode](Canvas& c) { Paint(mode, c); };
        s.message = [this, mode](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(mode, m, w, l, r); };
    }
    // Expanding the stack: the popup stays until the list's glass is there (no empty frame).
    center_.surface->onReady = [this] {
        if (handoffPopup_) {
            handoffPopup_ = false;
            popup_.surface->Hide(false);
        }
    };
    // A press next to the list closes it.
    center_.surface->onDismiss = [this] { center_.surface->Hide(), centerClosed_ = GetTickCount64(); };
}

void Notifications::SetOptions(double timeoutSeconds, bool hideBody, const std::wstring& placement, bool notifyFullscreen, bool notifyAlways)
{
    // A corner of the main screen (topLeft, topRight, bottomLeft, bottomRight), else over the bar.
    const bool corner = placement == L"topLeft" || placement == L"topRight" || placement == L"bottomLeft" || placement == L"bottomRight";
    const bool left = corner && placement.ends_with(L"Left"), bottom = corner && placement.starts_with(L"bottom");
    const bool placementChanged = corner != corner_ || left != left_ || bottom != bottom_;
    if (placementChanged) {
        HidePopup(L"placement changed");
        corner_ = corner;
        left_ = left;
        bottom_ = bottom;
    }
    timeout_ = std::clamp(timeoutSeconds, 3.0, 30.0);
    ApplyAlways(notifyAlways);
    hideBody_ = hideBody;
    notifyFullscreen_ = notifyFullscreen;
    // With their text hidden, the cards stay out of screenshots too.
    popup_.surface->SetPrivate(hideBody);
    center_.surface->SetPrivate(hideBody);
    if (placementChanged && center_.surface->Visible()) {
        ResetView(center_);
        expanded_ = corner_ || notices_.size() <= 1;
        Layout(Mode::Center); // an open centre follows a changed placement immediately
    }
    popup_.surface->Invalidate();
    center_.surface->Invalidate();
}

void Notifications::SetRadius(float dip)
{
    radius_ = dip;
    popup_.surface->SetRadius(dip);
    center_.surface->SetRadius(dip);
}

int Notifications::Unread() const { return int(std::ranges::count_if(notices_, &Notice::unread)); }

int Notifications::QuietState() const
{
    if (dnd_)
        return QUNS_QUIET_TIME;
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&state)))
        return QUNS_ACCEPTS_NOTIFICATIONS;
    const bool checkFs = state == QUNS_BUSY || state == QUNS_ACCEPTS_NOTIFICATIONS || state == QUNS_APP;
    const HMONITOR fgMon = checkFs ? MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONULL) : nullptr;
    const RECT bar = corner_ ? RECT{} : (barRect ? barRect() : (bellRect ? bellRect() : RECT{}));
    const HMONITOR popMon = checkFs ? MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY) : nullptr;
    const bool fs = (fgMon && FullScreenInFront(fgMon)) || (popMon && popMon != fgMon && FullScreenInFront(popMon));
    const bool game = fs && notifyFullscreen_ && ((fgMon && GameInFront(fgMon)) || (popMon && popMon != fgMon && GameInFront(popMon)));
    const bool ours = state == QUNS_QUIET_TIME && quietIsOurs && quietIsOurs();
    if (notification::AcceptsNotificationPopup(state, ours, fs, notifyFullscreen_, game))
        return QUNS_ACCEPTS_NOTIFICATIONS;
    return (state == QUNS_ACCEPTS_NOTIFICATIONS || state == QUNS_APP) ? int(QUNS_BUSY) : int(state);
}

void Notifications::ResetView(View& v)
{
    v.cards.clear();
    v.glow.clear();
    v.lastPaint = 0;
    v.scroll = v.scrollTarget = 0;
    v.stackPos = v.stackVel = 0;
    v.stackGoal = -1;
    v.stackMoving = false;
}

bool Notifications::BodyHidden() const
{
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    return hideBody_ || (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_PRESENTATION_MODE);
}

void Notifications::ClearPreparedUpdate()
{
    std::erase_if(notices_, [](const Notice& n) { return n.source == L"atrium-update"; });
    std::erase_if(popupIds_, [&](uint64_t id) { return std::ranges::none_of(notices_, [&](const Notice& n) { return n.id == id; }); });
    Changed();
}

bool Notifications::MergeOrIgnoreDuplicate(Notice& n)
{
    // Repeats of the listener's notices, and bursts of the same title from one
    // app, become one card with a count.
    for (auto it = notices_.begin(); it != notices_.end(); ++it) {
        const bool sameExternal = n.externalId && notification::HasExternalId(*it, n.externalId) && it->source == n.source;
        const bool burst = (!it->pid || !n.pid || it->pid == n.pid) && it->source == n.source && FoldCase(it->appId) == FoldCase(n.appId) && it->app == n.app &&
                           it->title == n.title && n.time - it->time < kMergeMs;
        if (sameExternal && it->title == n.title && it->body == n.body)
            return false;
        if (!sameExternal && !burst)
            continue;
        Notice merged = *it;
        if (sameExternal)
            merged.title = n.title;
        else
            merged.count += 1;
        notification::AddExternalId(merged, n.externalId);
        merged.body = n.body, merged.time = n.time, merged.unread = true;
        if (!merged.pid)
            merged.pid = n.pid;
        if (merged.trayHostKey.empty())
            merged.trayHostKey = n.trayHostKey;
        if (!sameExternal && !n.action.empty())
            merged.action = n.action;
        ShiftStack(Mode::Center, size_t(it - notices_.begin()), -1);
        notices_.erase(it);
        n = std::move(merged);
        Log(sameExternal ? L"notify: %llu updated by its sender" : L"notify: merged into %llu (x%d)", n.id, n.count);
        return true;
    }
    return true;
}

void Notifications::Post(Notice n, bool silent)
{
    if (!n.unread) {
        // The sender reports the notification/conversation read: withdraw matching cards
        // (by externalId for listener toasts, keeping merged cards until every id is gone).
        Withdraw(n);
        return;
    }
    n.time = GetTickCount64();
    notification::AddExternalId(n, n.externalId);
    if (!MergeOrIgnoreDuplicate(n))
        return;
    if (!n.id)
        n.id = nextId_++;
    leaving_.erase(n.id); // a card closing just now that the new message continues stays
    notices_.insert(notices_.begin(), n);
    ShiftStack(Mode::Center, 0, +1);
    if (notices_.size() > kMaxNotices) {
        notices_.resize(kMaxNotices);
        // The oldest went: so do their places in a stack held back meanwhile.
        std::erase_if(popupIds_, [&](uint64_t id) { return std::ranges::none_of(notices_, [&](const Notice& o) { return o.id == id; }); });
    }
    Log(L"notify: posted %llu from %s (%s)", n.id, n.app.c_str(), n.source.c_str());

    // The open list shows it right away; otherwise it pops up, unless the user
    // is in a full-screen app, presenting or in quiet hours.
    if (silent || center_.surface->Visible()) {
        Changed();
        return;
    }
    if (int quiet = QuietState(); quiet != QUNS_ACCEPTS_NOTIFICATIONS && quiet != QUNS_APP) {
        Log(L"notify: popup suppressed (user notification state %d)", quiet);
        Changed();
        return;
    }
    if (auto old = std::ranges::find(popupIds_, n.id); old != popupIds_.end()) {
        ShiftStack(Mode::Popup, size_t(old - popupIds_.begin()), -1);
        popupIds_.erase(old);
    }
    popupIds_.insert(popupIds_.begin(), n.id);
    if (corner_)
        popup_.scroll = popup_.scrollTarget = 0; // a new arrival takes the corner's slot
    ShiftStack(Mode::Popup, 0, +1);
    const bool wasVisible = popup_.surface->Visible();
    Layout(Mode::Popup);
    if (!popup_.surface->Visible()) {
        Changed();
        return;
    }
    if (wasVisible)
        SetWindowPos(popup_.surface->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ArmPopupTimer();
    Changed();
}

void Notifications::PanelsChanged()
{
    if (!popupIds_.empty())
        Layout(Mode::Popup);
    if (center_.surface->Visible())
        Layout(Mode::Center); // also follow primary-display and work-area changes
}

void Notifications::HidePopup(const wchar_t* why)
{
    if (!popup_.surface->Visible()) {
        // Cards held back by an open panel are dropped too (they stay in the list).
        KillTimer(popup_.surface->Hwnd(), kHoldTimer);
        popupIds_.clear();
        return;
    }
    KillTimer(popup_.surface->Hwnd(), kPopupTimer);
    popup_.surface->Hide();
    popupIds_.clear();
    Log(L"notify: popup hidden (%s)", why);
}

void Notifications::ToggleCenter(bool expand)
{
    if (center_.surface->Visible()) {
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
        Log(L"notify: center closed");
        return;
    }
    if (!expand && GetTickCount64() - centerClosed_ < 400)
        return; // the click on the bell that just closed it
    if (!handoffPending_)
        HidePopup(L"center opened");
    expanded_ = corner_ || expand || notices_.size() <= 1;
    center_.scroll = center_.scrollTarget = 0;
    Layout(Mode::Center);
    // Opening the list marks everything as read.
    for (auto& n : notices_)
        n.unread = false;
    Changed();
    Log(L"notify: center opened (%zu)", notices_.size());
}

void Notifications::Clear()
{
    size_t n = notices_.size();
    HidePopup(L"cleared");
    popupIds_.clear();
    if (center_.surface->Visible() && n) {
        // The cards fly out one after another, then go.
        double now = ClockMs();
        int i = 0;
        for (const auto& notice : notices_)
            leaving_.try_emplace(notice.id, now + 30.0 * std::min(i++, 8));
        SetTimer(center_.surface->Hwnd(), kSweepTimer, UINT(kLeaveMs + 30 * 8 + 20), nullptr);
        center_.surface->Invalidate();
    } else {
        notices_.clear();
        leaving_.clear();
    }
    Log(L"notify: cleared %zu", n);
    Changed();
}

void Notifications::Remove(uint64_t id)
{
    auto startLeave = [&](View& v) {
        leaving_.try_emplace(id, ClockMs());
        SetTimer(v.surface->Hwnd(), kSweepTimer, UINT(kLeaveMs + 20), nullptr);
        v.surface->Invalidate();
        Changed();
    };
    bool inPopup = std::ranges::find(popupIds_, id) != popupIds_.end();
    if (popup_.surface->Visible() && inPopup) {
        // Fades and drifts away; the cards behind move up once it is gone.
        KillTimer(popup_.surface->Hwnd(), kPopupTimer);
        startLeave(popup_);
        return;
    }
    std::erase(popupIds_, id);
    if (popupIds_.empty())
        HidePopup(L"last card closed");
    if (center_.surface->Visible() && std::ranges::any_of(notices_, [&](const Notice& n) { return n.id == id; })) {
        startLeave(center_); // slides out; the others close up
        return;
    }
    std::erase_if(notices_, [&](const Notice& n) { return n.id == id; });
    Changed();
}

void Notifications::Sweep()
{
    double now = ClockMs();
    bool swept = false;
    for (auto it = leaving_.begin(); it != leaving_.end();) {
        if (now - it->second >= kLeaveMs || (!center_.surface->Visible() && !popup_.surface->Visible())) {
            uint64_t id = it->first;
            for (Mode mode : {Mode::Popup, Mode::Center}) {
                auto shown = Shown(mode);
                if (auto at = std::ranges::find_if(shown, [&](const Notice* n) { return n->id == id; }); at != shown.end())
                    ShiftStack(mode, size_t(at - shown.begin()), -1);
            }
            std::erase_if(notices_, [&](const Notice& n) { return n.id == id; });
            std::erase(popupIds_, id);
            it = leaving_.erase(it);
            swept = true;
        } else {
            ++it;
        }
    }
    if (!leaving_.empty())
        SetTimer((center_.surface->Visible() ? center_ : popup_).surface->Hwnd(), kSweepTimer, 60, nullptr);
    if (swept && popup_.surface->Visible()) {
        if (popupIds_.empty()) {
            // Already faded out: the window just goes.
            KillTimer(popup_.surface->Hwnd(), kPopupTimer);
            popup_.surface->Hide(false);
            Log(L"notify: popup hidden (last card closed)");
        } else {
            ArmPopupTimer();
        }
    }
    if (swept)
        Changed();
}

void Notifications::Activate(uint64_t id)
{
    auto it = std::ranges::find_if(notices_, [&](const Notice& n) { return n.id == id; });
    if (it == notices_.end())
        return;
    const Notice& n = *it;
    bool ok = onActivate ? onActivate(n) : false;
    Log(L"notify: activated %llu action '%s' %s", n.id, n.action.c_str(), ok ? L"ok" : L"failed");
    HidePopup(L"activated");
    if (center_.surface->Visible()) {
        center_.surface->Hide();
        centerClosed_ = GetTickCount64();
    }
    Remove(id);
    Sweep();
}

void Notifications::Changed()
{
    if (onUnread)
        onUnread(Unread());
    // While the stack turns into the list the popup waits for the list's glass (onReady).
    if (!handoffPopup_ && (popup_.surface->Visible() || !popupIds_.empty()))
        Layout(Mode::Popup);
    if (center_.surface->Visible())
        Layout(Mode::Center);
}

std::vector<const Notice*> Notifications::Shown(Mode mode) const
{
    std::vector<const Notice*> out;
    if (mode == Mode::Center) {
        for (const auto& n : notices_)
            out.push_back(&n);
        return out;
    }
    for (uint64_t id : popupIds_)
        for (const auto& n : notices_)
            if (n.id == id)
                out.push_back(&n);
    return out;
}

void Notifications::Layout(Mode mode)
{
    View& v = ViewOf(mode);
    auto shown = Shown(mode);
    if (mode == Mode::Popup && shown.empty()) {
        HidePopup(L"empty");
        return;
    }
    if (mode == Mode::Popup && !corner_ && held && held()) {
        if (v.surface->Visible()) {
            KillTimer(v.surface->Hwnd(), kPopupTimer);
            v.surface->Hide();
            Log(L"notify: popup held (a panel is open)");
        }
        SetTimer(v.surface->Hwnd(), kHoldTimer, kHoldMs, nullptr);
        return;
    }
    if (corner_)
        LayoutCorner(mode, v, shown.size());
    else
        LayoutAnchored(mode, v, shown.size());
}

void Notifications::Click(Mode mode, const std::wstring& id)
{
    auto shown = Shown(mode);
    auto index = [&](size_t prefix) { return size_t(std::stoul(id.substr(prefix))); };
    if (id == L"clear") {
        Clear();
    } else if (id == L"collapse") {
        expanded_ = false;
        Layout(Mode::Center);
    } else if (id == L"stack") {
        if (mode == Mode::Popup) {
            // The stack becomes the list in place: the popup goes at once and
            // its cards carry on in the list from where they are.
            handoff_ = popup_.cards;
            handoffFront_ = popupIds_.empty() ? 0 : popupIds_.front();
            handoffRect_ = popup_.surface->Rect();
            handoffPending_ = true;
            KillTimer(popup_.surface->Hwnd(), kPopupTimer);
            popupIds_.clear();
            handoffPopup_ = true; // hidden once the list's glass shows (onReady)
            ToggleCenter(true);   // the full list, expanded
            handoffPending_ = false;
            handoff_.clear();
        } else {
            expanded_ = true;
            Layout(Mode::Center);
        }
        Log(L"notify: expanded");
    } else if (id.starts_with(L"close:")) {
        if (size_t i = index(6); i < shown.size()) {
            Log(L"notify: closed %llu", shown[i]->id);
            Remove(shown[i]->id);
        }
    } else if (id.starts_with(L"card:")) {
        if (size_t i = index(5); i < shown.size())
            Activate(shown[i]->id);
    }
}

std::wstring Notifications::HitAt(const View& v, POINT pt) const
{
    const float s = v.surface->Scale(), x = float(pt.x) / s, y = float(pt.y) / s;
    // Close buttons sit on top of their cards: they are tested first.
    for (const Hit& h : v.hits) {
        if (h.id.starts_with(L"close:") && In(h.rect, x, y) && (v.hover == h.id || v.hover == L"card:" + h.id.substr(6)))
            return h.id;
    }
    for (const Hit& h : v.hits) {
        if (!h.id.starts_with(L"close:") && In(h.rect, x, y))
            return h.id;
    }
    return {};
}

bool Notifications::HandleTimerMessage(Mode mode, View& v, WPARAM w)
{
    if (w == kPopupTimer && mode == Mode::Popup) {
        OnPopupTimer();
        return true;
    }
    if (w == kHoldTimer && mode == Mode::Popup) {
        if (!held || !held()) {
            KillTimer(v.surface->Hwnd(), kHoldTimer);
            if (!popupIds_.empty()) {
                Layout(Mode::Popup);
                ArmPopupTimer();
                Log(L"notify: popup shown after the panel closed");
            }
        }
        return true;
    }
    if (w == kSweepTimer) {
        KillTimer(v.surface->Hwnd(), kSweepTimer);
        Sweep();
        return true;
    }
    return false;
}

void Notifications::HandleMouseMove(Mode mode, View& v, POINT pt)
{
    if (!v.tracking) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, v.surface->Hwnd(), 0};
        v.tracking = TrackMouseEvent(&tme) != FALSE;
        if (mode == Mode::Popup)
            KillTimer(v.surface->Hwnd(), kPopupTimer); // reading: do not time out
    }
    std::wstring h = HitAt(v, pt);
    // Hovering anywhere on a card shows its close button.
    if (h == L"stack")
        h = L"card:" + std::to_wstring(v.front);
    if (h != v.hover) {
        v.hover = h;
        v.surface->Invalidate();
    }
}

void Notifications::HandleMouseLeave(Mode mode, View& v)
{
    v.tracking = false;
    v.hover.clear();
    v.pressed.clear();
    if (mode == Mode::Popup && v.surface->Visible())
        ArmPopupTimer();
    v.surface->Invalidate();
}

void Notifications::HandleLButtonDown(View& v, POINT pt)
{
    v.pressed = HitAt(v, pt);
    if (!v.pressed.empty()) {
        // The glass under the card gives way, and a soft light spreads from the press.
        const float s = v.surface->Scale();
        v.rippleId = v.pressed == L"stack" ? L"card:" + std::to_wstring(v.front) : v.pressed;
        v.rippleX = float(pt.x) / s;
        v.rippleY = float(pt.y) / s;
        v.rippleMs = ClockMs();
        v.surface->Ripple(v.rippleX, v.rippleY);
    }
    v.surface->Invalidate();
}

void Notifications::HandleLButtonUp(Mode mode, View& v, POINT pt)
{
    const std::wstring h = HitAt(v, pt);
    const bool same = !h.empty() && h == v.pressed;
    v.pressed.clear();
    if (same)
        Click(mode, h);
    else
        v.surface->Invalidate();
}

void Notifications::HandleMouseWheel(Mode mode, View& v, WPARAM w)
{
    const int delta = GET_WHEEL_DELTA_WPARAM(w);
    const size_t shownCount = Shown(mode).size();
    if (!corner_ && (mode == Mode::Popup || (!expanded_ && shownCount > 1))) {
        // Over a stack: the wheel goes through it, staying a stack (down: older cards come to the front).
        PushStack(v, shownCount, delta);
        return;
    }
    const bool fromTop = corner_ ? !bottom_ : barEdge_ == BarEdge::Top;
    v.scrollTarget += float(delta) / WHEEL_DELTA * (fromTop ? -80 : 80);
    v.scrollTarget = std::clamp(v.scrollTarget, 0.f, std::max(0.f, v.contentH - v.viewH));
    v.surface->Invalidate();
}

bool Notifications::Message(Mode mode, UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    View& v = ViewOf(mode);
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_TIMER: return HandleTimerMessage(mode, v, w);
    case WM_ACTIVATE:
        if (mode == Mode::Center && LOWORD(w) == WA_INACTIVE && v.surface->Visible()) {
            v.surface->Hide();
            centerClosed_ = GetTickCount64();
            Log(L"notify: center closed (focus moved)");
        }
        return false;
    case WM_MOUSEMOVE: HandleMouseMove(mode, v, pt); return true;
    case WM_MOUSELEAVE: HandleMouseLeave(mode, v); return true;
    case WM_LBUTTONDOWN: HandleLButtonDown(v, pt); return true;
    case WM_LBUTTONUP: HandleLButtonUp(mode, v, pt); return true;
    case WM_MOUSEWHEEL: HandleMouseWheel(mode, v, w); return true;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE && mode == Mode::Center) {
            v.surface->Hide();
            centerClosed_ = GetTickCount64();
            return true;
        }
        return false;
    default: return false;
    }
}

} // namespace atrium
