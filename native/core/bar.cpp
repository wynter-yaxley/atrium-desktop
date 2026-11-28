#include "bar.h"
#include "bar_drop.h"
#include "bar_entries.h"

#include "localization.h"
#include "render/continuous_corners.h"

#include "clock.h"
#include "log.h"
#include "shell/shell_taskbar.h"
#include "render/capture.h"
#include "render/glass_renderer.h"
#include "render/shutdown_wait.h"
#include "ui/surface.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace atrium {

namespace {

constexpr UINT WM_BAR_CAPTURE = WM_APP + 0x70; // wParam 1: Atrium may show in screenshots (render thread -> UI thread)

constexpr wchar_t kVisualClass[] = L"AtriumBarVisual";
constexpr wchar_t kInputClass[] = L"AtriumBarInput";

std::map<HWINEVENTHOOK, Bar*> g_guards; // each display has its own stacking guard
std::atomic<uint64_t> g_nextDropBarId{1};
struct CaptureWindows {
    HWND visual, input, genieOverlay;
    bool visible;
};
std::map<Bar*, CaptureWindows> g_capturePolicies; // UI-thread only

void ApplyCapturePolicies()
{
    // One capture fallback must exclude every Atrium window to prevent feedback.
    const bool visible = std::ranges::all_of(g_capturePolicies, [](const auto& entry) { return entry.second.visible; });
    for (const auto& [bar, windows] : g_capturePolicies) {
        SetCaptureExclusion(windows.visual, visible);
        SetCaptureExclusion(windows.input, visible);
        if (windows.genieOverlay)
            SetCaptureExclusion(windows.genieOverlay, visible);
    }
    Surface::SetCaptureVisible(visible);
}

void RegisterClasses(HINSTANCE instance)
{
    static bool done = false;
    if (done)
        return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = DefWindowProcW; // replaced per window once `this` is attached
    wc.lpszClassName = kVisualClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kInputClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

void RefreshCaptureExclusion() { ApplyCapturePolicies(); }

UINT Bar::AppBarNotice()
{
    static const UINT kNotice = RegisterWindowMessageW(L"Atrium.AppBar");
    return kNotice;
}

int Bar::Px(double dip, float scale) { return int(std::lround(dip * scale)); }

Bar::Bar(HINSTANCE instance, bool debugCapturable, std::wstring display)
    : debugCapturable_(debugCapturable),
      dropId_(g_nextDropBarId.fetch_add(1)),
      displayOverride_(std::move(display)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      frame_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      captureAck_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      captureVisible_(debugCapturable)
{
    RegisterClasses(instance);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put()));

    // Visual: never hit-tested (layered + transparent), no redirection bitmap.
    visual_ = CreateWindowExW( // NOLINT(cppcoreguidelines-prefer-member-initializer): after RegisterClasses
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT | WS_EX_LAYERED, kVisualClass, L"Atrium Bar",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    SetLayeredWindowAttributes(visual_, 0, 255, LWA_ALPHA);
    // Input: draws nothing, so it is invisible; its window region is the shape.
    input_ = CreateWindowExW( // NOLINT(cppcoreguidelines-prefer-member-initializer): after RegisterClasses
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kInputClass, L"Atrium Bar Input", WS_POPUP, 0, 0, 1, 1, nullptr,
        nullptr, instance, nullptr);
    SetWindowLongPtrW(input_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::InputProc));
    SetWindowLongPtrW(visual_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(visual_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Bar::VisualProc));
    // Out of every capture until the render thread has a backdrop that
    // leaves them out by itself (Desktop Duplication).
    if (debugCapturable_)
        Log(L"bar: --debug-capturable, Atrium shows in screenshots (with Windows.Graphics.Capture, the backdrop freezes after the first frame)");
    SetCaptureExclusion(visual_, captureVisible_);
    SetCaptureExclusion(input_, captureVisible_);
    g_capturePolicies[this] = {visual_, input_, nullptr, debugCapturable_};
    ApplyCapturePolicies();

    // A compact Dock-style label, using the same material and adaptive ink as
    // every other surface. It never takes focus or receives pointer input.
    tooltip_ = std::make_unique<Surface>(instance, L"Atrium Hover Label", false);
    tooltip_->message = [](UINT m, WPARAM, LPARAM, LRESULT& result) { return m == WM_NCHITTEST ? (result = HTTRANSPARENT, true) : false; };
    tooltip_->paint = [this](Canvas& c) {
        auto bounds = D2D1::RectF(0, 0, tooltipSize_.width, tooltipSize_.height);
        if (c.panel)
            c.Callout(bounds, std::min(16.f, tooltipSize_.height / 2), tooltipTailX_, kTooltipTailHalfW, edge_ == BarEdge::Bottom ? kTooltipTailH : 0,
                      kTooltipTailInset, c.theme.panel);
        c.Text(tooltipText_, D2D1::RectF(kTooltipPadX, kTooltipPadY, bounds.right - kTooltipPadX, bounds.bottom - kTooltipPadY), kTooltipFont, c.theme.text,
               DWRITE_FONT_WEIGHT_MEDIUM, 1);
    };

    // Drag and drop needs OLE on this (STA) thread; balanced in the destructor.
    oleReady_ = SUCCEEDED(OleInitialize(nullptr));
    if (oleReady_) {
        dropTarget_ = winrt::make<BarDropTarget>(this);
        if (FAILED(RegisterDragDrop(input_, dropTarget_.get()))) {
            Log(L"bar: RegisterDragDrop failed; dropping files on apps is unavailable");
            dropTarget_ = nullptr;
        }
    }

    // Stacking guard: raised again when covered, hidden behind full-screen apps.
    const DWORD evs[] = {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MOVESIZEEND, EVENT_OBJECT_LOCATIONCHANGE};
    for (int i = 0; i < 3; ++i) {
        hooks_[i] = SetWinEventHook(evs[i], evs[i], nullptr, &Bar::WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (hooks_[i])
            g_guards[hooks_[i]] = this;
    }
    SetTimer(input_, kGuardTimer, kGuardMs, nullptr);

    render_ = std::thread([this] { RenderThread(); });
}

Bar::~Bar()
{
    DetachInput();
    SetEvent(quit_);
    if (render_.joinable())
        render_.join();
    if (dropTarget_)
        RevokeDragDrop(input_);
    dropTarget_ = nullptr;
    tooltip_.reset();
    g_capturePolicies.erase(this);
    ApplyCapturePolicies();
    DestroyWindow(input_);
    DestroyWindow(visual_);
    for (HANDLE h : {wake_, quit_, frame_, captureAck_})
        CloseHandle(h);
    if (oleReady_)
        OleUninitialize();
}

void Bar::DetachInput()
{
    for (HWINEVENTHOOK& h : hooks_) {
        if (h) {
            UnhookWinEvent(h);
            g_guards.erase(h);
            h = nullptr;
        }
    }
    if (input_) {
        for (UINT_PTR id : {kTooltipTimer, kFileHoverTimer, kGuardTimer, kSlideTimer, kPointerTimer, kDisplayTimer, kCoverTimer, kNearTimer})
            KillTimer(input_, id);
        if (GetCapture() == input_)
            ReleaseCapture();
    }
    if (tooltip_)
        tooltip_->Hide(false);
    if (appBar_)
        UpdateAppBar(nullptr, RECT{}, 0);
}

void Bar::SignalStopRendering()
{
    if (quit_)
        SetEvent(quit_);
}

bool Bar::WaitStopRendering(DWORD timeoutMs)
{
    if (!render_.joinable())
        return true;
    // The render thread may wait for this thread (ShowOwnWindowsToCapture, DirectComposition):
    // keep its messages flowing while waiting, never a bare join.
    if (!WaitRenderShutdown(render_.native_handle(), visual_, WM_BAR_CAPTURE, timeoutMs)) {
        Log(L"core: the render thread did not stop in %lu ms", timeoutMs);
        return false;
    }
    render_.join();
    return true;
}

bool Bar::StopRendering(DWORD timeoutMs)
{
    SignalStopRendering();
    return WaitStopRendering(timeoutMs);
}

LRESULT CALLBACK Bar::VisualProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_DPICHANGED && self) {
        PostMessageW(self->input_, WM_BAR_LAYOUT, 0, 0);
        return 0;
    }
    if (m == WM_BAR_CAPTURE && self) {
        // From the render thread: whether Atrium's windows may show in screenshots.
        bool visible = w != 0;
        g_capturePolicies[self].visible = visible;
        ApplyCapturePolicies();
        SetEvent(self->captureAck_);
        return 0;
    }
    if (m == WM_CLOSE && self) {
        if (self->onCloseRequest)
            self->onCloseRequest(); // the app decides; never destroy just this window
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// The bar's strip of the screen while it is always shown (`reserve` px on `edge_` of
// `monitor`); 0 gives it back. Windows keeps the work area: maximized windows end before the bar.
void Bar::UpdateAppBar(HMONITOR monitor, RECT bounds, int reserve)
{
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = input_;
    if (reserve <= 0 || !monitor) {
        if (appBar_) {
            AppBarMessage(ABM_REMOVE, abd);
            appBar_ = false;
            appBarReserved_ = 0;
            appBarMonitor_ = nullptr;
            appBarRect_ = RECT{};
            appBarWant_ = RECT{};
            Log(L"bar: screen strip given back");
        }
        return;
    }
    if (!appBar_) {
        abd.uCallbackMessage = AppBarNotice();
        if (!AppBarMessage(ABM_NEW, abd))
            return;
        appBar_ = true;
        appBarRect_ = RECT{};
        appBarWant_ = RECT{};
    }
    const EdgeFrame frame{edge_, bounds};
    const RECT want = frame.ToArea(RECT{0, LONG(frame.Across()) - reserve, LONG(frame.Along()), LONG(frame.Across())});
    if (appBarMonitor_ == monitor && appBarEdge_ == edge_ && EqualRect(&appBarWant_, &want))
        return;
    constexpr UINT kAppBarEdge[] = {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}; // in BarEdge's order
    abd.uEdge = kAppBarEdge[int(edge_)];
    abd.rc = want;
    UINT_PTR queryOut = 0, setOut = 0;
    if (!TryAppBarMessage(ABM_QUERYPOS, abd, queryOut)) // inside another app bar (the system taskbar, if shown) on the same edge
        return;
    RECT rc = frame.FromArea(abd.rc);
    rc.top = rc.bottom - reserve;
    abd.rc = frame.ToArea(rc);
    if (!TryAppBarMessage(ABM_SETPOS, abd, setOut))
        return;
    const RECT granted = frame.FromArea(abd.rc);
    const bool exact = EqualRect(&abd.rc, &want) != FALSE;
    const bool changed = appBarMonitor_ != monitor || appBarEdge_ != edge_ || !EqualRect(&appBarRect_, &abd.rc);
    appBarRect_ = abd.rc;
    // Leave appBarWant_ empty when clipped so the next relayout reclaims the full edge once Explorer releases it.
    appBarWant_ = exact ? want : RECT{};
    appBarReserved_ = int(std::max(0L, granted.bottom - granted.top));
    appBarMonitor_ = monitor;
    appBarEdge_ = edge_;
    if (changed) {
        if (!exact) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(monitor, &mi);
            Log(L"bar: appbar granted (%ld,%ld,%ld,%ld) != want (%ld,%ld,%ld,%ld), monitor (%ld,%ld,%ld,%ld), work (%ld,%ld,%ld,%ld)", abd.rc.left, abd.rc.top,
                abd.rc.right, abd.rc.bottom, want.left, want.top, want.right, want.bottom, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
                mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
        }
        Log(L"bar: keeps %d px of the screen's %s edge (always shown)", appBarReserved_, settings_.barEdge.c_str());
    }
}

void Bar::ReadDisplays()
{
    displays_ = ConnectedDisplays();
    displayDpis_.assign(displays_.size(), 96);
    for (size_t i = 0; i < displays_.size(); ++i)
        if (UINT dx = 96, dy = 96; SUCCEEDED(GetDpiForMonitor(displays_[i].monitor, MDT_EFFECTIVE_DPI, &dx, &dy)))
            displayDpis_[i] = dx;
}

void Bar::Apply(const Settings& s)
{
    const bool changed = settings_.dockDisplayMode != s.dockDisplayMode || settings_.dockDisplay != s.dockDisplay;
    settings_ = s;
    if (displays_.empty())
        ReadDisplays();
    if (changed)
        activeDisplay_.clear();
    summonMonitor_ = nullptr;
    summonGraceUntil_ = 0;
    WatchDisplaysIfNeeded();
    Relayout();
    UpdateCover(); // the hide mode may have changed
    UpdateNearWatch(); // hoverRise may have been turned on or off
}

// Watching the pointer for the follow mode only with another display to follow it to.
void Bar::WatchDisplaysIfNeeded()
{
    if (settings_.dockDisplayMode == L"follow" && displayOverride_.empty() && displays_.size() > 1)
        SetTimer(input_, kDisplayTimer, kPointerMs, nullptr);
    else
        KillTimer(input_, kDisplayTimer);
}

void Bar::RefreshDisplays(bool workAreaOnly)
{
    auto before = std::exchange(displays_, {});
    auto beforeDpis = std::exchange(displayDpis_, {});
    ReadDisplays();
    WatchDisplaysIfNeeded();
    const bool changed = beforeDpis != displayDpis_ || !std::ranges::equal(before, displays_, [](const DisplayInfo& a, const DisplayInfo& b) {
                             return a.monitor == b.monitor && a.id == b.id && EqualRect(&a.bounds, &b.bounds) && a.primary == b.primary;
                         });
    const bool monitorGone = hmonitor_ && std::ranges::none_of(displays_, [&](const DisplayInfo& d) { return d.monitor == hmonitor_; });
    if ((changed && !workAreaOnly) || monitorGone)
        ++displayGeneration_; // HMONITOR can be reused after a mode or adapter change.
    summonMonitor_ = nullptr;
    summonGraceUntil_ = 0;
    if (dragging_)
        EndDrag(false);
    Relayout();
    GuardStacking(L"display");
}

void Bar::WatchDisplays()
{
    // Never move a Dock while its icons, a file drop, or a popup are being used.
    if (!visible_ || dragging_ || pressedLeft_ || !fileHoverKey_.empty() || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (keepShown && keepShown()) ||
        (previewOpen && previewOpen())) {
        summonMonitor_ = nullptr;
        return;
    }
    POINT point{};
    GetCursorPos(&point);
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    auto target = std::ranges::find_if(displays_, [&](const DisplayInfo& d) { return d.monitor == monitor; });
    if (monitor && target == displays_.end()) {
        ReadDisplays();
        target = std::ranges::find_if(displays_, [&](const DisplayInfo& d) { return d.monitor == monitor; });
    }
    // The pointer must stay at the actual edge; screen crossings do not summon the Dock.
    if (target == displays_.end() || monitor == hmonitor_ || !AtDisplayEdge(edge_, point, target->bounds, FollowEdgeMargin(summonMonitor_ == monitor))) {
        summonMonitor_ = nullptr;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (summonMonitor_ != monitor) {
        summonMonitor_ = monitor;
        summonAt_ = now;
        return;
    }
    if (double(now - summonAt_) < settings_.dockDisplayDelay)
        return;
    activeDisplay_ = target->id;
    summonMonitor_ = nullptr;
    KillTimer(input_, kSlideTimer);
    covered_ = revealed_ = tucked_ = slidOut_ = scene_.tucked = false;
    HideTooltip(), scene_.hoverKey.clear(), scene_.pointerX = scene_.pointerY = -1;
    SetRegion(false);
    Relayout();
    UpdateCover();
    // A summoned Dock stays visible long enough for the pointer to reach it.
    if (!FullScreenInFront(hmonitor_)) {
        revealed_ = true, leftAt_ = now, summonGraceUntil_ = now + 1200;
        Raise();
    }
    UpdateAutoHide();
    UpdateNearWatch();
}

void Bar::SetItems(std::vector<TaskItem> items)
{
    // Title changes (a browser switching tabs) only affect the tooltips; skip
    // the relayout and the redraw unless something visible changed.
    auto visible = [](const std::vector<TaskItem>& v) {
        std::wstring sig;
        for (const auto& t : v)
            sig += t.key + L"|" + std::to_wstring(!t.windows.empty()) + std::to_wstring(t.active) + std::to_wstring(t.attention) + L"|" +
                   std::to_wstring(t.badge) + L"|" + std::to_wstring(t.iconVersion) + L"\n";
        return sig;
    };
    bool same = visible(items) == visible(allItems_);
    allItems_ = std::move(items);
    if (filterTasks_)
        same = false; // monitor membership can change without the group's window count changing
    if (same) {
        items_ = allItems_;
        UpdateTooltips();
    } else {
        Relayout();
    }
}

void Bar::SetRegion(bool extended)
{
    regionExtended_ = extended;
    const int barW = int(scene_.bar.right - scene_.bar.left), barH = int(scene_.bar.bottom - scene_.bar.top);
    const int ext = extended ? rise_ : 0;
    const int rgnMetrics[7] = {barW, barH, radius_, insetX_, insetTop_, ext, int(edge_)};
    if (std::memcmp(appliedRgn_, rgnMetrics, sizeof(rgnMetrics)) == 0)
        return;
    RECT cr{};
    GetClientRect(input_, &cr);
    const EdgeFrame frame{edge_, cr};
    const RECT shape = frame.ToArea(RECT{insetX_, insetTop_, insetX_ + barW, insetTop_ + barH});
    HRGN rgn = ContinuousRegion(RECT{shape.left, shape.top, shape.right + 1, shape.bottom + 1}, radius_);
    if (ext > 0) {
        // While the pointer is on the bar, the room on its inner side belongs to the
        // raised glass and icons (clicks go to the icon under the pointer).
        const RECT inner = frame.ToArea(RECT{insetX_ + radius_ / 2, insetTop_ - ext, insetX_ + barW - radius_ / 2, insetTop_ + radius_});
        HRGN top = CreateRectRgn(inner.left, inner.top, inner.right, inner.bottom);
        CombineRgn(rgn, rgn, top, RGN_OR);
        DeleteObject(top);
    }
    if (!SetWindowRgn(input_, rgn, FALSE))
        DeleteObject(rgn);
    std::memcpy(appliedRgn_, rgnMetrics, sizeof(rgnMetrics));
}

void Bar::ActivateIndex(int index)
{
    if (index < 0 || index >= int(items_.size()) || !onActivate)
        return;
    const TaskItem& t = items_[size_t(index)];
    Log(L"bar: app %d by the keyboard (%s)", index + 1, t.name.c_str());
    if (t.windows.empty())
        Bounce(t.key);
    onActivate(std::wstring(t.key)); // the copy: activating may change the items
}

void Bar::Bounce(const std::wstring& key)
{
    Log(L"bar: bounce %s", key.c_str());
    double now = ClockMs();
    bounces_[key] = now;
    for (auto& it : scene_.items)
        if (it.key == key)
            it.bounceMs = now;
    Publish();
}

void Bar::SetPanel(const GlassPanel& panel)
{
    if (panel.visible)
        panels_[panel.hwnd] = panel;
    else
        panels_.erase(panel.hwnd);
    Publish(false);
}

void Bar::SetGenieOverlay(HWND hwnd, uint64_t epoch)
{
    {
        std::lock_guard g(genieLock_);
        genieOverlay_ = hwnd;
        genieEpoch_ = epoch;
        genieScene_.overlayHwnd = hwnd;
        genieScene_.epoch = epoch;
        ++genieScene_.seq;
        genieSeq_.store(genieScene_.seq, std::memory_order_release);
    }
    if (const auto it = g_capturePolicies.find(this); it != g_capturePolicies.end()) {
        it->second.genieOverlay = hwnd;
        ApplyCapturePolicies();
    }
    WakeRender();
}

void Bar::SubmitGenieFlights(GenieSceneSnapshot scene)
{
    if (onGenieScene)
        onGenieScene(scene, scene.cutoutRects);
    {
        std::lock_guard g(genieLock_);
        scene.seq = genieScene_.seq + 1;
        if (!scene.overlayHwnd)
            scene.overlayHwnd = genieOverlay_;
        if (scene.epoch == 0)
            scene.epoch = genieEpoch_;
        genieScene_ = std::move(scene);
        genieSeq_.store(genieScene_.seq, std::memory_order_release);
    }
    WakeRender();
}

void Bar::SubmitBarGenieOverlay(GenieSceneSnapshot scene)
{
    scene.overlayHwnd = nullptr;
    {
        std::lock_guard g(genieLock_);
        scene.seq = genieScene_.seq + 1;
        genieScene_ = std::move(scene);
        genieSeq_.store(genieScene_.seq, std::memory_order_release);
    }
    WakeRender();
}

void Bar::ClearGenieFlights()
{
    if (onGenieScene) {
        std::vector<RECT> unusedCutouts;
        onGenieScene(GenieSceneSnapshot{}, unusedCutouts);
    }
    {
        std::lock_guard g(genieLock_);
        genieScene_.flights.clear();
        genieScene_.cutoutRects.clear();
        genieScene_.overlayBounds = {};
        genieScene_.overlayHwnd = nullptr;
        ++genieScene_.seq;
        genieSeq_.store(genieScene_.seq, std::memory_order_release);
    }
    WakeRender();
}

void Bar::NotifyGenieDeviceLost()
{
    HWND hwnd = nullptr;
    uint64_t epoch = 0;
    bool hadFlights = false;
    {
        std::lock_guard g(genieLock_);
        hwnd = genieOverlay_;
        epoch = genieEpoch_;
        hadFlights = !genieScene_.flights.empty();
        genieScene_.flights.clear();
        ++genieScene_.seq;
        genieSeq_.store(genieScene_.seq, std::memory_order_release);
    }
    if (hwnd && hadFlights)
        PostMessageW(hwnd, WM_GENIE_DEVICE_LOST, static_cast<WPARAM>(epoch), 0);
}

void CALLBACK Bar::WinEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD)
{
    auto found = g_guards.find(hook);
    if (found == g_guards.end() || idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
        return;
    Bar* bar = found->second;
    if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (bar->filterTasks_ && !bar->layoutPending_) {
            const bool known =
                std::ranges::any_of(bar->allItems_, [&](const TaskItem& item) { return std::ranges::find(item.windows, hwnd) != item.windows.end(); });
            const bool shown =
                std::ranges::any_of(bar->items_, [&](const TaskItem& item) { return std::ranges::find(item.windows, hwnd) != item.windows.end(); });
            if (known && shown != (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == bar->hmonitor_)) {
                bar->layoutPending_ = true;
                PostMessageW(bar->input_, WM_BAR_LAYOUT, 0, 0);
            }
        }
        // Windows move many times a second while dragged: the windows of the monitor are walked
        // once for each burst, not for every step.
        if ((hwnd == GetForegroundWindow() || MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) == bar->hmonitor_) && !bar->coverPending_) {
            bar->coverPending_ = true;
            SetTimer(bar->input_, kCoverTimer, kCoverMs, nullptr);
        }
        return;
    }
    if (bar->settings_.taskbarApps != L"all")
        bar->Relayout();
    bar->GuardStacking(event == EVENT_SYSTEM_FOREGROUND ? L"foreground" : L"move");
}

void Bar::ExpectCover(UINT ms)
{
    expectCoverUntil_ = GetTickCount64() + ms;
    UpdateCover();
    coverPending_ = true;
    SetTimer(input_, kCoverTimer, ms + 20, nullptr); // then by what is really in front
}

void Bar::Reveal()
{
    if (!covered_ || revealed_ || !visible_ || FullScreenInFront(hmonitor_))
        return;
    revealed_ = true;
    leftAt_ = 0;
    UpdateAutoHide();
}

bool Bar::RevealOverGame()
{
    if (!visible_ || !tucked_)
        return false;
    const ULONGLONG now = GetTickCount64();
    revealed_ = true;
    leftAt_ = now;
    summonGraceUntil_ = now + 2500;
    UpdateAutoHide(); // raised without activation (Raise)
    return true;
}

void Bar::UpdateAutoHide()
{
    bool tuck = covered_ && !revealed_ && visible_;
    if (tuck == tucked_)
        return;
    tucked_ = scene_.tucked = tuck;
    onTuckChanged();
    onDropLayoutChanged();
    if (tuck) {
        HideTooltip();
        SetRegion(false);
        // The painter's slide out takes kSlideOutMs; then the windows go.
        SetTimer(input_, kSlideTimer, UINT(BarPainter::kSlideOutMs) + 60, nullptr);
        UpdateNearWatch();
        Publish();
        return;
    }
    KillTimer(input_, kSlideTimer);
    if (slidOut_) {
        slidOut_ = false;
        Relayout(); // shows the windows; the painter slides the bar up
    } else {
        Publish();
    }
    UpdateNearWatch();
    if (visible_)
        Raise();
}

void Bar::SetStatus(const SystemStatus& status)
{
    if (status == status_)
        return;
    status_ = status;
    scene_.status = status;
    Publish();
    UpdateTooltips();
}

void Bar::SetIme(const std::wstring& glyph)
{
    if (glyph == scene_.imeGlyph)
        return;
    scene_.imePrevGlyph = scene_.imeGlyph;
    scene_.imeGlyph = glyph;
    scene_.imeMs = ClockMs();
    Publish(); // content only: the glass does not change
}

void Bar::SetUnread(int count)
{
    if (count == unread_)
        return;
    unread_ = count;
    Relayout();
}

void Bar::SetDoNotDisturb(bool on)
{
    if (on == dnd_)
        return;
    dnd_ = on;
    Relayout();
}

void Bar::Publish(bool bar)
{
    {
        std::lock_guard g(lock_);
        snapshot_.settings = settings_;
        snapshot_.scene = scene_;
        snapshot_.window = window_;
        snapshot_.monitor = monitor_;
        snapshot_.hmonitor = hmonitor_;
        snapshot_.displayGeneration = displayGeneration_;
        snapshot_.scale = scale_;
        snapshot_.visible = visible_ && !slidOut_;
        snapshot_.scene.music = music_;
        snapshot_.panels.clear();
        for (const auto& [hwnd, p] : panels_)
            snapshot_.panels.push_back(p);
        if (bar)
            ++snapshot_.generation;
        publishSeq_.store(publishSeq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
    SetEvent(wake_);
}

// The bar's time and date, in the system's own formats. `seconds` asks for the
// long time format (which carries seconds); a locale whose long time has none
// gets them added after the minutes, so the switch always shows a difference.
void Bar::ClockStrings(std::wstring& time, std::wstring& date, bool seconds)
{
    wchar_t t[64] = {}, d[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, seconds ? 0 : TIME_NOSECONDS, nullptr, nullptr, t, 64);
    if (seconds) {
        wchar_t plain[64] = {};
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, plain, 64);
        if (std::wstring(t) == plain) {
            wchar_t pattern[64] = {};
            if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, pattern, ARRAYSIZE(pattern)) > 0) {
                std::wstring withSeconds = pattern;
                const size_t minute = withSeconds.rfind(L'm');
                if (minute != std::wstring::npos) {
                    withSeconds.insert(minute + 1, L":ss");
                    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, withSeconds.c_str(), t, ARRAYSIZE(t));
                }
            }
        }
    }
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, nullptr, nullptr, d, 64, nullptr);
    time = t;
    date = d;
}

std::unique_ptr<GlassRenderer> Bar::TryCreatePanelGlass(const Gpu& gpu, HWND hwnd, int width, int height, bool desktop) const
{
    try {
        return std::make_unique<GlassRenderer>(gpu, hwnd, width, height, false, false, desktop);
    } catch (const winrt::hresult_error& e) {
        Log(L"render: popup glass unavailable: 0x%08X", static_cast<unsigned>(e.code()));
        return nullptr;
    }
}

bool Bar::ShowOwnWindowsToCapture(bool visible)
{
    if (WaitForSingleObject(quit_, 0) != WAIT_TIMEOUT)
        return false;
    if (debugCapturable_)
        visible = true;
    if (visible == captureVisible_)
        return true;
    // The windows belong to the UI thread; it answers once they are changed.
    ResetEvent(captureAck_);
    PostMessageW(visual_, WM_BAR_CAPTURE, visible ? 1 : 0, 0);
    if (!WaitCaptureAckOrQuit(quit_, captureAck_, 1000)) {
        if (WaitForSingleObject(quit_, 0) != WAIT_TIMEOUT)
            return false;
        Log(L"render: the UI thread did not confirm the capture exclusion in time");
    }
    captureVisible_ = visible;
    Log(L"render: Atrium %s screenshots", visible ? L"shows in" : L"is kept out of");
    return true;
}

void Bar::RenderThread()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        SessionEnd end = SessionEnd::Retry;
        try {
            end = RenderSession();
        } catch (const winrt::hresult_error& e) {
            Log(L"render: 0x%08X %s; rebuilding in 1 s", static_cast<unsigned>(e.code()), e.message().c_str());
        }
        if (end == SessionEnd::Retry)
            NotifyGenieDeviceLost();
        if (end == SessionEnd::Exit || (end == SessionEnd::Retry && WaitForSingleObject(quit_, 1000) != WAIT_TIMEOUT))
            break;
    }
    winrt::uninit_apartment();
}

} // namespace atrium
