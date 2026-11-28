#include "app_runtime.h"

#include "bar.h"
#include "bar_edge.h"
#include "desktop_helpers.h"
#include "extras_host.h"
#include "guard_process.h"
#include "host_messages.h"
#include "localization.h"
#include "log.h"
#include "render/capture.h"
#include "render/shutdown_wait.h"
#include "shell/desktop_agent_client.h"
#include "shell/displays.h"
#include "shell/feishu_source.h"
#include "shell/games.h"
#include "shell/ime_status.h"
#include "shell/messengers.h"
#include "shell/notification_listener.h"
#include "shell/system_chrome.h"
#include "shell/tray_model.h"
#include "shell/update_checker.h"
#include "shell/wechat_source.h"
#include "shell/win_key.h"
#include "ui/calendar_panel.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/genie_minimize.h"
#include "ui/glass_menu.h"
#include "ui/ime_bubble.h"
#include "ui/notifications.h"
#include "ui/popup_theme.h"
#include "ui/stage_manager.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"
#include "ui/window_preview.h"
#include "win32_path.h"

#include <algorithm>

namespace atrium {

namespace {

void ApplyImeSettings(const Settings& s)
{
    g_app.imeOn = s.imeStatus;
    g_app.imeBubble = s.imeBubble;
    if (!s.imeBubble && g_app.ime)
        g_app.ime->Cancel();
    if (g_app.imeOn && !g_app.imeWatcher) {
        g_app.imeWatcher = std::make_unique<ImeWatcher>(g_app.host, WM_IME_STATE);
        g_app.imeWatch.Reset();
    } else if (!g_app.imeOn && g_app.imeWatcher) {
        g_app.imeWatcher.reset();
        g_app.imeWatch.Reset();
        ForBars([](Bar& bar) { bar.SetIme(L""); });
        if (g_app.ime)
            g_app.ime->Cancel(); // a bubble already on screen goes with the setting, not when its life ends
    }
}

void ApplyNotificationSources(const Settings& s, bool badgesChanged)
{
    const bool toasts = s.notifyListener || s.notifyQQ || s.notifyWeChat || s.notifyFeishu || s.notifyWeCom || s.notifyDingTalk;
    if (s.bar && toasts && !g_app.listener)
        g_app.listener = std::make_unique<NotificationListener>(g_app.host, WM_NOTICE);
    else if (!(s.bar && toasts) && g_app.listener)
        g_app.listener.reset();
    if (g_app.listener)
        g_app.listener->SetAlways(s.notifyAlways);
    const bool needWeChat = s.bar && (s.notifyWeChat || s.barBadges);
    if (needWeChat && !g_app.wechat)
        g_app.wechat = std::make_unique<WeChatSource>(g_app.host, WM_NOTICE, WM_BADGES);
    else if (!needWeChat && g_app.wechat)
        g_app.wechat.reset();
    if (g_app.wechat) {
        g_app.wechat->SetBlockOfficial(s.wechatBlockOfficial);
        g_app.wechat->SetPostCards(s.notifyWeChat);
    }
    if (badgesChanged && g_app.tasks && g_app.bar) {
        const std::vector<TaskItem> items = BarItems();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
    }
    if (s.bar && s.notifyFeishu && !g_app.feishu)
        g_app.feishu = std::make_unique<FeishuSource>(g_app.host, WM_NOTICE);
    else if (!(s.bar && s.notifyFeishu) && g_app.feishu)
        g_app.feishu.reset();
}

void ApplyDesktopAndSystemSettings(const Settings& s)
{
    const HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), s.desktopDisplay);
    g_app.widgets->SetMonitor(desktop);
    g_app.widgets->SetOnRight(s.stageManager ? !s.stageOnRight : s.widgetsOnRight);
    g_app.stage->SetMonitor(desktop), g_app.stage->SetOnRight(s.stageOnRight), g_app.stage->SetAutoHide(s.stageAutoHide);
    g_app.stage->SetMinimizeToStrip(s.stageMinimize), g_app.stage->SetDragMerge(s.stageDragMerge);
    g_app.stage->SetExcluded(s.stageExcluded), SetUserGames(s.stageExcluded);
    g_app.stage->SetCloseButton(s.stageCloseButton), g_app.stage->SetScrollWrap(s.stageScrollWrap);
    g_app.stage->SetAvoid(s.stageAvoid), g_app.stage->SetEnabled(s.stageManager);
    g_app.genie->SetEnabled(s.genieMinimize && s.bar);
    g_app.preview->SetPeek(s.windowPeek);
    g_app.control->SetStage(s.stageManager, s.stageOnRight);
    g_app.control->SetModules(s.controlModules);
    g_app.widgets->SetMaterial(s.widgetMaterial == L"dusk"  ? WidgetMaterial::Dusk :
                               s.widgetMaterial == L"color" ? WidgetMaterial::Color :
                                                              WidgetMaterial::Glass);
    g_app.widgets->SetEnabled(s.widgets);
    g_app.widgetsOn = s.widgets;

    const bool keyStart = s.bar && s.winKeyStart && !LowIntegrity(), keyNumbers = s.bar && s.winNumbers && !LowIntegrity();
    if (keyStart != g_app.winKeyStart || keyNumbers != g_app.winNumbers || bool(g_app.winKey) != (keyStart || keyNumbers)) {
        g_app.winKey.reset();
        if (keyStart || keyNumbers) {
            g_app.winKey = std::make_unique<WinKey>(g_app.host, WM_WIN_KEY, keyStart, keyNumbers);
            if (g_app.start)
                g_app.winKey->SetStartMenuOpen(g_app.start->Visible());
        }
        g_app.winKeyStart = keyStart;
        g_app.winNumbers = keyNumbers;
    }
    ApplyTrayHostSetting(g_app.tray.get(), s.trayHost);
    if (s.remoteVisible != RemoteVisible()) {
        SetRemoteVisible(s.remoteVisible);
        RefreshCaptureExclusion();
        Log(L"core: remote-control mode %s", s.remoteVisible ? L"on: Atrium shows in every capture, the glass shows the wallpaper" : L"off");
    }
    if (g_app.chrome) {
        g_app.chrome->SetTaskbarHidden(s.bar && s.hideTaskbar);
        g_app.chrome->SetBannersHidden(s.bar && s.hideBanners);
        if ((s.bar && (s.hideTaskbar || s.hideBanners)) || s.trayHost)
            EnsureGuard(g_app.guard);
    }
}

Messenger ItemMessenger(const TaskItem& t, const PinnedApp& pin, std::wstring& exeLower)
{
    exeLower = FoldCase(pin.path);
    if (!exeLower.empty()) {
        if (const Messenger m = MessengerOfProgram(exeLower); m != Messenger::None)
            return m;
    }
    const std::wstring aumid = FoldCase(!pin.aumid.empty() ? pin.aumid : t.key);
    if (!aumid.empty()) {
        if (const Messenger m = MessengerOfNotice(L"windows", t.name, aumid); m != Messenger::None)
            return m;
        if (aumid.find(L"tencentqq") != std::wstring::npos || aumid.find(L".qq") != std::wstring::npos || aumid.find(L"qqdesktop") != std::wstring::npos ||
            aumid.find(L"qqnt") != std::wstring::npos)
            return Messenger::QQ;
    }
    return Messenger::None;
}

} // namespace

// Applies settings to the bar and the task model.
void ApplySettings(const Settings& s)
{
    if (SetLanguage(s.language) && g_app.widgets)
        g_app.widgets->ReloadLanguage();
    ApplyImeSettings(s);
    const bool badgesChanged = g_app.displaySettings.barBadges != s.barBadges;
    g_app.desktopDisplay = s.desktopDisplay;
    g_app.displaySettings = s;
    Theme::SetGlass(s.tint, s.transparency);
    const float radius = float(std::clamp(s.cornerRadius, 8.0, std::max(8.0, s.height / 2)));
    g_app.start->SetRadius(radius), g_app.trayPanel->SetRadius(radius), g_app.extras->Apply(s, radius);
    g_app.control->SetRadius(radius), g_app.control->SetBarHeight(float(s.height));
    g_app.calendar->SetRadius(radius), g_app.calendar->SetBarHeight(float(s.height));
    g_app.start->SetBarHeight(float(s.height)), g_app.start->SetShowTray(s.startTray), g_app.start->SetShowStatus(s.startStatus);
    g_app.start->SetCentered(s.startCentered), g_app.start->SetPreferredSize(s.startSize), g_app.start->SetPreferredIcon(s.startIcon);
    g_app.notes->SetRadius(radius), g_app.notes->SetOptions(s.notifyTimeout, s.notifyHideBody, s.notifyPlacement, s.notifyFullscreen, s.notifyAlways);
    ApplyNotificationSources(s, badgesChanged);
    g_app.tasks->SetPins(s.pins.value_or(std::vector<PinnedApp>{}));
    g_app.tasks->SetShowRunning(s.showRunning);
    ApplyDesktopAndSystemSettings(s);
    SyncDisplayBars(s);
    if (g_app.barOn != s.bar) {
        g_app.barOn = s.bar;
        if (!s.bar)
            CloseBarPopups();
        ForBars([&](Bar& bar) { bar.SetVisible(s.bar); });
        Log(L"modules: bar %s", s.bar ? L"on" : L"off");
    }
}

// A popup of the bar's is open: the notification cards keep clear of it.
bool BarPopupOpen()
{
    return g_app.start->Visible() || g_app.trayPanel->Visible() || g_app.control->Visible() || g_app.calendar->Visible() || g_app.extras->AnyPopupVisible();
}

void CloseBarPopups()
{
    g_app.extras->ClosePopups();
    g_app.start->Close(false);
    g_app.trayPanel->Close();
    g_app.control->Close();
    g_app.calendar->Close();
    g_app.menu->Close();
    g_app.preview->Close();
    if (g_app.notes->CenterVisible())
        g_app.notes->ToggleCenter();
}

void ConfigureBar(Bar& bar)
{
    bar.onInteract = [&bar] {
        if (g_app.activeBar == &bar)
            return;
        CloseBarPopups();
        g_app.activeBar = &bar;
        g_app.switchedBarAt = GetTickCount64();
        g_app.notes->PanelsChanged();
    };
    bar.iconProvider = [](const std::wstring& key, int px) { return g_app.tasks->Icon(key, px); };
    bar.onActivate = [&bar](const std::wstring& key) { g_app.tasks->Activate(key, bar.TaskMonitor()); };
    bar.onLaunch = [](const std::wstring& key) { g_app.tasks->Launch(key); };
    bar.onItemMenu = ShowItemMenu;
    bar.onClockClick = ToggleCalendar;
    bar.onReorder = Reorder;
    bar.onOverflow = ShowOverflow;
    bar.onStartClick = ToggleStart;
    bar.onStartMenu = ShowStartMenu;
    bar.onNotifyClick = [](RECT) { g_app.notes->ToggleCenter(); };
    bar.onTrayClick = ToggleTray;
    bar.onDesktopClick = [] { SendWinShortcut('D'); }; // the system's Show Desktop: a second click brings the windows back
    bar.onStatusClick = ToggleControl;
    g_app.extras->ConfigureBar(bar);
    bar.onImeClick = [] {
        if (g_app.imeWatcher)
            g_app.imeWatcher->Toggle();
    };
    if (!g_app.imeWatch.State().Empty())
        bar.SetIme(ImeGlyph(g_app.imeWatch.State()));
    bar.keepShown = [&bar] { return g_app.activeBar == &bar && (BarPopupOpen() || g_app.notes->CenterVisible() || g_app.menu->Visible()); };
    bar.onStatusWheel = [](int notches) {
        AdjustVolume(2 * notches);
        UpdateStatus();
    };
    bar.onFileDragHover = [](const std::wstring& key) { g_app.tasks->Show(key); };
    bar.canOpenFiles = [](const std::wstring& key) { return g_app.tasks->CanOpenFiles(key); };
    bar.onDropFiles = [](const std::wstring& key, const std::vector<std::wstring>& files) { g_app.tasks->OpenFiles(key, files); };
    bar.onContextMenu = ShowMenu;
    bar.onPreview = [&bar](const std::wstring& key, const std::vector<HWND>& windows, POINT anchor) {
        g_app.preview->Show(key, windows, anchor, bar.BarRect(), g_app.tasks->Icon(key, 64));
    };
    bar.onPreviewLeave = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Leave();
    };
    bar.onPreviewClose = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Close();
    };
    bar.previewOpen = [&bar] { return g_app.activeBar == &bar && g_app.preview->Visible(); };
    bar.onMonitorChanged = [&bar] {
        if (g_app.activeBar == &bar) {
            CloseBarPopups();
            g_app.notes->PanelsChanged();
        }
    };
    bar.onItemsChanged = [&bar] {
        if (g_app.activeBar == &bar)
            g_app.preview->Refresh(bar.Items());
    };
    bar.onCloseRequest = [] { PostMessageW(g_app.host, WM_CLOSE, 0, 0); };
    bar.onDropLayoutChanged = [] { SyncDesktopDropBars(); };
}

// The bar's items, lit for attention also where an app's tray icon flashes (QQ and WeChat flash
// theirs for a new message while they sit in the tray, with no window to flash).
std::vector<TaskItem> BarItems()
{
    std::vector<TaskItem> items = g_app.tasks->Snapshot();
    const bool badgesOn = g_app.displaySettings.barBadges;
    if (!badgesOn) {
        for (TaskItem& t : items)
            t.badge = 0;
    }
    const std::vector<std::wstring> flashing = g_app.tray ? g_app.tray->Flashing() : std::vector<std::wstring>{};
    const int wechatUnread = (badgesOn && g_app.wechat) ? g_app.wechat->Unread() : 0;
    if (flashing.empty() && wechatUnread <= 0)
        return items;
    for (TaskItem& t : items) {
        const bool needFlash = !t.attention && !flashing.empty();
        const bool needWeChat = wechatUnread > 0;
        if (!needFlash && !needWeChat)
            continue;
        const PinnedApp pin = g_app.tasks->PinFor(t.key);
        std::wstring exe;
        const Messenger messenger = ItemMessenger(t, pin, exe);
        if (needFlash) {
            if (!exe.empty())
                t.attention = std::ranges::find(flashing, exe) != flashing.end();
            else if (messenger != Messenger::None)
                t.attention = std::ranges::any_of(flashing, [messenger](const std::wstring& f) { return MessengerOfProgram(f) == messenger; });
        }
        if (needWeChat && messenger == Messenger::WeChat)
            t.badge = std::max(t.badge, wechatUnread);
    }
    return items;
}

void SyncDisplayBars(const Settings& settings, bool refresh, bool workAreaOnly)
{
    const auto displays = ConnectedDisplays();
    // Keep existing renderers when changing appearance; only retire disconnected or unused bars.
    const bool all = settings.bar && settings.dockDisplayMode == L"all";
    std::vector<std::unique_ptr<Bar>> retired;
    bool closeActive = false;
    std::erase_if(g_app.secondaryBars, [&](auto& bar) {
        if (all && std::ranges::any_of(displays, [&](const DisplayInfo& d) { return !d.primary && d.id == bar->DisplayOverride(); }))
            return false;
        if (g_app.activeBar == bar.get())
            g_app.activeBar = g_app.bar.get(), closeActive = true;
        retired.push_back(std::move(bar));
        return true;
    });
    if (closeActive)
        CloseBarPopups();
    for (const auto& bar : retired)
        g_app.extras->OnBarRetiring(bar.get());
    std::ranges::for_each(retired, [](const auto& bar) { bar->SetVisible(false), bar->DetachInput(), bar->SignalStopRendering(); });
    const size_t beforeSlow = g_app.drainingBars.size();
    CollectSlowWithinDeadline(retired, g_app.drainingBars, 200, [](const auto& bar, DWORD ms) { return bar->WaitStopRendering(ms); });
    if (g_app.drainingBars.size() > beforeSlow)
        Log(L"core: %zu secondary display renderer(s) slow during removal; draining", g_app.drainingBars.size() - beforeSlow);
    if (all)
        for (const DisplayInfo& display : displays) {
            if (display.primary || std::ranges::any_of(g_app.secondaryBars, [&](const auto& bar) { return bar->DisplayOverride() == display.id; }))
                continue;
            auto bar = std::make_unique<Bar>(g_app.instance, g_app.capturable, display.id);
            ConfigureBar(*bar);
            bar->Apply(settings);
            bar->SetStatus(g_app.lastStatus);
            bar->SetUnread(g_app.notes->Unread()), bar->SetDoNotDisturb(g_app.notes->DoNotDisturb());
            g_app.secondaryBars.push_back(std::move(bar));
        }
    const auto items = BarItems();
    ForBars([&](Bar& bar) {
        if (refresh)
            bar.RefreshDisplays(workAreaOnly);
        bar.Apply(settings);
        bar.SetItems(items);
        bar.SetVisible(settings.bar);
    });
    const BarEdge edge = ParseBarEdge(settings.barEdge);
    g_app.widgets->SetBarEdge(edge);
    g_app.stage->SetBarEdge(edge);
    g_app.notes->PanelsChanged();
    SyncDesktopDropBars();
}

void SyncDesktopDropBars()
{
    if (!g_app.desktopAgent || !g_app.bar || g_app.desktopAgentStopping.load())
        return;
    std::vector<AgentBarRect> bars;
    ForBars([&](Bar& bar) {
        const RECT r = bar.DropBarRect();
        if (!IsRectEmpty(&r))
            bars.push_back({bar.DropBarId(), r, true});
    });
    g_app.desktopAgent->SyncDropBars(bars);
}

void UpdateDesktopMonitor(bool notify)
{
    const HMONITOR desktop = ResolveDisplay(ConnectedDisplays(), g_app.desktopDisplay);
    g_app.widgets->SetMonitor(desktop, true);
    g_app.stage->SetMonitor(desktop, true);
    if (notify)
        g_app.notes->PanelsChanged();
}

} // namespace atrium
