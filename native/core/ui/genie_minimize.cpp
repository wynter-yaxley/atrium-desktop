#include "ui/genie_minimize.h"

#include "clock.h"
#include "log.h"
#include "shell/games.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace atrium {

namespace {

constexpr UINT WM_GENIE_PICTURE = WM_APP + 0x5E, WM_GENIE_RESTORE = WM_APP + 0x5F;
constexpr UINT_PTR kWatchTimer = 0x4751, kIdleTimer = 0x4752, kFreshTimer = 0x4753, kStateTimer = 0x4754, kHwIdleTimer = 0x4755;
constexpr UINT kWatchMs = 300, kStateMs = 16, kIdleMs = 4000, kFreshMs = 180;
constexpr ULONGLONG kDrawnMs = 700, kNearMs = 1500, kFarMs = 20000;
constexpr int kLargeMax = 1600, kRows = 64, kBoundsPadPx = 8;
constexpr double kFlightMs = 560, kHoldMs = 90, kDissolveMs = 160;
constexpr wchar_t kOverlayClass[] = L"AtriumGenieOverlay";

GenieMinimize* g_genie = nullptr; // the one effect, for its hook

void CALLBACK OnMinimizeStart(HWINEVENTHOOK, DWORD, HWND h, LONG object, LONG, DWORD, DWORD)
{
    if (object == OBJID_WINDOW && g_genie)
        g_genie->Minimizing(h);
}

bool QueryTransitionsDisabled(HWND h, BOOL& disabled)
{
    disabled = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &disabled, sizeof(disabled)));
}

void SetTransitionsDisabled(HWND h, bool disabled)
{
    const BOOL off = disabled ? TRUE : FALSE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

bool IsWindowCloaked(HWND h)
{
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != FALSE;
}

bool SameProcessKey(const WindowSnapshotKey& a, const WindowSnapshotKey& b)
{
    return a.pid == b.pid && a.creationTime.dwLowDateTime == b.creationTime.dwLowDateTime && a.creationTime.dwHighDateTime == b.creationTime.dwHighDateTime;
}

RECT Frame(HWND h)
{
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
        GetWindowRect(h, &r);
    return r;
}

RECT MonitorRect(const RECT& r)
{
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

template<class FlightRange> RECT CollectFlightBounds(const FlightRange& flights, const RECT& fallback)
{
    std::vector<RECT> rects;
    rects.reserve(flights.size() * 2);
    for (const auto& g : flights) {
        rects.push_back(g.from);
        rects.push_back(g.to);
    }
    if (rects.empty()) {
        if (IsRectEmpty(&fallback))
            return {};
        const RECT tight = ComputeTightFlightBounds(std::span<const RECT>(&fallback, 1), MonitorRect(fallback), kBoundsPadPx);
        return IsRectEmpty(&tight) ? MonitorRect(fallback) : tight;
    }
    RECT clampBounds{};
    for (const RECT& r : rects) {
        const RECT m = MonitorRect(r);
        if (IsRectEmpty(&clampBounds))
            clampBounds = m;
        else
            UnionRect(&clampBounds, &clampBounds, &m);
    }
    const RECT tight = ComputeTightFlightBounds(rects, clampBounds, kBoundsPadPx);
    return IsRectEmpty(&tight) ? MonitorRect(rects.front()) : tight;
}

bool Minimizable(HWND h)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    return h && IsWindowVisible(h) && !IsIconic(h) && (style & WS_MINIMIZEBOX) && (style & WS_CAPTION) == WS_CAPTION && !GetWindow(h, GW_OWNER);
}

} // namespace

LRESULT CALLBACK GenieMinimize::OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* self = reinterpret_cast<GenieMinimize*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_GENIE_FRAME_PRESENTED) {
        const auto packed = static_cast<uint64_t>(lParam);
        self->OnHardwareFramePresented(static_cast<uint64_t>(wParam), packed >> 1, (packed & 1ULL) != 0);
        return 0;
    }
    if (msg == WM_GENIE_DEVICE_LOST) {
        self->OnHardwareDeviceLost(static_cast<uint64_t>(wParam));
        return 0;
    }
    if (msg == WM_TIMER && wParam == kStateTimer) {
        self->TickHardwareFlights(ClockMs());
        return 0;
    }
    if (msg == WM_TIMER && wParam == kHwIdleTimer) {
        KillTimer(hwnd, kHwIdleTimer);
        if (self->hwFlights_.empty()) {
            ShowWindow(hwnd, SW_HIDE);
            if (self->hardwareHost_)
                self->hardwareHost_->ClearGenieFlights();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

GenieMinimize::GenieMinimize(HINSTANCE instance, std::function<std::optional<RECT>(HWND)> target, std::function<bool(HWND)> skip)
    : instance_(instance),
      surface_(instance, L"Atrium Genie", false, 0, false, true),
      snapshots_(surface_.Hwnd(), WM_GENIE_PICTURE),
      target_(std::move(target)),
      skip_(std::move(skip))
{
    surface_.SetGlassless(true);
    surface_.SetRegionNone(); // over everything and out of the way until something pours
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m == WM_TIMER && w == kWatchTimer) {
            Watch();
        } else if (m == WM_TIMER && w == kIdleTimer) {
            KillTimer(surface_.Hwnd(), kIdleTimer);
            if (flights_.empty())
                surface_.Hide(false);
        } else if (m == WM_TIMER && w == kFreshTimer) {
            RunPending();
        } else if (m == WM_GENIE_RESTORE) {
            RunRestores();
        } else if (m == WM_GENIE_PICTURE) {
            for (WindowSnapshots::Result& r : snapshots_.Take()) {
                if (!r.large.ok || !pictureMeta_.contains(r.window) || r.key.pid == 0)
                    continue;
                WindowSnapshotKey key{};
                if (!QueryWindowSnapshotKey(r.window, ++nextPictureGen_, key) || !SnapshotMatchesLiveIdentity(r.key, key))
                    continue;
                PictureMeta& meta = EnsureProcessMeta(r.window, key);
                const RECT frame = IsIconic(r.window) ? meta.frame : Frame(r.window);
                if (pictureCache_.Put(key, frame, std::make_shared<const WindowPicture>(std::move(r.large)))) {
                    meta.frame = frame;
                    meta.pictureId = ++nextPictureId_;
                }
                if (r.window == pending_)
                    RunPending();
            }
        } else {
            return false;
        }
        return true;
    };
}

GenieMinimize::~GenieMinimize()
{
    SetHardwareHost(nullptr);
    SetEnabled(false);
    Shutdown();
    DestroyOverlayWindow();
}

void GenieMinimize::EnsureOverlayWindow()
{
    if (hwOverlay_)
        return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance_;
        wc.lpfnWndProc = &GenieMinimize::OverlayProc;
        wc.lpszClassName = kOverlayClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    hwOverlay_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                 kOverlayClass, L"Atrium Genie Overlay", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    if (hwOverlay_) {
        SetLayeredWindowAttributes(hwOverlay_, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(hwOverlay_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
}

void GenieMinimize::DestroyOverlayWindow()
{
    if (!hwOverlay_)
        return;
    SetWindowLongPtrW(hwOverlay_, GWLP_USERDATA, 0);
    DestroyWindow(hwOverlay_);
    hwOverlay_ = nullptr;
}

void GenieMinimize::SetHardwareHost(GenieHost* host)
{
    if (host == hardwareHost_)
        return;
    if (hardwareHost_) {
        ++hardwareEpoch_;
        CancelHardwareFlights();
        hardwareHost_->SetGenieOverlay(nullptr, 0);
    }
    hardwareHost_ = host;
    if (hardwareHost_) {
        surface_.SetRegionNone();
        surface_.Hide(false);
        EnsureOverlayWindow();
        ++hardwareEpoch_;
        hardwareHost_->SetGenieOverlay(hwOverlay_, hardwareEpoch_);
    }
}

GenieMinimize::PictureMeta& GenieMinimize::EnsureProcessMeta(HWND window, const WindowSnapshotKey& key)
{
    PictureMeta& meta = pictureMeta_[window];
    if (meta.key.pid != 0 && key.pid != 0 && !SameProcessKey(meta.key, key)) {
        pictureCache_.Erase(window);
        meta = PictureMeta{};
    }
    if (key.pid != 0)
        meta.key = key;
    return meta;
}

std::vector<std::function<void()>> GenieMinimize::DrainHardwareFlights()
{
    if (hwOverlay_) {
        KillTimer(hwOverlay_, kStateTimer);
        KillTimer(hwOverlay_, kHwIdleTimer);
        ShowWindow(hwOverlay_, SW_HIDE);
    }
    std::vector<std::function<void()>> pendingCallbacks = stateMachine_.DrainOnShutdown([](HWND h) { return IsWindow(h) != FALSE; });
    for (HardwareFlight& f : hwFlights_) {
        if (!f.isRestore && f.window == held_)
            Release(f.window);
        if (f.restoreTransitions) {
            f.restoreTransitions = false;
            ReleaseTransitions(f.window, !(skip_ && skip_(f.window)));
        }
    }
    hwFlights_.clear();
    if (hardwareHost_)
        hardwareHost_->ClearGenieFlights();
    return pendingCallbacks;
}

void GenieMinimize::Shutdown()
{
    if (g_genie == this)
        g_genie = nullptr;
    if (hook_)
        UnhookWinEvent(hook_);
    hook_ = nullptr;
    KillTimer(surface_.Hwnd(), kWatchTimer);
    KillTimer(surface_.Hwnd(), kIdleTimer);
    RunPending();
    Release(held_, !held_ || !(skip_ && skip_(held_)));

    std::vector<std::function<void()>> hwCallbacks = DrainHardwareFlights();
    std::vector<std::function<void()>> pendingRestores = DrainGenieShutdownRestores(
        flights_, [](HWND h) { return IsWindow(h) != FALSE; }, [this](const std::wstring& key) { surface_.ForgetImage(key); },
        [this](HWND h) { ReleaseTransitions(h, !(skip_ && skip_(h))); });
    transitionLeases_.Clear([this](HWND h) { return skip_ && skip_(h); }, [](HWND h) { return IsWindow(h) != FALSE; }, SetTransitionsDisabled);
    pictureCache_.Clear();
    pictureMeta_.clear();
    front_ = nullptr;
    surface_.SetRegionNone();
    surface_.Hide(false);
    for (const auto& cb : hwCallbacks)
        cb();
    for (const auto& restore : pendingRestores)
        restore();
}

void GenieMinimize::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        g_genie = this;
        hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnMinimizeStart, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        SetTimer(surface_.Hwnd(), kWatchTimer, kWatchMs, nullptr);
    } else {
        Shutdown();
    }
    Log(L"genie: %s", on ? L"on" : L"off");
}

void GenieMinimize::AcquireTransitions(HWND window) { transitionLeases_.Acquire(window, QueryTransitionsDisabled, SetTransitionsDisabled); }

void GenieMinimize::ReleaseTransitions(HWND window, bool restoreAttribute)
{
    transitionLeases_.Release(window, restoreAttribute, [](HWND h) { return IsWindow(h) != FALSE; }, SetTransitionsDisabled);
}

void GenieMinimize::Release(HWND window, bool restoreTransitions)
{
    if (window && window == held_) {
        held_ = nullptr;
        ReleaseTransitions(window, restoreTransitions);
    }
}

void GenieMinimize::Hold(HWND window)
{
    if (window == held_)
        return;
    Release(held_, !held_ || !(skip_ && skip_(held_)));
    if (window && IsWindow(window)) {
        AcquireTransitions(window);
        held_ = window;
    }
}

std::optional<GenieMinimize::CachedPictureHit> GenieMinimize::LookupCachedPicture(HWND window)
{
    WindowSnapshotKey key{};
    if (!QueryWindowSnapshotKey(window, 0, key)) {
        pictureCache_.Erase(window);
        pictureMeta_.erase(window);
        return std::nullopt;
    }
    const WindowPictureCache::Entry* entry = pictureCache_.Find(window, key.pid, key.creationTime);
    if (!entry || !entry->picture || !entry->picture->ok || entry->picture->image.Empty()) {
        if (const auto it = pictureMeta_.find(window); it != pictureMeta_.end() && !SameProcessKey(it->second.key, key))
            pictureMeta_.erase(it);
        return std::nullopt;
    }
    const auto metaIt = pictureMeta_.find(window);
    const RECT frame = (metaIt != pictureMeta_.end() && !IsRectEmpty(&metaIt->second.frame)) ? metaIt->second.frame : entry->frame;
    const uint64_t pictureId = (metaIt != pictureMeta_.end() && metaIt->second.pictureId != 0) ? metaIt->second.pictureId : ++nextPictureId_;
    return CachedPictureHit{entry->picture, frame, pictureId};
}

bool GenieMinimize::HasActiveHardwareMinimize(HWND window) const
{
    return std::ranges::any_of(hwFlights_, [window](const HardwareFlight& f) { return f.window == window && !f.isRestore; });
}

RECT GenieMinimize::FlightBounds(const RECT& fallback) const { return CollectFlightBounds(flights_, fallback); }

RECT GenieMinimize::HardwareFlightBounds() const { return CollectFlightBounds(hwFlights_, RECT{}); }

void GenieMinimize::Arm(const RECT& frame)
{
    if (hardwareHost_)
        return;
    const RECT bounds = FlightBounds(frame);
    if (!surface_.Visible()) {
        surface_.Show(bounds, false, POINT{bounds.left, bounds.top}, false);
    } else {
        const RECT now = surface_.Rect();
        if (!EqualRect(&now, &bounds))
            surface_.Move(bounds);
    }
    SetTimer(surface_.Hwnd(), kIdleTimer, kIdleMs, nullptr);
}

// The window in front: its picture kept fresh, and its own minimize animation off once there is one.
void GenieMinimize::Watch()
{
    if (pending_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    DWORD pid = 0;
    if (root)
        GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId() || !Minimizable(root) || (skip_ && skip_(root)) || IsGameWindow(root))
        root = nullptr;
    const ULONGLONG now = GetTickCount64();
    if (root != front_) {
        if (!held_ || !HasActiveHardwareMinimize(held_))
            Release(held_, !(skip_ && skip_(held_)));
        front_ = root;
        frontSince_ = now;
    }
    const auto inFlight = [&](HWND h) {
        return std::ranges::any_of(flights_, [h](const Flight& f) { return f.window == h; }) ||
               std::ranges::any_of(hwFlights_, [h](const HardwareFlight& f) { return f.window == h; });
    };
    std::erase_if(pictureMeta_, [&](const auto& kv) {
        const HWND h = kv.first;
        if ((h != root && !IsWindow(h)) || (h != root && h != pending_ && !IsIconic(h) && !inFlight(h))) {
            pictureCache_.Erase(h);
            return true;
        }
        return false;
    });
    if (!root)
        return;
    WindowSnapshotKey key{};
    if (!QueryWindowSnapshotKey(root, 0, key))
        return;
    PictureMeta& meta = EnsureProcessMeta(root, key);
    meta.frame = Frame(root);
    const WindowPictureCache::Entry* cached = pictureCache_.Find(root, key.pid, key.creationTime);
    if (cached && cached->picture && cached->picture->ok && target_ && target_(root))
        Hold(root);
    POINT pt{};
    GetCursorPos(&pt);
    const float s = float(GetDpiForWindow(root)) / 96.f;
    const RECT& r = meta.frame;
    const bool atButtons = PtInRect(&r, pt) && pt.y < r.top + LONG(56 * s) && pt.x > r.right - LONG(200 * s);
    if (atButtons && held_ == root)
        Arm(r);
    if (now - frontSince_ < kDrawnMs || now - meta.taken < (atButtons ? kNearMs : kFarMs))
        return;
    meta.taken = now;
    snapshots_.Request(root, 64, 64, 0, kLargeMax);
}

void GenieMinimize::Minimizing(HWND window)
{
    if (!enabled_ || window != held_)
        return;
    if (skip_ && skip_(window)) {
        Release(window, false);
        front_ = nullptr;
        return;
    }
    if (HasActiveHardwareMinimize(window)) {
        Release(window);
        front_ = nullptr;
        return;
    }
    eventMs_ = ClockMs();
    const std::optional<CachedPictureHit> hit = LookupCachedPicture(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    Release(window);
    front_ = nullptr;
    if (!hit || !to) {
        Log(L"genie: %p went down without it (picture %d, icon %d)", window, hit.has_value(), to.has_value());
        return;
    }
    if (hardwareHost_) {
        LaunchHardwareFlight(window, *hit, *to, nullptr, false, false);
        return;
    }
    Launch(Flight{hit->picture, {}, hit->frame, *to, 0, window});
}

bool GenieMinimize::Minimize(HWND window, std::function<void()> minimize)
{
    if (!enabled_ || pending_ || !Minimizable(window) || (skip_ && skip_(window)) || !target_ || !target_(window))
        return false;
    WindowSnapshotKey key{};
    QueryWindowSnapshotKey(window, 0, key);
    PictureMeta& meta = EnsureProcessMeta(window, key);
    meta.frame = Frame(window);
    meta.taken = GetTickCount64();
    Hold(window);
    front_ = window;
    Arm(meta.frame);
    pending_ = window;
    pendingMinimize_ = std::move(minimize);
    snapshots_.Request(window, 64, 64, 0, kLargeMax, true);
    SetTimer(surface_.Hwnd(), kFreshTimer, kFreshMs, nullptr);
    return true;
}

void GenieMinimize::RunPending()
{
    KillTimer(surface_.Hwnd(), kFreshTimer);
    const HWND window = std::exchange(pending_, nullptr);
    std::function<void()> minimize = std::exchange(pendingMinimize_, nullptr);
    if (!window || !minimize)
        return;
    const std::optional<CachedPictureHit> hit = LookupCachedPicture(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    if (!enabled_ || !hit || !to) {
        Release(window);
        minimize();
        return;
    }
    if (hardwareHost_) {
        eventMs_ = ClockMs();
        LaunchHardwareFlight(window, *hit, *to, std::move(minimize), false, true);
        return;
    }
    minimize();
}

bool GenieMinimize::Restore(HWND window, std::function<void()> restore)
{
    if (!enabled_ || (skip_ && skip_(window)))
        return false;
    const std::optional<CachedPictureHit> hit = LookupCachedPicture(window);
    const std::optional<RECT> to = target_ ? target_(window) : std::nullopt;
    if (!hit || !to)
        return false;
    eventMs_ = ClockMs();
    if (hardwareHost_) {
        LaunchHardwareFlight(window, *hit, *to, std::move(restore), true, false);
        return true;
    }
    Launch(Flight{hit->picture, {}, hit->frame, *to, 0, window, std::move(restore)});
    return true;
}

void GenieMinimize::LaunchHardwareFlight(HWND window, const CachedPictureHit& hit, const RECT& to, std::function<void()> cb, bool isRestore, bool waitForCover)
{
    const bool ready = PrepareHardwareFlightLaunch(
        cb,
        [this] {
            EnsureOverlayWindow();
            return hwOverlay_ != nullptr;
        },
        [this] { return SetTimer(hwOverlay_, kStateTimer, kStateMs, nullptr) != 0; }, [this, window] { Release(window); },
        [](const wchar_t* msg) { Log(L"%s", msg); });
    if (!ready)
        return;

    const double nowMs = ClockMs();
    const uint64_t id =
        isRestore ? stateMachine_.BeginRestore(window, nowMs, std::move(cb)) : stateMachine_.BeginMinimize(window, nowMs, std::move(cb), waitForCover);
    hwFlights_.push_back(HardwareFlight{id, hit.pictureId, hit.picture, hit.frame, to, window, isRestore, false, false, nowMs});
    PublishHardwareFlights();
}

void GenieMinimize::PublishHardwareFlights()
{
    if (!hardwareHost_)
        return;
    EnsureOverlayWindow();
    if (hwOverlay_)
        KillTimer(hwOverlay_, kHwIdleTimer);

    const RECT bounds = HardwareFlightBounds();
    if (hwFlights_.empty() || IsRectEmpty(&bounds)) {
        hardwareHost_->SubmitGenieFlights(GenieSceneSnapshot{0, hardwareEpoch_, hwOverlay_, {}, {}});
        if (hwOverlay_)
            SetTimer(hwOverlay_, kHwIdleTimer, kIdleMs, nullptr);
        return;
    }
    if (hwOverlay_)
        SetWindowPos(hwOverlay_, HWND_TOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const std::vector<GenieFlightFrame> frames = stateMachine_.ActiveFrames();
    GenieSceneSnapshot scene{0, hardwareEpoch_, hwOverlay_, bounds, {}};
    scene.flights.reserve(hwFlights_.size());
    for (HardwareFlight& hf : hwFlights_) {
        const auto frameIt = std::ranges::find_if(frames, [&](const GenieFlightFrame& fr) { return fr.id == hf.id; });
        if (frameIt == frames.end() || !hf.picture || !hf.picture->ok || hf.picture->image.Empty())
            continue;
        scene.flights.push_back(GenieFlightDraw{hf.id, hf.pictureId, std::shared_ptr<const IconImage>(hf.picture, &hf.picture->image), hf.picture->width,
                                                hf.picture->height, kPicturePad, hf.from, hf.to, frameIt->phase, hf.isRestore, hf.phaseStartMs});
    }
    hardwareHost_->SubmitGenieFlights(std::move(scene));
}

void GenieMinimize::TickHardwareFlights(double nowMs)
{
    if (!hardwareHost_ || hwFlights_.empty()) {
        if (hwOverlay_)
            KillTimer(hwOverlay_, kStateTimer);
        return;
    }
    const std::vector<GenieFlightFrame> before = stateMachine_.ActiveFrames();
    for (const GenieFlightFrame& fr : before) {
        if (fr.phase != GenieFlightPhase::RestoreHolding || fr.restoreObserved)
            continue;
        auto it = std::ranges::find_if(hwFlights_, [&](const HardwareFlight& hf) { return hf.id == fr.id; });
        if (it == hwFlights_.end())
            continue;
        if (!it->holdPrimed) {
            it->holdPrimed = true;
            continue;
        }
        if (!IsWindow(it->window) || (IsWindowVisible(it->window) && !IsIconic(it->window) && !IsWindowCloaked(it->window)))
            stateMachine_.NotifyRestoreContentObserved(fr.id, nowMs);
    }

    const std::vector<std::function<void()>> due = stateMachine_.Tick(nowMs);
    const std::vector<GenieFlightFrame> after = stateMachine_.ActiveFrames();
    bool needRepublish = false;
    for (HardwareFlight& hf : hwFlights_) {
        const auto prevIt = std::ranges::find_if(before, [&](const GenieFlightFrame& fr) { return fr.id == hf.id; });
        const auto nextIt = std::ranges::find_if(after, [&](const GenieFlightFrame& fr) { return fr.id == hf.id; });
        if (nextIt == after.end()) {
            needRepublish = true;
            continue;
        }
        if (prevIt == before.end() || prevIt->phase != nextIt->phase) {
            needRepublish = true;
            hf.phaseStartMs = nowMs;
            if (nextIt->phase == GenieFlightPhase::RestoreHolding && hf.window && IsWindow(hf.window) && !hf.restoreTransitions) {
                AcquireTransitions(hf.window);
                hf.restoreTransitions = true;
            }
        }
    }
    for (const auto& cb : due)
        cb();

    for (auto it = hwFlights_.begin(); it != hwFlights_.end();) {
        if (std::ranges::any_of(after, [&](const GenieFlightFrame& fr) { return fr.id == it->id; })) {
            ++it;
            continue;
        }
        if (!it->isRestore && it->window == held_)
            Release(it->window);
        if (it->restoreTransitions) {
            it->restoreTransitions = false;
            ReleaseTransitions(it->window, !(skip_ && skip_(it->window)));
        }
        it = hwFlights_.erase(it);
        needRepublish = true;
    }
    if (hwFlights_.empty() && hwOverlay_)
        KillTimer(hwOverlay_, kStateTimer);
    if (needRepublish)
        PublishHardwareFlights();
}

void GenieMinimize::OnHardwareFramePresented(uint64_t flightId, uint64_t epoch, bool pourFinished)
{
    if (!hardwareHost_ || epoch != hardwareEpoch_)
        return;
    double nowMs = ClockMs();
    if (auto it = std::ranges::find_if(hwFlights_, [flightId](const HardwareFlight& hf) { return hf.id == flightId; }); it != hwFlights_.end()) {
        stateMachine_.NotifyCoverPresented(flightId, nowMs);
        if (pourFinished)
            nowMs = std::max(nowMs, it->phaseStartMs + kGenieFlightMs);
    }
    TickHardwareFlights(nowMs);
}

void GenieMinimize::OnHardwareDeviceLost(uint64_t epoch)
{
    if (epoch != hardwareEpoch_)
        return;
    Log(L"genie: hardware device lost; completing pending callbacks safely");
    CancelHardwareFlights();
}

void GenieMinimize::CancelHardwareFlights()
{
    for (const auto& cb : DrainHardwareFlights())
        cb();
}

void GenieMinimize::Launch(Flight f)
{
    f.key = L"genie|" + std::to_wstring(++flightGen_);
    flights_.push_back(std::move(f));
    Arm(flights_.back().from);
    const RECT origin = surface_.Rect();
    const float s = surface_.Scale();
    const float maxW = float(origin.right - origin.left) / s, maxH = float(origin.bottom - origin.top) / s;
    D2D1_RECT_F area = D2D1::RectF(1e9f, 1e9f, -1e9f, -1e9f);
    for (const Flight& g : flights_)
        for (const RECT& r : {g.from, g.to})
            area = D2D1::RectF(std::min(area.left, float(r.left - origin.left) / s), std::min(area.top, float(r.top - origin.top) / s),
                               std::max(area.right, float(r.right - origin.left) / s), std::max(area.bottom, float(r.bottom - origin.top) / s));
    area =
        D2D1::RectF(std::clamp(area.left, 0.f, maxW), std::clamp(area.top, 0.f, maxH), std::clamp(area.right, 0.f, maxW), std::clamp(area.bottom, 0.f, maxH));
    if (area.right > area.left && area.bottom > area.top)
        surface_.SetRegion(area);
    else
        surface_.SetRegionNone();
    surface_.Invalidate();
}

// Out of the icon and landed: the window comes back under its picture, without an animation of its own.
void GenieMinimize::RunRestores()
{
    std::vector<std::function<void()>> due;
    for (Flight& f : flights_)
        if (f.restoreDue && f.restore) {
            std::function<void()> restore = std::exchange(f.restore, nullptr);
            if (f.window && IsWindow(f.window)) {
                if (!f.restoreTransitions) {
                    AcquireTransitions(f.window);
                    f.restoreTransitions = true;
                }
                due.push_back(std::move(restore));
            }
        }
    for (const auto& restore : due)
        restore();
}

void GenieMinimize::Paint(Canvas& c)
{
    for (auto it = flights_.begin(); it != flights_.end();) {
        if (PaintOne(c, *it)) {
            if (it->restoreTransitions) {
                it->restoreTransitions = false;
                ReleaseTransitions(it->window, !(skip_ && skip_(it->window)));
            }
            surface_.ForgetImage(it->key);
            it = flights_.erase(it);
        } else {
            ++it;
        }
    }
    if (!flights_.empty()) {
        c.Continue();
    } else {
        surface_.SetRegionNone();
        SetTimer(surface_.Hwnd(), kIdleTimer, kIdleMs, nullptr);
    }
}

bool GenieMinimize::PaintOne(Canvas& c, Flight& f)
{
    if (f.startMs <= 0) {
        f.startMs = c.now;
        Log(L"genie: %s, first frame %.0f ms after it began", f.restore ? L"restore" : L"minimize", c.now - eventMs_);
    }
    const double ms = c.now - f.startMs;
    const float t = float(std::clamp(ms / kFlightMs, 0.0, 1.0));
    if (!f.restore && !f.restoreDue) {
        PaintFlight(c, f, t, 1);
        return t >= 1;
    }
    if (t >= 1 && !f.restoreDue) {
        f.restoreDue = true;
        PostMessageW(surface_.Hwnd(), WM_GENIE_RESTORE, 0, 0);
    }
    const float fade = 1 - GenieSmoothStep(float(kFlightMs + kHoldMs), float(kFlightMs + kHoldMs + kDissolveMs), float(ms));
    PaintFlight(c, f, 1 - t, fade);
    return ms >= kFlightMs + kHoldMs + kDissolveMs && !f.restore;
}

void GenieMinimize::PaintFlight(Canvas& c, const Flight& f, float t, float alpha)
{
    const float opacity = (1 - GenieSmoothStep(0.86f, 1.f, t)) * alpha;
    if (opacity <= 0.002f)
        return;
    GenieMeshInput in;
    in.fromRect = f.from;
    in.toRect = f.to;
    const detail::GenieRowSlices s = detail::ComputeGenieRows(in, t, kRows);
    if (!s.valid)
        return;
    const WindowPicture& pic = *f.picture;
    const RECT origin = surface_.Rect();
    const float pw = float(pic.width), ph = float(pic.height), pad = float(kPicturePad), invS = 1.f / c.scale;
    const float ox = float(s.vertical ? origin.left : origin.top), oy = float(s.vertical ? origin.top : origin.left);
    const float hair = 0.5f * c.scale;
    for (int i = 0; i < kRows; ++i) {
        const auto rowIdx = static_cast<size_t>(i);
        const float p0 = s.a[rowIdx], p1 = s.a[rowIdx + 1] + hair;
        if (p1 - p0 < 0.55f * c.scale)
            continue;
        const float lo0 = (s.lo[rowIdx] - ox) * invS, hi0 = (s.hi[rowIdx] - ox) * invS;
        const float lo1 = (s.lo[rowIdx + 1] - ox) * invS, hi1 = (s.hi[rowIdx + 1] - ox) * invS;
        const float y0 = (s.sign * p0 - oy) * invS, y1 = (s.sign * p1 - oy) * invS;
        const float v0 = float(i) / kRows, v1 = float(i + 1) / kRows;
        const float s0 = s.sign > 0 ? v0 : 1 - v1, s1 = s.sign > 0 ? v1 : 1 - v0;
        std::array<D2D1_POINT_2F, 4> quad{};
        D2D1_RECT_F src{};
        if (s.vertical) {
            quad = s.sign > 0 ? std::array{D2D1::Point2F(lo0, y0), D2D1::Point2F(hi0, y0), D2D1::Point2F(hi1, y1), D2D1::Point2F(lo1, y1)} :
                                std::array{D2D1::Point2F(lo1, y1), D2D1::Point2F(hi1, y1), D2D1::Point2F(hi0, y0), D2D1::Point2F(lo0, y0)};
            src = D2D1::RectF(pad, pad + s0 * ph, pad + pw, pad + s1 * ph);
        } else {
            quad = s.sign > 0 ? std::array{D2D1::Point2F(y0, lo0), D2D1::Point2F(y1, lo1), D2D1::Point2F(y1, hi1), D2D1::Point2F(y0, hi0)} :
                                std::array{D2D1::Point2F(y1, lo1), D2D1::Point2F(y0, lo0), D2D1::Point2F(y0, hi0), D2D1::Point2F(y1, hi1)};
            src = D2D1::RectF(pad + s0 * pw, pad, pad + s1 * pw, pad + ph);
        }
        c.ImageQuad(f.key, pic.image, src, quad, opacity);
    }
}

} // namespace atrium
