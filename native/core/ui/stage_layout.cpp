// Stage Manager strip layout, reveal/glide animation, edge-watching, idle timer throttling, and placeholder pictures.
#include "ui/stage_manager.h"

#include "clock.h"
#include "shell/displays.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace atrium {

namespace {

// DIP. Each set of windows stands in a box kBoxW x kBoxH, kEdge from the screen's edge, the boxes one
// above the other in the middle of what the bar leaves of the screen's height. The windows turn
// kTilt degrees about their upright axis, the side towards the screen's middle away from the eye
// (a viewer kDepth in front of the screen); a pointer over them turns them most of the way back.
// The eye is level with a point kKeystone of the way from a box to the strip's middle: the boxes
// above it lean down towards the middle of the screen, those below lean up, the more so the
// further they are.
constexpr float kSlotGap = kStageSlotGap, kVPad = 24, kStripW = kStageStripW;
constexpr float kSlot = kStageBoxH + kSlotGap;
constexpr float kBarRoom = 90; // the bar floats over the bottom of the work area
constexpr int kMaxEntries = 6;
constexpr ULONGLONG kDesktopMs = 250; // the desktop in front this long: it was clicked
// The pointer at the screen's edge for kEdgeMs brings the strip out; away from it for kPeekLeaveMs
// sends it back. Whether a window covers the strip's place is looked at every kCoverMs.
constexpr ULONGLONG kEdgeMs = 40, kPeekLeaveMs = 350, kCoverMs = 150;
constexpr double kRevealInMs = 380, kRevealOutMs = 210, kRevealStagger = 32;
constexpr motion::Spring kGlide{0.42f, 0.86f};
constexpr motion::Spring kFadeSpring{0.30f, 1.f};

bool AppsDark()
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                 &light, &size);
    return light == 0;
}

float EaseOut3(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }

} // namespace

// The flight layer over the whole screen: kept up between flights (its first frame is slow), and
// letting clicks through while nothing flies.
void StageManager::Launch()
{
    const MONITORINFO mi = MonitorInfo();
    KillTimer(flight_.Hwnd(), kFlightTimer);
    SetWindowRgn(flight_.Hwnd(), nullptr, FALSE);
    if (!flight_.Visible())
        flight_.Show(mi.rcMonitor, false, POINT{mi.rcMonitor.left, mi.rcMonitor.top}, false);
    else
        flight_.Move(mi.rcMonitor);
    flight_.Invalidate();
    SyncWatchTimer(GetTickCount64());
}

MONITORINFO StageManager::MonitorInfo() const
{
    MONITORINFO mi{sizeof(mi)};
    if (monitor_ && GetMonitorInfoW(monitor_, &mi))
        return mi;
    if (const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); primary && GetMonitorInfoW(primary, &mi))
        return mi;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &mi.rcWork, 0);
    mi.rcMonitor = mi.rcWork;
    return mi;
}

RECT StageManager::WorkArea() const { return MonitorInfo().rcWork; }

int StageManager::Capacity() const
{
    RECT wa = WorkArea();
    float avail = float(wa.bottom - wa.top) / PopupScale(wa) - 2 * kVPad - (Vertical(barEdge_) ? 0 : kBarRoom);
    return std::clamp(int((avail + kSlotGap) / kSlot), 1, kMaxEntries);
}

std::vector<StageManager::Entry> StageManager::BuildStripEntries() const
{
    std::vector<Entry> next;
    if (!enabled_)
        return next;
    for (const Set& set : recent_) {
        std::vector<std::pair<uint64_t, HWND>> mine;
        for (const auto& [h, p] : parked_) {
            if (!HasKey(set, p.key) || !IsWindow(h))
                continue;
            BOOL here = TRUE; // windows put away on another virtual desktop wait there
            if (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here)
                continue;
            mine.emplace_back(p.seq, h);
        }
        if (mine.empty())
            continue;
        std::ranges::sort(std::ranges::reverse_view(mine)); // most recent first
        Entry e;
        e.keys = set;
        std::ranges::stable_sort(e.keys, [&](const std::wstring& a, const std::wstring& b) {
            auto newest = [&](const std::wstring& k) {
                uint64_t n = 0;
                for (const auto& [seq, h] : mine)
                    if (parked_.at(h).key == k)
                        n = std::max(n, seq);
                return n;
            };
            return newest(a) > newest(b);
        });
        for (size_t i = 0; i < mine.size() && i < kStack; ++i)
            e.windows.push_back(mine[i].second);
        next.push_back(std::move(e));
    }
    return next;
}

void StageManager::Rebuild(bool keepAnchor)
{
    std::vector<Entry> next = BuildStripEntries();
    scrollCount_ = next.size();
    const size_t cap = size_t(Capacity());
    scroll_.Fit(scrollCount_, cap);
    if (keepAnchor && !entries_.empty()) {
        auto anchor = std::ranges::find_if(next, [&](const Entry& e) { return e.keys == entries_.front().keys; });
        if (anchor != next.end())
            scroll_.Anchor(size_t(anchor - next.begin()), scrollCount_, cap);
    }
    if (!next.empty())
        std::ranges::rotate(next, next.begin() + scroll_.Offset());
    if (next.size() > cap)
        next.resize(cap);
    // An index can now name another group. Check identity before the old keys are moved/cleared.
    auto sameSlot = [&](int index) {
        return index >= 0 && size_t(index) < next.size() && size_t(index) < entries_.size() && next[size_t(index)].keys == entries_[size_t(index)].keys;
    };
    if (!sameSlot(pressed_))
        pressed_ = -1;
    if (!sameSlot(closeHot_))
        closeHot_ = -1;
    if (!sameSlot(closePressed_))
        closePressed_ = -1;
    const bool visible = shown_ && surface_.Visible();
    // A set keeps its card (where it is, how far it is hovered).
    for (Entry& e : next) {
        auto old = std::ranges::find_if(entries_, [&](const Entry& o) { return o.keys == e.keys; });
        if (old != entries_.end()) {
            e.hover = old->hover;
            e.drop = old->drop;
            e.y = old->y;
            e.vy = old->vy;
            e.appear = old->appear;
            e.va = old->va;
            e.closeAt = old->closeAt;
            old->keys.clear(); // taken
        } else {
            e.appear = visible ? 0.f : 1.f; // a new card fades in; a strip coming out slides as a whole
        }
    }
    // Cards that went fade out where they were.
    for (Entry& old : entries_) {
        if (old.keys.empty() || !visible || old.appear < 0.02f)
            continue;
        old.targetAppear = 0;
        old.hover = motion::Follow{};
        old.drop = motion::Follow{};
        leaving_.push_back(std::move(old));
    }
    if (!visible)
        leaving_.clear();
    const bool changed = next.size() != entries_.size() ||
                         !std::equal(next.begin(), next.end(), entries_.begin(), [](const Entry& a, const Entry& b) { return a.keys == b.keys; }) ||
                         !leaving_.empty();
    entries_ = std::move(next);
    if (hover_ >= int(entries_.size()))
        hover_ = -1;
    if (pressed_ >= int(entries_.size()))
        pressed_ = -1;
    Layout();
    PruneSurfaceCaches();
    if (entries_.empty() && leaving_.empty()) {
        Hide();
        SyncWatchTimer(GetTickCount64());
        return;
    }
    if (surface_.Visible()) {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
    if (changed)
        Watch();
    else
        SyncWatchTimer(GetTickCount64());
}

// The strip's window has room for Capacity() boxes, in the middle of what the bar leaves of the
// screen's height, from the screen's edge; the boxes stand in the middle of it, and glide when
// that changes.
void StageManager::Layout()
{
    RECT wa = WorkArea();
    const float s = PopupScale(wa);
    const int cap = Capacity();
    size_ = D2D1::SizeF(kStripW, float(cap) * kSlot - kSlotGap + 2 * kVPad);
    const size_t n = entries_.size();
    const float used = n ? float(n) * kSlot - kSlotGap : 0;
    const float top = (size_.height - used) / 2;
    const bool snap = !surface_.Visible();
    for (size_t i = 0; i < n; ++i) {
        Entry& e = entries_[i];
        e.targetY = top + float(i) * kSlot;
        if (snap || e.appear == 0) {
            e.y = e.targetY; // a new card fades in at its place
            e.vy = 0;
        }
    }
    const LONG w = LONG(std::lround(size_.width * s)), h = LONG(std::lround(size_.height * s));
    const LONG room = LONG(std::lround(kBarRoom * s));
    const LONG x0 = wa.left + (!onRight_ && barEdge_ == BarEdge::Left ? room : 0);
    const LONG x1 = wa.right - (onRight_ && barEdge_ == BarEdge::Right ? room : 0);
    const LONG y0 = wa.top + (barEdge_ == BarEdge::Top ? room : 0);
    const LONG y1 = wa.bottom - (barEdge_ == BarEdge::Bottom ? room : 0);
    LONG x = onRight_ ? x1 - w : x0;
    LONG y = y0 + std::max<LONG>(0, (y1 - y0 - h) / 2);
    rect_ = RECT{x, y, x + w, y + h};
}

float StageManager::Reveal(int index, double nowMs) const
{
    // Out: one card after the other, from the top. In: all together.
    const double delay = shown_ && revealFrom_ < 0.01f ? kRevealStagger * index : 0;
    const float t = float(std::clamp((nowMs - revealStart_ - delay) / (shown_ ? kRevealInMs : kRevealOutMs), 0.0, 1.0));
    const float target = shown_ ? 1.f : 0.f;
    return revealFrom_ + (target - revealFrom_) * (shown_ ? EaseOut3(t) : t * t);
}

bool StageManager::Animate(double nowMs)
{
    if (!entries_.empty() || !leaving_.empty())
        RecordActiveIconKeys();
    const bool hadLeaving = !leaving_.empty();
    const float dt = lastStep_ > 0 ? float(std::clamp((nowMs - lastStep_) / 1000.0, 0.0, 0.05)) : 0.f;
    lastStep_ = nowMs;
    bool moving = false;
    auto step = [&](Entry& e) {
        kGlide.Step(e.y, e.vy, e.targetY, dt);
        kFadeSpring.Step(e.appear, e.va, e.targetAppear, dt);
        if (motion::Spring::AtRest(e.y, e.vy, e.targetY, 0.2f))
            e.y = e.targetY, e.vy = 0;
        else
            moving = true;
        if (motion::Spring::AtRest(e.appear, e.va, e.targetAppear, 0.004f))
            e.appear = e.targetAppear, e.va = 0;
        else
            moving = true;
        e.appear = std::clamp(e.appear, 0.f, 1.f);
    };
    for (Entry& e : entries_)
        step(e);
    for (Entry& e : leaving_)
        step(e);
    std::erase_if(leaving_, [](const Entry& e) { return e.appear <= 0.01f; });
    if (hadLeaving && leaving_.empty())
        PruneSurfaceCaches();
    const double span = shown_ ? kRevealInMs + kRevealStagger * double(entries_.size()) : kRevealOutMs;
    if (nowMs - revealStart_ < span)
        moving = true;
    if (!moving) {
        lastStep_ = 0;
        SyncWatchTimer(GetTickCount64());
    }
    return moving;
}

// The boxes' span, with room for their shadows and a hovered box's growth: the rest of the column
// lets clicks through. While cards move the region only grows (where they go is in it already); it
// is fitted again when they rest.
void StageManager::UpdateRegion(bool moving)
{
    float top = size_.height, bottom = 0;
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list) {
            top = std::min({top, e.y, e.targetY});
            bottom = std::max({bottom, e.y + kBoxH, e.targetY + kBoxH});
        }
    RECT wr{};
    if (!GetWindowRect(surface_.Hwnd(), &wr))
        return;
    const float s = surface_.Scale();
    const LONG inset = rect_.top - wr.top;
    RECT r{0, std::max<LONG>(0, inset + LONG(std::floor((top - 24) * s))), wr.right - wr.left,
           std::min<LONG>(wr.bottom - wr.top, inset + LONG(std::ceil((bottom + 34) * s)))};
    if (bottom <= top)
        r = RECT{0, 0, 0, 0};
    if (moving && !IsRectEmpty(&region_))
        UnionRect(&r, &r, &region_);
    if (EqualRect(&r, &region_))
        return;
    region_ = r;
    HRGN rgn = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (!SetWindowRgn(surface_.Hwnd(), rgn, FALSE))
        DeleteObject(rgn);
}

RECT StageManager::Zone() const
{
    const MONITORINFO mi = MonitorInfo();
    return onRight_ ? RECT{rect_.left, rect_.top, mi.rcMonitor.right, rect_.bottom} : RECT{mi.rcMonitor.left, rect_.top, rect_.right, rect_.bottom};
}

bool StageManager::Covered()
{
    const ULONGLONG now = GetTickCount64();
    if (coveredAt_ && now - coveredAt_ < kCoverMs)
        return covered_;
    coveredAt_ = now;
    // An app filling the whole screen (a game, a video): out of its way.
    covered_ = FullScreenInFront(monitor_);
    if (covered_)
        return covered_;
    const RECT zone = Zone();
    std::vector<HWND> windows = excludedWindows_; // an excluded app's window over the strip counts too
    for (const TaskItem& t : items_)
        windows.insert(windows.end(), t.windows.begin(), t.windows.end());
    for (HWND h : windows) {
        if (h == dragWindow_ || parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
            continue;
        RECT r{}, hit{};
        if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(h, &r))
            continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue; // on another virtual desktop
        if (IntersectRect(&hit, &r, &zone))
            return covered_ = true;
    }
    return covered_;
}

bool StageManager::NeedsAdopt(ULONGLONG now) const
{
    if (!minimizeToStrip_)
        return false;
    if (std::ranges::any_of(up_, [this, now](const auto& u) { return !parked_.contains(u.first) && IsIconic(u.first) && !IsRestoring(u.first, now); }))
        return true;
    for (const TaskItem& t : items_) {
        if (!HasKey(stage_, t.key))
            continue;
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h))
                continue;
            if (IsIconic(h)) {
                if (!IsRestoring(h, now))
                    return true;
            } else if (!up_.contains(h) && OnMonitor(h, monitor_)) {
                return true;
            }
        }
    }
    return false;
}

bool StageManager::WatchActive(ULONGLONG now) const
{
    if (dragWindow_ != nullptr || !flights_.empty() || !leaving_.empty() || lastStep_ != 0)
        return true;
    if (peek_ || edgeSince_ != 0 || inside_ || pressed_ >= 0 || closePressed_ >= 0)
        return true;
    if ((desktopStow_.desktopSince != 0 && !desktopStow_.onDesktop) || desktopStow_.buttonDown)
        return true;
    if (!pending_.empty() || !restoring_.empty() || NeedsAdopt(now))
        return true;
    if (std::ranges::any_of(entries_, [](const Entry& e) { return e.hover.moving || e.drop.moving; }))
        return true;
    return std::ranges::any_of(parked_, [](const auto& kv) { return !kv.second.down; });
}

void StageManager::SyncWatchTimer(ULONGLONG now)
{
    if (!enabled_)
        return;
    const UINT want = WatchActive(now) ? kWatchActiveMs : kWatchIdleMs;
    if (want == watchMs_)
        return;
    watchMs_ = want;
    SetTimer(surface_.Hwnd(), kWatchTimer, want, nullptr);
}

void StageManager::UpdateAutoHidePeek(POINT p, bool dragging, ULONGLONG now)
{
    const MONITORINFO mi = MonitorInfo();
    const bool adjacentAtPointerY = MonitorFromPoint(POINT{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left - 1, p.y}, MONITOR_DEFAULTTONULL) != nullptr;
    const StageEdgeResult edge =
        EvaluateStageEdge(mi.rcMonitor, mi.rcWork, p, onRight_, adjacentAtPointerY, PopupScale(mi.rcWork), dragging, dragWindow_ != nullptr, kEdgeMs);
    edgeSince_ = edge.atEdge ? (edgeSince_ ? edgeSince_ : now) : 0;
    if (!autoHide_ && !Covered()) {
        peek_ = false;
        Show();
        return;
    }
    if (!peek_ && edgeSince_ && now - edgeSince_ >= edge.dwellMs && !FullScreenInFront(monitor_)) {
        peek_ = true;
        leftAt_ = 0;
    }
    if (peek_) {
        const RECT zone = Zone();
        if (edge.atEdge || inside_ || PtInRect(&zone, p))
            leftAt_ = 0;
        else if (!leftAt_)
            leftAt_ = now;
        else if (now - leftAt_ >= kPeekLeaveMs)
            peek_ = false;
    }
    if (peek_)
        Show();
    else
        Hide();
}

// Every kWatchActiveMs while active or kWatchIdleMs while idle. Always shown, the strip is out unless a
// window covers its place; then, as auto-hidden, it comes out over the windows while the pointer rests
// on that edge of the screen, and goes once the pointer has left it.
void StageManager::Watch()
{
    const ULONGLONG now = GetTickCount64();
    POINT p{};
    GetCursorPos(&p);
    const bool anyButtonDown = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
    const bool dragging = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
    // A click on the desktop clears the stage, while pressing to drag a desktop file must not.
    if (enabled_) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[16] = {};
        const bool atDesktop = fg && GetClassNameW(fg, cls, 16) && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW"));
        // A window minimized on the stage goes to the strip even when nothing else told of it.
        if (NeedsAdopt(now) && AdoptMinimized())
            Rebuild();
        if (StepStageDesktopStow(desktopStow_, atDesktop, anyButtonDown, p, GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG), now, kDesktopMs)) {
            if (AdoptMinimized())
                Rebuild();
            if (!stage_.empty()) {
                pending_.clear();
                Stage(Set{}, false, nullptr, false);
                Rebuild();
            }
        }
        KeepPicture();
    }
    if (!enabled_ || entries_.empty()) {
        peek_ = false;
        edgeSince_ = 0;
        StepDrag(POINT{});
        if (leaving_.empty())
            Hide();
        SyncWatchTimer(now);
        return;
    }
    UpdateAutoHidePeek(p, dragging, now);
    StepDrag(p);
    SyncWatchTimer(now);
}

void StageManager::Show()
{
    if (shown_) {
        if (surface_.Visible())
            surface_.Move(rect_);
        return;
    }
    const double now = ClockMs();
    const bool visible = surface_.Visible();
    revealFrom_ = visible ? Reveal(0, now) : 0.f; // turning back on its way in
    revealStart_ = now;
    shown_ = true;
    closeHot_ = closePressed_ = -1;
    if (!visible) {
        hover_ = pressed_ = dragHover_ = -1;
        leaving_.clear();
        for (Entry& e : entries_) {
            e.y = e.targetY, e.vy = 0;
            e.appear = e.targetAppear = 1, e.va = 0;
            e.hover = e.drop = motion::Follow{};
        }
        region_ = RECT{};
        const MONITORINFO mi = MonitorInfo();
        POINT anchor{onRight_ ? mi.rcMonitor.right : mi.rcMonitor.left, (rect_.top + rect_.bottom) / 2};
        surface_.Show(rect_, false, anchor, false); // the cards slide out themselves
    } else {
        surface_.Move(rect_);
        surface_.Invalidate();
    }
    SyncWatchTimer(GetTickCount64());
}

void StageManager::Hide()
{
    if (!shown_)
        return;
    const double now = ClockMs();
    revealFrom_ = Reveal(0, now), revealStart_ = now, shown_ = false, inside_ = false;
    hover_ = pressed_ = dragHover_ = closeHot_ = closePressed_ = -1;
    surface_.Invalidate(); // the cards slide back into the edge; then the window goes
    SyncWatchTimer(GetTickCount64());
}

std::shared_ptr<const WindowPicture> StageManager::PictureOf(HWND window, const std::wstring& app, std::wstring& cacheKey)
{
    auto it = pictures_.find(window);
    if (it != pictures_.end() && it->second.picture && it->second.picture->ok) {
        cacheKey = L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(window)) + L"|" + std::to_wstring(it->second.gen);
        issuedWindowKeys_.insert(cacheKey);
        return it->second.picture;
    }
    // No picture (put away before Atrium started, or one that cannot be printed): its app's icon.
    const std::wstring appKey = AppKeyOf(app);
    cacheKey = L"stage|p|" + appKey;
    issuedPlaceholderApps_.insert(appKey);
    const float s = PopupScale(WorkArea());
    const bool dark = AppsDark();
    auto& ph = placeholders_[appKey];
    if (!ph.picture || !ph.hasIcon || ph.scale != s || ph.dark != dark) {
        const int w = int(std::lround(kBoxW * s)), h = int(std::lround(kBoxW * s / 1.6f));
        // The largest icon the app has, up to 52 DIP.
        IconImage icon;
        for (int px : {int(std::lround(52 * s)), 64, 48, 32}) {
            icon = tasks_.Icon(appKey, px);
            if (!icon.Empty())
                break;
        }
        if (!ph.picture || ph.scale != s || ph.dark != dark || !icon.Empty()) {
            if (ph.picture)
                ForgetImages(cacheKey, false);
            ph.picture = std::make_shared<const WindowPicture>(PlaceholderPicture(icon, w, h, dark));
            ph.hasIcon = !icon.Empty();
            ph.dark = dark;
            ph.scale = s;
        }
    }
    return ph.picture;
}

} // namespace atrium
