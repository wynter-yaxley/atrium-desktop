#include "app_runtime.h"

#include "app_handlers.h"
#include "app_launch.h"
#include "bar.h"
#include "clock.h"
#include "crash_dump.h"
#include "desktop_helpers.h"
#include "extras_host.h"
#include "host_messages.h"
#include "lifecycle_server.h"
#include "log.h"
#include "render/capture.h"
#include "render/shutdown_wait.h"
#include "resource.h"
#include "settings_server.h"
#include "shell/app_catalog.h"
#include "shell/feishu_source.h"
#include "shell/jump_list.h"
#include "shell/notification_listener.h"
#include "shell/shell_icon.h"
#include "shell/shell_launch.h"
#include "shell/system_chrome.h"
#include "shell/tray_model.h"
#include "shell/update_checker.h"
#include "shell/usb_volume.h"
#include "shell/wechat_source.h"
#include "shell/win_key.h"
#include "ui/calendar_panel.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/genie_minimize.h"
#include "ui/glass_menu.h"
#include "ui/ime_bubble.h"
#include "ui/notifications.h"
#include "ui/stage_manager.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"
#include "ui/window_preview.h"
#include "win32_path.h"

#include <shellapi.h>

#include <algorithm>

namespace atrium {

namespace {

// Atrium's tray panel and control centre open above their entries on the
// bar; a click on the entry while one is open closes it (and must not reopen
// it: the press that deactivated it is the same click).
template<class Panel, class OpenFn> void TogglePopup(Panel& panel, OpenFn open)
{
    if (panel.Visible()) {
        panel.Close();
        return;
    }
    if (GetTickCount64() - panel.LastClosed() < 400 && panel.LastClosed() > g_app.switchedBarAt)
        return;
    open();
    g_app.notes->PanelsChanged();
}

std::optional<RECT> GenieIconTarget(HWND window)
{
    std::optional<RECT> icon;
    ForBars([&](Bar& bar) {
        if (icon || !bar.Visible())
            return;
        for (const TaskItem& item : bar.Items())
            if (std::ranges::find(item.windows, window) != item.windows.end()) {
                if (bar.Seen()) {
                    icon = bar.EntryRect(item.key);
                } else {
                    const RECT r = bar.BarRect();
                    const LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                    icon = RECT{cx - 1, cy - 1, cx + 1, cy + 1};
                }
                return;
            }
    });
    return icon;
}

void OnTasksChanged()
{
    const double t0 = ClockMs();
    WithdrawForegroundMessenger(g_app.alerts, g_app.notes.get());
    const std::vector<TaskItem> all = g_app.tasks->Snapshot(true);
    if (g_app.stage)
        g_app.stage->Update(all);
    if (g_app.trayPanel)
        g_app.trayPanel->SetAttention(all);
    const double t1 = ClockMs();
    const auto items = BarItems();
    const double t2 = ClockMs();
    ForBars([&](Bar& bar) { bar.SetItems(items); });
    if (g_app.preview)
        g_app.preview->Refresh(ActiveBar().Items());
    const double t3 = ClockMs();
    if (g_app.start)
        g_app.start->Refresh();
    const double t4 = ClockMs();
    if (t4 - t0 > 20)
        Log(L"tasks: changes took %.0f ms (stage %.0f, preview %.0f, bar %.0f, start %.0f)", t4 - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
}

// Launchpad, the now-playing entry and the folder stack: theirs the bars' extra entries and popups.
void MakeExtras(HINSTANCE instance)
{
    g_app.extras = std::make_unique<ExtrasHost>(instance, *g_app.catalog, [](const std::function<void(Bar&)>& each) { ForBars(each); });
    g_app.extras->onPanelsChanged = [] { g_app.notes->PanelsChanged(); };
    g_app.extras->onOpenSettings = [](const wchar_t* page) { OpenSettings(page); };
    g_app.extras->SetAppMenu(ShowCatalogMenu, g_app.menu.get());
}

void BindNoteAndWidgetCallbacks()
{
    g_app.widgets->onStatusRequest = UpdateStatus;
    g_app.widgets->menu = g_app.menu.get();
    g_app.widgets->onAddWidgets = [] { OpenSettings(L"widgets"); };
    g_app.widgets->onCalendar = [] {
        if (!g_app.calendar->Visible())
            ToggleCalendar();
    };
    g_app.notes->bellRect = [] { return ActiveBar().EntryRect(L"#notify"); };
    g_app.notes->barRect = [] { return ActiveBar().BarRect(); };
    g_app.notes->onUnread = [](int n) { ForBars([&](Bar& bar) { bar.SetUnread(n); }); };
    g_app.notes->onActivate = [](const Notice& n) { return RunNoticeAction(NoticeEnv(), n); };
    g_app.notes->quietIsOurs = g_app.control->quietIsOurs = [] { return g_app.chrome && g_app.chrome->QuietIsOurs(); };
    g_app.control->onDnd = [](bool on) { g_app.notes->SetDoNotDisturb(on), ForBars([on](Bar& bar) { bar.SetDoNotDisturb(on); }); };
    g_app.notes->held = BarPopupOpen;
}

void BindStartMenuCallbacks(Settings& settings)
{
    g_app.start->onOpenChanged = [](bool open) {
        if (g_app.winKey)
            g_app.winKey->SetStartMenuOpen(open);
    };
    g_app.start->onSize = [](const std::wstring& name) { g_app.server->Modify([&](Settings& s) { s.startSize = name; }); };
    g_app.start->onIcon = [](const std::wstring& name) { g_app.server->Modify([&](Settings& s) { s.startIcon = name; }); };
    if (settings.startSize.empty())
        g_app.start->onSize(g_app.start->SizeName());
    if (settings.startIcon.empty())
        g_app.start->onIcon(g_app.start->IconSizeName());
    g_app.start->onPin = [](const PinnedApp& pin) {
        if (g_app.server)
            g_app.server->Modify([&](Settings& s) {
                if (auto pins = s.pins.value_or(std::vector<PinnedApp>{}); AddUniquePin(pins, pin))
                    s.pins = std::move(pins);
            });
    };
}

void AbortStuckShutdown(HANDLE mutex, const wchar_t* reason)
{
    g_app.chrome.reset();
    Log(L"%s", reason);
    ReleaseMutex(mutex);
    ShutdownLog();
    TerminateProcess(GetCurrentProcess(), 0);
}

} // namespace

AppState g_app;

AppState::AppState() = default;
AppState::~AppState() = default;

Bar& ActiveBar() { return *(g_app.activeBar ? g_app.activeBar : g_app.bar.get()); }

Bar* WinKeyBar() { return g_app.barOn.value_or(true) ? (g_app.activeBar ? g_app.activeBar : g_app.bar.get()) : nullptr; }

void AddTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = static_cast<HICON>(
        LoadImageW(g_app.instance, MAKEINTRESOURCEW(IDI_ATRIUM), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(nid.szTip, L"Atrium");
    g_app.trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    const DWORD err = g_app.trayAdded ? 0 : GetLastError();
    DestroyIcon(nid.hIcon);
    static bool failLogged = false;
    static DWORD lastErr = 0;
    static HWND lastFirst = nullptr;
    if (!g_app.trayAdded) {
        HWND first = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!failLogged || err != lastErr || first != lastFirst) {
            failLogged = true;
            lastErr = err;
            lastFirst = first;
            const bool hosting = g_app.tray && g_app.tray->Hosting();
            const bool yielded = g_app.tray && g_app.tray->HostYielded();
            const auto reason = g_app.tray ? g_app.tray->HostYieldReason() : TrayHostYieldReason::None;
            Log(L"tray: could not add Atrium's notification icon (err=%lu first=0x%p host=%d yielded=%d reason=%s); will retry", err, static_cast<void*>(first),
                hosting ? 1 : 0, yielded ? 1 : 0, YieldReasonName(reason));
            LogShellTrayWindows();
        }
        return;
    }
    failLogged = false;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &nid))
        Log(L"tray: could not set notification icon version");
    Log(L"tray: Atrium notification icon added");
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_app.host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_app.trayAdded = false;
}

// Win+<key> (or Win+Ctrl+<key>), a public system shortcut (D11).
// UIA and popup discovery run on the tray worker, including explicit fallback.
void OpenTray()
{
    if (g_app.tray)
        g_app.tray->OpenSystemTray();
}

void OpenSystem(const std::wstring& what)
{
    if (what == L"network" || what == L"volume" || what == L"battery") {
        g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry %s (control centre)", what.c_str());
        return;
    }
    if (what == L"lock") {
        LockWorkStation();
        return;
    }
    if (what == L"tray") {
        g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground());
        g_app.notes->PanelsChanged();
        Log(L"start: system entry tray (tray panel)");
        return;
    }
    if (what == L"account") {
        LaunchRequest req;
        req.target = L"ms-settings:yourinfo";
        req.kind = LaunchTargetKind::Uri;
        req.privilege = LaunchPrivilege::ShellDefault;
        req.show = SW_SHOWNORMAL;
        ShellLaunchAsync(std::move(req));
    }
    Log(L"start: system entry %s", what.c_str());
}

void UpdateStatus()
{
    if (g_app.status)
        g_app.status->Refresh();
}

void ToggleTray(RECT)
{
    TogglePopup(*g_app.trayPanel, [] { g_app.trayPanel->Open(ActiveBar().EntryRect(L"#tray"), ActiveBar().BarRect(), ActiveBar().PreviousForeground()); });
}

void ToggleControl(RECT)
{
    TogglePopup(*g_app.control, [] { g_app.control->Open(ActiveBar().EntryRect(L"#status"), ActiveBar().BarRect()); });
}

void ToggleCalendar()
{
    TogglePopup(*g_app.calendar, [] { g_app.calendar->Open(ActiveBar().EntryRect(L"#clock"), ActiveBar().BarRect()); });
}

void ToggleStart(RECT)
{
    if (g_app.start->Visible()) {
        g_app.start->Close(false);
        return;
    }
    // A click on the logo while the menu is open first deactivates (closes) it;
    // that click must not open it again.
    if (GetTickCount64() - g_app.start->LastClosed() < 400 && g_app.start->LastClosed() > g_app.switchedBarAt)
        return;
    g_app.start->Open(ActiveBar().BarRect(), ActiveBar().EntryRect(L"#start"), ActiveBar().PreviousForeground());
    g_app.notes->PanelsChanged();
}

// The bar module on or off (its menu, the tray icon's). Off, its popups close and Windows' own
// taskbar and banners take over again (ApplySettings).
void SetBarModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.bar = on; });
}

void SetWidgetsModule(bool on)
{
    g_app.server->Modify([on](Settings& s) { s.widgets = on; });
}

// Stage Manager from the bar (its menu, the control centre); the widgets go to the other side.
void SetStageManager(bool on)
{
    g_app.server->Modify([on](Settings& s) {
        s.stageManager = on;
        if (on)
            s.widgetsOnRight = !s.stageOnRight;
    });
}

AppMenuEnv MenuEnv()
{
    return {
        g_app.host,
        g_app.menu.get(),
        g_app.tasks.get(),
        g_app.server.get(),
        g_app.stage.get(),
        []() -> Bar& { return ActiveBar(); },
        [](const wchar_t* page) { OpenSettings(page); },
        [] { return g_app.barOn.value_or(true); },
        [] { return g_app.widgetsOn; },
        SetBarModule,
        SetWidgetsModule,
        SetStageManager,
    };
}

NoticeActionEnv NoticeEnv()
{
    return {
        g_app.tray.get(),
        g_app.tasks.get(),
        g_app.wechat.get(),
        [](const wchar_t* page) { OpenSettings(page); },
    };
}

void ShowMenu(POINT pt) { ShowBarContextMenu(MenuEnv(), pt); }

void ShowStartMenu(POINT pt) { ShowStartContextMenu(MenuEnv(), pt); }

void ShowItemMenu(const std::wstring& key, POINT pt) { ShowTaskItemMenu(MenuEnv(), key, pt); }

void ShowCatalogMenu(const CatalogApp& app, POINT pt, std::function<void(bool)> done, std::vector<AppMenuExtra> extras)
{
    ShowCatalogAppMenu(MenuEnv(), app, pt, std::move(done), std::move(extras));
}

void ShowOverflow(const std::vector<std::wstring>& keys, RECT anchor) { ShowOverflowAppsMenu(MenuEnv(), keys, anchor); }

void Reorder(const std::wstring& key, int to) { ReorderBarTask(MenuEnv(), key, to); }

void InitializeSubsystems(HINSTANCE instance, Settings& settings)
{
    StartShellLaunch();
    g_app.bar = std::make_unique<Bar>(instance, g_app.capturable);
    g_app.activeBar = g_app.bar.get();
    Surface::SetGlassHost(g_app.bar.get());
    Surface::SetCapturable(g_app.capturable);
    g_app.menu = std::make_unique<GlassMenu>(instance);
    StartJumpListIndex(settings.windowsContextMenu);
    g_app.tasks = std::make_unique<TaskModel>(g_app.host, OnTasksChanged, [](const std::wstring& exe) { return g_app.tray->OpenExisting(exe); });
    g_app.tasks->SetOnFlash([](HWND window) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        MessengerFlashed(ProgramOf(window), pid);
    });
    g_app.catalog = std::make_unique<AppCatalog>(g_app.host, WM_CATALOG);
    g_app.start = std::make_unique<StartMenu>(instance, *g_app.tasks, *g_app.catalog);
    MakeExtras(instance);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_app.catalog->PrefetchStartIcons(StartMenu::TileIconPx(PopupScale(mi.rcWork)));
    g_app.start->onSystem = OpenSystem, g_app.start->SetPowerNotify(g_app.host, WM_NOTICE);
    g_app.start->menu = g_app.menu.get();
    g_app.notes = std::make_unique<Notifications>(instance);
    g_app.tray = std::make_unique<TrayModel>(g_app.host, WM_TRAY_ICONS, [] {
        std::vector<PinnedApp> names;
        for (const auto& app : g_app.catalog->Apps())
            names.push_back(AppCatalog::PinFor(app));
        return names;
    });
    g_app.trayPanel = std::make_unique<TrayPanel>(instance, *g_app.tray);
    g_app.trayPanel->onSystemTray = OpenTray;
    g_app.trayPanel->SetAttention(g_app.tasks->Snapshot(true));
    g_app.control = std::make_unique<ControlCenter>(instance);
    g_app.control->onChanged = UpdateStatus, g_app.control->onStage = SetStageManager;
    g_app.start->onStatusRequest = UpdateStatus;
    g_app.calendar = std::make_unique<CalendarPanel>(instance);
    g_app.widgets = std::make_unique<DesktopWidgets>(instance);
    g_app.widgets->ReloadLanguage();
    g_app.stage = std::make_unique<StageManager>(instance, *g_app.tasks);
    g_app.stage->onCoverSoon = [](HMONITOR monitor) {
        ForBars([&](Bar& bar) {
            if (bar.Monitor() == monitor)
                bar.ExpectCover(1200);
        });
    };
    g_app.genie = std::make_unique<GenieMinimize>(instance, GenieIconTarget, [](HWND window) { return g_app.stage && g_app.stage->OwnsMinimize(window); });
    TaskModel::SetWindowSteps([](HWND w, std::function<void()> step) { return g_app.genie && g_app.genie->Minimize(w, std::move(step)); },
                              [](HWND w, std::function<void()> step) { return g_app.genie && g_app.genie->Restore(w, std::move(step)); });
    g_app.status = std::make_unique<SystemStatusReader>(g_app.host, WM_STATUS, WM_VOLUME);
    g_app.usb = std::make_unique<UsbVolumeMonitor>([host = g_app.host] { PostMessageW(host, WM_USB, 0, 0); });
    BindNoteAndWidgetCallbacks();
    g_app.preview = std::make_unique<WindowPreview>(instance);
    g_app.ime = std::make_unique<ImeBubble>(instance);
    ConfigureBar(*g_app.bar);
    g_app.displaysReady = true;
    ApplySettings(settings);
    g_app.bar->SetItems(BarItems());
    g_app.updates = std::make_unique<UpdateChecker>(g_app.host, WM_UPDATE_READY, settings.autoUpdate);
    g_app.server = std::make_unique<SettingsServer>(g_app.host, settings, std::wstring{}, std::wstring{}, [](bool on) {
        if (g_app.updates)
            g_app.updates->SetAutoCheck(on);
    });
    g_app.onRestartStandard = [] {
        if (!CanDeElevateToShell(CurrentPrivilegeMode()))
            return false;
        const std::wstring exe = ModulePath();
        if (exe.empty())
            return false;
        LaunchRequest req;
        req.target = exe;
        req.parameters = L"--replace" + DataDirectoryArgs();
        req.kind = LaunchTargetKind::Executable;
        req.privilege = LaunchPrivilege::RequireMedium;
        req.show = SW_SHOWNORMAL;
        return ShellLaunch(req, kDefaultShellLaunchTimeoutMs).outcome == LaunchOutcome::Succeeded;
    };
    g_app.lifecycle = std::make_unique<LifecycleServer>(g_app.host, std::wstring{},
                                                        LifecycleCallbacks{
                                                            .onOpenSettings =
                                                                [](const std::wstring& page) {
                                                                    if (page.empty() && g_app.host)
                                                                        PostMessageW(g_app.host, WM_OPEN_SETTINGS, 0, 0);
                                                                    else
                                                                        OpenSettingsQuiet(page.empty() ? nullptr : page.c_str());
                                                                },
                                                            .onQuit =
                                                                [] {
                                                                    if (g_app.host)
                                                                        PostMessageW(g_app.host, WM_CLOSE, 0, 0);
                                                                },
                                                            .onRestartStandard = [] { return g_app.onRestartStandard ? g_app.onRestartStandard() : false; },
                                                        });
    BindStartMenuCallbacks(settings);
}

void ShutdownApp(HANDLE mutex, std::thread& startupThread)
{
    g_app.displaysReady = false;
    if (g_app.chrome)
        g_app.chrome->RestoreForExit();
    g_app.stage->RestoreAll();
    g_app.stage->SignalSnapshotStop();
    if (g_app.genie)
        g_app.genie->SignalSnapshotStop();
    g_app.winKey.reset(), g_app.imeWatcher.reset(), g_app.status.reset();
    g_app.preview->Close();
    std::vector<Bar*> stopBars;
    ForBars([&](Bar& bar) { stopBars.push_back(&bar); });
    for (const auto& bar : g_app.drainingBars)
        stopBars.push_back(bar.get());
    const bool renderingStopped =
        StopWithinDeadline(stopBars, 3000, [](Bar* bar) { bar->SignalStopRendering(); }, [](Bar* bar, DWORD ms) { return bar->WaitStopRendering(ms); });
    if (!renderingStopped) {
        AbortStuckShutdown(mutex, L"core: exited (render thread stuck)");
        return;
    }
    g_app.lifecycle.reset(), g_app.server.reset(), g_app.updates.reset(), g_app.listener.reset();
    g_app.wechat.reset(), g_app.feishu.reset(), g_app.usb.reset();
    g_app.notes.reset(), g_app.widgets.reset(), g_app.calendar.reset(), g_app.control.reset();
    g_app.trayPanel.reset(), g_app.tray.reset(), g_app.start.reset();
    g_app.menu.reset(), g_app.preview.reset();
    TaskModel::SetWindowSteps({}, {});
    if (g_app.genie)
        g_app.genie->SetEnabled(false);
    const ULONGLONG snapshotDeadline = GetTickCount64() + 400;
    auto remainingSnapshotMs = [snapshotDeadline]() -> DWORD {
        const ULONGLONG now = GetTickCount64();
        return now < snapshotDeadline ? static_cast<DWORD>(snapshotDeadline - now) : 0;
    };
    if ((g_app.stage && !g_app.stage->WaitSnapshotStop(remainingSnapshotMs())) || (g_app.genie && !g_app.genie->WaitSnapshotStop(remainingSnapshotMs()))) {
        AbortStuckShutdown(mutex, L"core: exited (snapshot worker stuck)");
        return;
    }
    g_app.genie.reset();
    g_app.stage.reset(), g_app.ime.reset();
    Surface::SetGlassHost(nullptr);
    g_app.extras.reset(), g_app.catalog.reset();
    g_app.tasks.reset();
    g_app.drainingBars.clear(), g_app.secondaryBars.clear(), g_app.bar.reset();
    g_app.chrome.reset();
    StopShellLaunch();
    if (startupThread.joinable())
        startupThread.join();
    Log(L"core: exited");
    ShutdownLog();
    ShutdownCrashHandlers();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

} // namespace atrium
