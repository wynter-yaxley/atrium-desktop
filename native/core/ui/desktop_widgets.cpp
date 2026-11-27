#include "ui/widget_internal.h"
#include "ui/widget_refresh_policy.h"
#include "ui/widget_stacking.h"

#include "clock.h"
#include "log.h"
#include "motion.h"
#include "../posted.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace atrium {

using namespace widget_detail;

namespace {

DesktopWidgets* g_widgets = nullptr; // the WinEvent hook's way back

} // namespace

DesktopWidgets::DesktopWidgets(HINSTANCE instance)
    : instance_(instance),
      host_([instance] {
          WNDCLASSEXW wc{sizeof(wc)};
          wc.lpfnWndProc = HostProc;
          wc.hInstance = instance;
          wc.lpszClassName = kHostClass;
          RegisterClassExW(&wc);
          // A hidden top-level window: timers, display changes, the menu's owner.
          return CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Atrium Widgets", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
      }()),
      weather_(std::make_unique<WeatherSource>(host_, WM_WIDGET_WEATHER)),
      bluetooth_(std::make_unique<BluetoothBatteries>(host_, WM_WIDGET_BLUETOOTH)),
      network_(std::make_unique<NetworkMonitor>())
{
    g_widgets = this;
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    world_.city = LocalCity();
    world_.lunar = &lunar_;
    world_.weather = weather_->Latest(); // the last one, until the first fetch
    world_.network = network_.get();
}

void DesktopWidgets::SetMaterial(WidgetMaterial material)
{
    if (material == material_)
        return;
    material_ = material;
    for (auto& w : widgets_)
        if (w->surface) {
            w->surface->SetGlassless(material_ == WidgetMaterial::Color);
            w->surface->Invalidate();
        }
}

void DesktopWidgets::ReloadLanguage()
{
    if (weather_)
        weather_->Refresh();
    for (auto& w : widgets_)
        if (w->surface)
            w->surface->Invalidate();
}

DesktopWidgets::~DesktopWidgets()
{
    SetEnabled(false);
    {
        std::lock_guard g(photoMu_);
        photoStop_ = true;
        photoJobs_.clear();
    }
    photoCv_.notify_one();
    if (photoWorker_.joinable())
        photoWorker_.join();
    if (host_)
        DestroyWindow(host_);
    g_widgets = nullptr;
}

void DesktopWidgets::SetEnabled(bool on)
{
    if (on == enabled_)
        return;
    enabled_ = on;
    if (on) {
        if (onStatusRequest)
            onStatusRequest();
        SampleSystem();
        Load();
        Build();
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0, flags);
        minHook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnWinEvent, 0, 0, flags);
        // A window that appears without taking the foreground, or uncloaks
        // after it did (WinUI), must also send raised widgets back down.
        showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnWinEvent, 0, 0, flags);
        uncloakHook_ = SetWinEventHook(EVENT_OBJECT_UNCLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnWinEvent, 0, 0, flags);
        SetTimer(host_, kTickTimer, 1000, nullptr);
        SetTimer(host_, kSafetyTimer, 3000, nullptr);
        Restack();
        Log(L"widgets: on (%zu)", widgets_.size());
    } else {
        for (HWINEVENTHOOK* h : {&hook_, &minHook_, &showHook_, &uncloakHook_})
            if (*h)
                UnhookWinEvent(std::exchange(*h, nullptr));
        KillTimer(host_, kTickTimer);
        KillTimer(host_, kSafetyTimer);
        weather_->SetActive(false);
        bluetooth_->SetActive(false);
        network_->SetActive(false);
        KillTimer(host_, kCaretTimer);
        if (KillTimer(host_, kSaveTimer))
            Save(); // typing not saved yet
        Clear();
        Log(L"widgets: off");
    }
}

void DesktopWidgets::SetMonitor(HMONITOR monitor, bool refresh)
{
    if (monitor == monitor_ && !refresh)
        return;
    monitor_ = monitor;
    if (enabled_) {
        for (auto& w : widgets_)
            Place(*w, false);
        Restack();
    }
}

void DesktopWidgets::OnShellRestart()
{
    wasShowDesktop_ = false;
    Restack();
}

void DesktopWidgets::SetStatus(const SystemStatus& status)
{
    bool batteryChanged =
        world_.status.battery != status.battery || world_.status.batteryPercent != status.batteryPercent || world_.status.charging != status.charging;
    world_.status = status;
    if (batteryChanged)
        InvalidateKind(WidgetKind::Battery);
}

void DesktopWidgets::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, true);
}

void DesktopWidgets::SetBarEdge(BarEdge edge)
{
    if (edge == barEdge_)
        return;
    barEdge_ = edge;
    if (enabled_)
        for (auto& w : widgets_)
            Place(*w, true);
}

// ---- windows -----------------------------------------------------------------

DesktopWidgets::Widget& DesktopWidgets::Create(WidgetKind kind, WidgetSize size, int col, int row)
{
    widgets_.push_back(std::make_unique<Widget>(Widget{kind, size, col, row}));
    return *widgets_.back();
}

bool DesktopWidgets::Has(const Widget* w) const
{
    return std::ranges::any_of(widgets_, [&](const auto& p) { return p.get() == w; });
}

void DesktopWidgets::Build()
{
    for (auto& wp : widgets_) {
        Widget& w = *wp;
        if (w.surface)
            continue;
        // Notes and reminders take the keyboard: their windows can be activated.
        w.surface = std::make_unique<Surface>(instance_, L"Atrium Widget", Editable(w.kind), 14.f, true);
        w.surface->SetRadius(kWidgetRadius);
        w.surface->SetGlassless(material_ == WidgetMaterial::Color); // its own face, no glass under it
        w.surface->paint = [this, &w](Canvas& c) { Paint(w, c); };
        w.surface->message = [this, &w](UINT m, WPARAM wp2, LPARAM lp, LRESULT& r) { return Message(w, m, wp2, lp, r); };
        Place(w, false);
        if (w.kind == WidgetKind::Photos)
            LoadPhoto(w, true);
    }
}

void DesktopWidgets::Clear()
{
    for (auto& w : widgets_)
        if (w->surface)
            w->surface->Hide(false);
    widgets_.clear();
}

void DesktopWidgets::Place(Widget& w, bool glide)
{
    if (!w.surface)
        return;
    RECT to = CellRect(w.col, w.row, w.size);
    if (!w.surface->Visible()) {
        w.surface->Show(to, false, POINT{(to.left + to.right) / 2, (to.top + to.bottom) / 2}, true);
        Restack();
        return;
    }
    if (!glide) {
        w.glideMs = 0;
        w.surface->Move(to);
        return;
    }
    RECT current = w.surface->Rect();
    if (current.right - current.left != to.right - to.left || current.bottom - current.top != to.bottom - to.top)
        w.surface->ReserveResize(D2D1::SizeF(WidgetWidth(WidgetSupports(w.kind, WidgetSize::Medium) ? WidgetSize::Medium : WidgetSize::Small),
                                             WidgetHeight(WidgetSupports(w.kind, WidgetSize::Large) ? WidgetSize::Large : WidgetSize::Small)));
    w.glideFrom = w.surface->Rect();
    w.glideTo = to;
    w.glideMs = ClockMs();
    SetTimer(host_, kGlideTimer, 8, nullptr);
}

void DesktopWidgets::Glide()
{
    bool any = false;
    double now = ClockMs();
    for (auto& w : widgets_) {
        if (w->glideMs <= 0 || !w->surface)
            continue;
        float k = motion::EaseOut(float((now - w->glideMs) / kGlideMs));
        // Size changes too (a widget made larger or smaller).
        auto lerp = [&](LONG a, LONG b) { return LONG(std::lround(float(a) + float(b - a) * k)); };
        RECT r{lerp(w->glideFrom.left, w->glideTo.left), lerp(w->glideFrom.top, w->glideTo.top), lerp(w->glideFrom.right, w->glideTo.right),
               lerp(w->glideFrom.bottom, w->glideTo.bottom)};
        w->surface->Move(r);
        if (k >= 1) {
            w->glideMs = 0;
            if (!w->resizing)
                w->surface->ReserveResize(D2D1::SizeF());
        } else
            any = true;
    }
    if (!any)
        KillTimer(host_, kGlideTimer);
}

bool DesktopWidgets::Add(WidgetKind kind, WidgetSize size)
{
    if (!WidgetSupports(kind, size))
        size = WidgetSize::Small;
    int c = 0, r = 0;
    if (!FreeCellNear(0, 0, size, nullptr, c, r)) {
        Log(L"widgets: no room for another");
        return false;
    }
    Create(kind, size, c, r);
    if (enabled_)
        Build();
    Save();
    Log(L"widgets: added %s %s at %d,%d", NameOf(kind), SizeName(size), c, r);
    return true;
}

// Its window may be handling the message that asked for this.
void DesktopWidgets::Remove(Widget& w) { PostMessageW(host_, WM_WIDGET_REMOVE, 0, reinterpret_cast<LPARAM>(&w)); }

void DesktopWidgets::RemoveNow(Widget& w)
{
    auto it = std::ranges::find_if(widgets_, [&](const auto& p) { return p.get() == &w; });
    if (it == widgets_.end())
        return;
    std::unique_ptr<Widget> gone = std::move(*it);
    widgets_.erase(it);
    if (gone->surface)
        gone->surface->Hide(false);
    Save();
    Log(L"widgets: removed %s", NameOf(gone->kind));
}

void DesktopWidgets::Resize(Widget& w, WidgetSize size)
{
    if (w.size == size || !WidgetSupports(w.kind, size))
        return;
    int c = w.col, r = w.row;
    if (!Fits(c, r, size, &w) && !FreeCellNear(float(c), float(r), size, &w, c, r))
        return;
    w.size = size;
    w.col = c;
    w.row = r;
    Place(w, true);
    w.surface->Invalidate();
    if (w.kind == WidgetKind::Photos)
        LoadPhoto(w, false); // the same photo, cropped for the new size
    Save();
}

bool DesktopWidgets::IsOurs(HWND h) const
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

// ---- the layer -----------------------------------------------------------------

void CALLBACK DesktopWidgets::OnWinEvent(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD)
{
    if (!g_widgets || obj != OBJID_WINDOW || child != CHILDID_SELF || !h || GetAncestor(h, GA_ROOT) != h)
        return;
    if (!g_widgets->restackPosted_) {
        g_widgets->restackPosted_ = true; // a burst of events, one restack right after it
        PostMessageW(g_widgets->host_, WM_WIDGET_RESTACK, 0, 0);
    }
}

void DesktopWidgets::LogRestackAnomaly(const WidgetStackPlan& plan, HWND fg, DWORD swpErr)
{
    const std::wstring fgCls = fg ? WindowClassOf(fg) : L"(none)", hostCls = plan.desktopClass.empty() ? L"(none)" : plan.desktopClass,
                       lowCls = plan.lowestClass.empty() ? L"(none)" : plan.lowestClass;
    const std::wstring sig = std::to_wstring(plan.showDesktop) + L"|" + hostCls + L"|" + lowCls + L"|" + fgCls + L"|" + std::to_wstring(swpErr);
    const ULONGLONG now = GetTickCount64();
    if (sig == lastRestackSig_ && now - lastRestackLog_ < 30000)
        return;
    lastRestackSig_ = sig;
    lastRestackLog_ = now;
    const wchar_t* kind = plan.showDesktop ? L"show-desktop" : L"swp-failed";
    Log(L"widgets: restack %s (host=%s, lowest=%s, fg=%s, err=%lu)", kind, hostCls.c_str(), lowCls.c_str(), fgCls.c_str(), swpErr);
}

void DesktopWidgets::Restack()
{
    if (!enabled_ || widgets_.empty())
        return;
    zMoving_ = true;
    MONITORINFO mi{sizeof(mi)};
    if (!monitor_ || !GetMonitorInfoW(monitor_, &mi))
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    std::vector<HWND> hwnds;
    for (auto& w : widgets_)
        if (w->surface)
            hwnds.push_back(w->surface->Hwnd());
    HWND fg = GetForegroundWindow();
    const WidgetStackPlan plan =
        ComputeWidgetStackPlan(CollectStackSnapshot(hwnds), mi.rcMonitor, fg ? GetAncestor(fg, GA_ROOT) : nullptr, fg && IsOurs(fg), wasShowDesktop_);
    wasShowDesktop_ = plan.showDesktop;
    DWORD swpErr = 0;
    ApplyWidgetZOrder(hwnds, plan, swpErr);
    if (plan.showDesktop || swpErr != 0)
        LogRestackAnomaly(plan, fg, swpErr);
    zMoving_ = false;
}

void DesktopWidgets::OnDisplayChanged()
{
    if (!enabled_)
        return;
    MONITORINFO mi{sizeof(mi)};
    if (!monitor_ || !GetMonitorInfoW(monitor_, &mi)) {
        monitor_ = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        Log(L"widgets: display missing; falling back to primary %p", monitor_);
    }
    for (auto& wd : widgets_)
        Place(*wd, false);
    Restack();
}

void DesktopWidgets::BlinkCaret()
{
    bool any = false;
    for (auto& wd : widgets_) {
        if (!wd->content.editing)
            continue;
        wd->content.caretOn = !wd->content.caretOn;
        wd->surface->Invalidate();
        any = true;
    }
    if (!any)
        KillTimer(host_, kCaretTimer);
}

void DesktopWidgets::OnHostTimer(WPARAM timerId)
{
    switch (timerId) {
    case kTickTimer: Tick(); break;
    case kSafetyTimer: Restack(); break;
    case kGlideTimer: Glide(); break;
    case kSaveTimer:
        KillTimer(host_, kSaveTimer);
        Save();
        break;
    case kCaretTimer: BlinkCaret(); break;
    default: break;
    }
}

void DesktopWidgets::OnPhotoReady(LPARAM lp)
{
    auto read = TakePosted<PhotoRead>(lp);
    auto* wd = static_cast<Widget*>(read->widget);
    if (!Has(wd) || read->gen != wd->photoGen || !wd->surface)
        return; // removed, or a newer photo is on its way
    WidgetContent& c = wd->content;
    if (!c.imageKey.empty())
        wd->surface->ForgetImage(c.imageKey);
    c.photo = read->path;
    c.image = read->image.Empty() ? nullptr : std::make_shared<const IconImage>(std::move(read->image));
    c.imageKey = L"photo:" + std::to_wstring(read->gen);
    wd->photoAt = GetTickCount64();
    wd->surface->Invalidate();
}

void DesktopWidgets::InvalidateKind(WidgetKind kind)
{
    for (auto& wd : widgets_)
        if (wd->kind == kind && wd->surface)
            wd->surface->Invalidate();
}

LRESULT CALLBACK DesktopWidgets::HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<DesktopWidgets*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_TIMER: self->OnHostTimer(w); return 0;
    case WM_WIDGET_RESTACK:
        self->restackPosted_ = false;
        self->Restack();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: self->OnDisplayChanged(); return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETWORKAREA)
            self->OnDisplayChanged();
        return 0;
    case WM_WIDGET_REMOVE:
        if (auto* gone = reinterpret_cast<Widget*>(l); self->Has(gone))
            self->RemoveNow(*gone);
        return 0;
    case WM_WIDGET_PHOTO: self->OnPhotoReady(l); return 0;
    case WM_WIDGET_WEATHER:
        self->world_.weather = self->weather_->Latest();
        self->InvalidateKind(WidgetKind::Weather);
        return 0;
    case WM_WIDGET_BLUETOOTH:
        self->world_.bluetooth = self->bluetooth_->Latest();
        self->world_.bluetoothRead = self->bluetooth_->Read();
        self->InvalidateKind(WidgetKind::Bluetooth);
        return 0;
    case WM_TIMECHANGE:
        self->world_.city = LocalCity();
        self->lastMinute_ = -1;
        self->Tick();
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void DesktopWidgets::Tick()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    bool minute = t.wMinute != lastMinute_;
    lastMinute_ = t.wMinute;
    if ((minute || t.wSecond % 30 == 0) && onStatusRequest)
        onStatusRequest();
    bool sample = t.wSecond % 2 == 0 && std::ranges::any_of(widgets_, [](const auto& w) { return w->kind == WidgetKind::System; });
    if (sample)
        SampleSystem();
    ULONGLONG now = GetTickCount64();
    // The forecast is fetched while a weather widget is on the desktop.
    weather_->SetActive(std::ranges::any_of(widgets_, [](const auto& w) { return w->kind == WidgetKind::Weather; }));
    // The Bluetooth batteries are read while a Bluetooth widget is.
    bluetooth_->SetActive(std::ranges::any_of(widgets_, [](const auto& w) { return w->kind == WidgetKind::Bluetooth; }));
    // The link's rates and the minute of them behind them, once a second while a network widget is up.
    const bool watching = std::ranges::any_of(widgets_, [](const auto& w) { return w->kind == WidgetKind::Network; });
    network_->SetActive(watching);
    if (watching)
        network_->Sample();
    for (auto& w : widgets_) {
        if (!w->surface)
            continue;
        if (w->kind == WidgetKind::Clock || (minute && WidgetNeedsMinuteRefresh(w->kind)) || (sample && w->kind == WidgetKind::System) ||
            (watching && w->kind == WidgetKind::Network))
            w->surface->Invalidate();
        if (w->kind == WidgetKind::Photos && w->photoAt && now - w->photoAt > kPhotoMs)
            LoadPhoto(*w, true);
    }
}

void DesktopWidgets::SampleSystem()
{
    auto u64 = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        uint64_t i = u64(idle), total = u64(kernel) + u64(user); // kernel time includes idle time
        if (cpuTotal_ && total > cpuTotal_)
            world_.cpu = std::clamp(1.f - float(double(i - cpuIdle_) / double(total - cpuTotal_)), 0.f, 1.f);
        cpuIdle_ = i;
        cpuTotal_ = total;
    }
    constexpr double kGb = 1024.0 * 1024 * 1024;
    if (MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms)) {
        world_.memory = float(ms.dwMemoryLoad) / 100.f;
        world_.memoryTotalGb = double(ms.ullTotalPhys) / kGb;
        world_.memoryGb = double(ms.ullTotalPhys - ms.ullAvailPhys) / kGb;
    }
    if (const ULONGLONG now = GetTickCount64(); ShouldSampleDisk(diskSampled_, now, lastDiskSampleMs_)) {
        diskSampled_ = true;
        lastDiskSampleMs_ = now;
        wchar_t windows[MAX_PATH] = {};
        GetWindowsDirectoryW(windows, MAX_PATH);
        ULARGE_INTEGER avail{}, total{}, free{};
        if (GetDiskFreeSpaceExW(std::wstring(windows).substr(0, 3).c_str(), &avail, &total, &free) && total.QuadPart) {
            world_.disk = 1.f - float(double(free.QuadPart) / double(total.QuadPart));
            world_.diskFreeGb = double(avail.QuadPart) / kGb;
        }
    }
}

// ---- painting ------------------------------------------------------------------

// Without glass there is no glass shadow: a soft one of its own in the margin, a little lower
// than the face, as under macOS's widgets. Only around the face (it is opaque, and already drawn).
void DesktopWidgets::PaintShadow(Canvas& c, D2D1_RECT_F face)
{
    ID2D1DeviceContext* d2d = c.D2D();
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    winrt::com_ptr<ID2D1RectangleGeometry> outer;
    winrt::com_ptr<ID2D1RoundedRectangleGeometry> inner;
    winrt::com_ptr<ID2D1PathGeometry> ring;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    const D2D1_RECT_F px = c.Px(face);
    const float r = kWidgetRadius * c.scale, room = 14 * c.scale;
    if (FAILED(factory->CreateRectangleGeometry(D2D1::RectF(px.left - room, px.top - room, px.right + room, px.bottom + room), outer.put())) ||
        FAILED(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(px, r, r), inner.put())) || FAILED(factory->CreatePathGeometry(ring.put())) ||
        FAILED(ring->Open(sink.put())) || FAILED(outer->CombineWithGeometry(inner.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get())) ||
        FAILED(sink->Close()))
        return;
    d2d->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), ring.get()), nullptr);
    for (int i = 9; i >= 1; --i)
        c.Fill(D2D1::RectF(face.left - float(i), face.top - float(i) + 2, face.right + float(i), face.bottom + float(i) + 2), kWidgetRadius + float(i),
               D2D1::ColorF(0, 0, 0, 0.018f));
    d2d->PopLayer();
}

void DesktopWidgets::Paint(Widget& w, Canvas& c)
{
    RECT rect = w.surface->Rect();
    const float width = float(rect.right - rect.left) / c.scale, height = float(rect.bottom - rect.top) / c.scale;
    const auto bounds = D2D1::RectF(0, 0, width, height);
    // Preserve each size's layout. Only its viewport grows or shrinks, so a
    // large face is revealed without squeezing its columns into a small one.
    c.Clip(bounds);
    if (c.panel && material_ != WidgetMaterial::Color)
        c.Fill(bounds, 0, c.theme.panel);
    GetLocalTime(&world_.now);
    if (w.kind == WidgetKind::Reminders)
        w.content.scroll = std::clamp(w.content.scroll, 0.f, ReminderScrollMax(w.content, w.size));
    PaintWidget(c, w.kind, w.size, material_, w.content, world_, w.hits);
    if ((w.hover || w.resizing) && !w.dragging && Resizable(w.kind)) {
        // Both corners use the same quiet arc; either can be dragged to resize.
        const float r = kWidgetRadius, h = height;
        c.Arc(r, h - r, r - 6, 100.f, 170.f, c.theme.subtext, 3.5f);
        c.Arc(width - r, h - r, r - 6, 10.f, 80.f, c.theme.subtext, 3.5f);
    }
    c.Unclip();
    c.MaskRounded(bounds, kWidgetRadius);
    if (material_ == WidgetMaterial::Color)
        PaintShadow(c, bounds);
    if (w.content.editing)
        PlaceImeWindow(w);
}

} // namespace atrium
