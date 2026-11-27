#include "app_handlers.h"

#include "app_launch.h"
#include "app_runtime.h"
#include "bar.h"
#include "desktop_helpers.h"
#include "extras_host.h"
#include "guard_process.h"
#include "host_messages.h"
#include "localization.h"
#include "log.h"
#include "posted.h"
#include "settings_server.h"
#include "shell/displays.h"
#include "shell/feishu_source.h"
#include "shell/games.h"
#include "shell/ime_status.h"
#include "shell/notification_listener.h"
#include "shell/shell_taskbar.h"
#include "shell/system_chrome.h"
#include "shell/tray_model.h"
#include "shell/update_checker.h"
#include "shell/usb_volume.h"
#include "shell/wechat_source.h"
#include "shell/win_key.h"
#include "ui/control_center.h"
#include "ui/desktop_widgets.h"
#include "ui/ime_bubble.h"
#include "ui/notifications.h"
#include "ui/start_menu.h"
#include "ui/tray_panel.h"
#include "win32_path.h"

#include <dbt.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace atrium {

namespace {

// A settled change also shows in the bottom bubble, but only while the bar on
// that display is hidden — its input method entry already shows the glyph
// otherwise. A fullscreen window in front (a game, a video: it covers the
// screen, not just its work area) keeps the bubble quiet.
void ShowImeBubble(HWND foreground, const ImeStatus& status)
{
    if (!g_app.barOn.value_or(true) || !g_app.ime || !g_app.imeBubble)
        return; // the bar is off with Windows' taskbar back: it shows its own
    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    Bar* bar = g_app.bar && g_app.bar->Monitor() == monitor ? g_app.bar.get() : nullptr;
    if (!bar)
        for (const auto& secondary : g_app.secondaryBars)
            if (secondary->Monitor() == monitor) {
                bar = secondary.get();
                break;
            }
    if (bar && bar->ShownOnScreen())
        return;
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info))
        return;
    if (FullScreenInFront(monitor))
        return; // nothing is typed over a fullscreen window
    g_app.ime->Show(status, info.rcWork, int(info.rcMonitor.bottom));
    Log(L"ime: %s (layout %08lX) as the bubble", ImeGlyph(status).c_str(), status.layout);
}

void OnImeSample(ImeWatcher::Sample& sample)
{
    if (!g_app.imeOn || !g_app.imeWatcher)
        return;
    ImeStatus status = sample.status;
    status.caps = status.upper = g_app.imeWatcher->CapsLock();
    const bool changed = g_app.imeWatch.Update(sample.foreground, status);
    const ImeStatus& shown = g_app.imeWatch.State();
    if (shown.Empty())
        return;
    const std::wstring glyph = ImeGlyph(shown);
    ForBars([&](Bar& bar) { bar.SetIme(glyph); });
    if (changed) {
        Log(L"ime: %s (layout %08lX)", glyph.c_str(), shown.layout);
        ShowImeBubble(sample.foreground, shown);
    }
}

void PostMessengerCard(Messenger m, DWORD pid)
{
    if (!g_app.notes || !g_app.barOn.value_or(true) || !MessengerOn(m))
        return;
    if (HWND fg = GetForegroundWindow()) {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        if (pid != 0 ? (fgPid == pid) : (MessengerOfProgram(ProgramOf(fg)) == m))
            return;
    }
    if (m == Messenger::WeChat && g_app.wechat && g_app.wechat->ReadingPid(pid))
        return;
    Log(L"notify: %s flashed (pid %lu) with no card: a plain one", InfoOf(m).key, pid);
    const auto it = g_app.messengerSources.find({m, pid});
    const MessengerSourceMeta meta = it != g_app.messengerSources.end() ? it->second : MessengerSourceMeta{};
    g_app.notes->Post(BuildMessengerCardNotice(m, pid, meta));
}

void HandleTrayCallback(WPARAM w, LPARAM l)
{
    switch (LOWORD(l)) {
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: {
        POINT at{GET_X_LPARAM(w), GET_Y_LPARAM(w)}, cursor;
        if (GetCursorPos(&cursor) && (std::abs(at.x - cursor.x) > 48 || std::abs(at.y - cursor.y) > 48))
            at = cursor;
        ShowMenu(at);
        return;
    }
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_LBUTTONDBLCLK: OpenSettings(); return;
    default: break;
    }
}

void HandleWinKeyMessage(WPARAM w)
{
    Bar* bar = WinKeyBar();
    if (!bar)
        return;
    if (w) {
        HWND fg = GetForegroundWindow();
        ForegroundAttach attach(fg);
        AllowSetForegroundWindow(ASFW_ANY);
        bar->ActivateIndex(int(w) - 1);
    } else if (g_app.start->Visible()) {
        g_app.start->Close(false);
    } else if (const RECT r = bar->BarRect(); GameInFront(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST)) && bar->RevealOverGame()) {
        Log(L"winkey: the bar over a game");
    } else {
        g_app.activeBar = bar;
        HWND fg = GetForegroundWindow();
        {
            ForegroundAttach attach(fg);
            if (g_app.displaySettings.startWithBar)
                bar->Reveal();
            g_app.start->Open(bar->BarRect(), bar->EntryRect(L"#start"), fg);
        }
        g_app.notes->PanelsChanged();
        if (GetForegroundWindow() != g_app.start->Hwnd())
            Log(L"winkey: the start menu did not get the focus");
    }
}

void HandleNoticeMessage(LPARAM l)
{
    if (auto n = TakePosted<Notice>(l)) {
        if (!n->unread) {
            if (g_app.notes)
                g_app.notes->Post(*n);
            return;
        }
        const Messenger messenger = MessengerOfNotice(n->source, n->app, n->appId);
        bool on = n->source == L"atrium" ? g_app.barOn.value_or(true) :
                  n->source == L"wechat" ? bool(g_app.wechat) :
                  n->source == L"feishu" ? bool(g_app.feishu) :
                                           bool(g_app.listener);
        on = on && (messenger != Messenger::None ? MessengerOn(messenger) : n->source != L"windows" || g_app.displaySettings.notifyListener);
        if (g_app.notes && on) {
            if (n->source == L"wechat" && n->unread && (n->title != Tr(L"New message") || !n->body.empty()))
                g_app.notes->Post(Notice{.source = L"wechat", .title = Tr(L"New message"), .unread = false, .pid = n->pid});
            g_app.notes->Post(*n);
            if (n->unread)
                g_app.alerts.Noticed(messenger, GetTickCount64(), n->pid);
        }
    }
}

void HandleUpdateReadyMessage()
{
    if (g_app.updates && g_app.notes)
        g_app.updates->NotifyPrepared(
            [](std::optional<Notice> card, bool silent) { card ? g_app.notes->Post(std::move(*card), silent) : g_app.notes->ClearPreparedUpdate(); });
}

void HandleBadgesMessage()
{
    if (g_app.displaySettings.barBadges && g_app.tasks && g_app.bar) {
        const std::vector<TaskItem> items = BarItems();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
    }
}

void HandleStatusMessage(LPARAM l)
{
    auto status = TakePosted<SystemStatus>(l);
    status->brightness = g_app.brightness;
    g_app.lastStatus = *status;
    ForBars([&](Bar& bar) { bar.SetStatus(*status); });
    g_app.control->SetStatus(*status), g_app.start->SetStatus(*status), g_app.widgets->SetStatus(*status);
}

LRESULT HandleAddWidgetMessage(WPARAM w, LPARAM l)
{
    const bool valid = w <= WPARAM(WidgetKind::Network) && l >= 0 && l <= LPARAM(WidgetSize::Large);
    return valid && g_app.widgetsOn && g_app.widgets->Add(WidgetKind(w), WidgetSize(l)) ? 1 : 0;
}

void HandleCatalogMessage(bool listChanged)
{
    if (listChanged && g_app.tasks)
        g_app.tasks->OnCatalogLoaded();
    if (g_app.start)
        g_app.start->Refresh(listChanged);
    if (g_app.extras)
        g_app.extras->OnCatalogChanged(listChanged);
}

void HandleTrayIconsMessage(HWND h, WPARAM w)
{
    if (w == 6) {
        if (g_app.trayPanel)
            g_app.trayPanel->YieldTopmostForPointerFallback();
        return;
    }
    if (w == 4 || w == 1) {
        SetTimer(h, kTrayFocusTimer, 600, nullptr);
        return;
    }
    if (w == 5) {
        if (g_app.trayPanel)
            g_app.trayPanel->Close();
        return;
    }
    if (w == 2 || w == 3) {
        KillTimer(h, kTrayFocusTimer);
        if (g_app.trayPanel)
            g_app.trayPanel->Close();
        static ULONGLONG lastTrayNotice = 0;
        const auto now = GetTickCount64();
        if (g_app.notes && (!lastTrayNotice || now - lastTrayNotice >= 30000)) {
            lastTrayNotice = now;
            Notice notice;
            notice.source = L"atrium";
            notice.app = L"Atrium";
            notice.title = Tr(w == 2 ? L"Use the Windows notification area" : L"Windows notification area is unavailable");
            notice.body = Tr(w == 2 ? L"Select the icon in the Windows hidden icons panel." :
                                      L"Temporarily turn off Hide system taskbar, open the hidden icons panel, and try again.");
            g_app.notes->Post(std::move(notice));
        }
        return;
    }
    if (g_app.trayPanel)
        g_app.trayPanel->Refresh();
    static std::vector<FlashingTrayIcon> lastFlashing;
    std::vector<FlashingTrayIcon> flashing = g_app.tray ? g_app.tray->FlashingIcons() : std::vector<FlashingTrayIcon>{};
    std::ranges::sort(flashing, [](const auto& a, const auto& b) { return std::tie(a.hostKey, a.pid, a.exe) < std::tie(b.hostKey, b.pid, b.exe); });
    if (flashing != lastFlashing && g_app.tasks) {
        for (const auto& icon : flashing)
            if (std::ranges::none_of(lastFlashing, [&](const auto& p) { return p.hostKey == icon.hostKey && p.pid == icon.pid && p.exe == icon.exe; }))
                MessengerFlashed(icon.exe, icon.pid, icon.hostKey);
        lastFlashing = std::move(flashing);
        const auto items = BarItems();
        ForBars([&](Bar& bar) { bar.SetItems(items); });
    }
}

void TrySignalReadyEvent(HWND h)
{
    if (g_app.readyEvent.empty()) {
        KillTimer(h, kReadyTimer);
        return;
    }
    if (!g_app.server || !g_app.server->Ready())
        return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, g_app.readyEvent.c_str());
    if (event) {
        SetEvent(event);
        CloseHandle(event);
        Log(L"core: ready event signalled");
    } else {
        Log(L"core: OpenEventW failed for ready event (%lu)", GetLastError());
    }
    g_app.readyEvent.clear();
    KillTimer(h, kReadyTimer);
}

bool HandleDeviceBroadcast(WPARAM w, LPARAM l)
{
    if (!g_app.usb || (w != DBT_DEVICEARRIVAL && w != DBT_DEVICEREMOVEPENDING && w != DBT_DEVICEQUERYREMOVEFAILED && w != DBT_DEVICEREMOVECOMPLETE))
        return false;
    UsbVolumes::Event event;
    event.kind = UINT(w);
    if (const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(l); header && header->dbch_devicetype == DBT_DEVTYP_VOLUME) {
        event.unitmask = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(l)->dbcv_unitmask;
    } else if (!header || header->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE ||
               reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(l)->dbcc_classguid != kVolumeInterface) {
        return false;
    }
    g_app.usb->Changed(event);
    return true;
}

void HandleUsbMessage()
{
    if (!g_app.usb)
        return;
    const std::vector<RemovedVolume> removed = g_app.usb->TakeRemoved();
    if (removed.empty() || !g_app.notes || !g_app.barOn.value_or(true))
        return;
    std::vector<std::wstring> names;
    bool ejected = true;
    for (const auto& volume : removed) {
        names.push_back(VolumeName(volume.label, volume.letter));
        if (!volume.ejected)
            ejected = false;
    }
    Notice notice;
    notice.source = L"usb";
    notice.app = L"Atrium";
    const std::wstring list = JoinNames(names);
    if (ejected) {
        notice.title = Tr(L"Safe to remove hardware");
        notice.body = TrFormat(L"\"%s\" can now be safely unplugged.", list.c_str());
    } else {
        notice.title = Tr(L"USB device removed");
        notice.body = TrFormat(L"\"%s\" has been removed from the computer.", list.c_str());
    }
    g_app.notes->Post(std::move(notice));
}

bool HandleHostTimer(HWND h, WPARAM w)
{
    if (w == kReadyTimer)
        return TrySignalReadyEvent(h), true;
    if (w == kStatusTimer) {
        std::erase_if(g_app.drainingBars, [](const auto& bar) { return bar->WaitStopRendering(0); });
        if (!g_app.trayAdded)
            AddTrayIcon();
        if (g_app.guard && WaitForSingleObject(g_app.guard, 0) == WAIT_OBJECT_0) {
            Log(L"guard: it went away; started again");
            EnsureGuard(g_app.guard);
        }
        UpdateStatus();
        if (g_app.chrome)
            g_app.chrome->Enforce();
        return true;
    }
    if (w == kMessengerTimer) {
        KillTimer(h, kMessengerTimer);
        bool waiting = false;
        for (const auto& due : g_app.alerts.DueInstances(GetTickCount64(), &waiting))
            PostMessengerCard(due.messenger, due.pid);
        if (waiting)
            SetTimer(h, kMessengerTimer, 500, nullptr);
        return true;
    }
    if (w == kDisplaySettleTimer) {
        KillTimer(h, kDisplaySettleTimer);
        if (g_app.displaysReady) {
            Log(L"core: displays settled: renderers set up again");
            SyncDisplayBars(g_app.displaySettings, true);
            UpdateDesktopMonitor(true);
        }
        return true;
    }
    if (w == kTrayFocusTimer) {
        if (g_app.trayPanel && g_app.trayPanel->MenuSessionActive())
            return SetTimer(h, kTrayFocusTimer, 150, nullptr), true;
        return KillTimer(h, kTrayFocusTimer), RestoreHiddenOverflowFocus(g_app.trayPanel ? g_app.trayPanel->Previous() : nullptr), true;
    }
    return false;
}

void HandlePowerBroadcast(WPARAM w, LPARAM l)
{
    if ((w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) && g_app.updates)
        g_app.updates->OnPowerResume();
    else if (const auto* s = w == PBT_POWERSETTINGCHANGE ? reinterpret_cast<const POWERBROADCAST_SETTING*>(l) : nullptr;
             s && s->PowerSetting == kBrightnessSetting && s->DataLength == sizeof(DWORD)) {
        g_app.brightness = int(std::min<DWORD>(*reinterpret_cast<const DWORD*>(s->Data), 100));
        g_app.control->BrightnessChanged(g_app.brightness);
        UpdateStatus();
    }
}

void HandleDisplayOrDpiMessage(HWND h)
{
    if (!g_app.displaysReady)
        return;
    if (g_app.chrome)
        g_app.chrome->Enforce();
    SyncDisplayBars(g_app.displaySettings, true);
    SetTimer(h, kDisplaySettleTimer, 2000, nullptr);
    UpdateDesktopMonitor(true);
}

void HandleSettingChangeMessage(WPARAM w)
{
    if (w == SPI_SETWORKAREA && g_app.displaysReady) {
        SyncDisplayBars(g_app.displaySettings, true, true);
        UpdateDesktopMonitor(false);
    }
}

void HandleCloseMessage(HWND h)
{
    if (g_app.chrome)
        g_app.chrome->RestoreForExit();
    DestroyWindow(h);
}

void HandleEndSessionMessage(WPARAM w)
{
    if (w) {
        Log(L"core: session ending");
        if (g_app.chrome)
            g_app.chrome->RestoreForExit();
    }
}

void HandleDestroyMessage()
{
    if (g_app.brightnessNotify)
        UnregisterPowerSettingNotification(g_app.brightnessNotify);
    if (g_app.usbNotify)
        UnregisterDeviceNotification(g_app.usbNotify);
    RemoveTrayIcon(), PostQuitMessage(0);
}

} // namespace

// The chat apps' own switches (Settings ▸ Notifications ▸ Sources); other apps' Windows
// notifications follow notifyListener.
bool MessengerOn(Messenger m)
{
    const Settings& s = g_app.displaySettings;
    switch (m) {
    case Messenger::WeChat: return s.notifyWeChat;
    case Messenger::QQ: return s.notifyQQ;
    case Messenger::Feishu: return s.notifyFeishu;
    case Messenger::WeCom: return s.notifyWeCom;
    case Messenger::DingTalk: return s.notifyDingTalk;
    default: return s.notifyListener;
    }
}

void MessengerFlashed(const std::wstring& program, DWORD pid, const std::wstring& hostKey)
{
    const Messenger m = MessengerOfProgram(program);
    if (m == Messenger::None || !g_app.barOn.value_or(true) || !MessengerOn(m))
        return;
    g_app.messengerSources[{m, pid}] = {FoldCase(program), hostKey};
    if (m == Messenger::WeChat && g_app.wechat)
        g_app.wechat->Wake();
    if (g_app.alerts.Flash(m, GetTickCount64(), pid))
        SetTimer(g_app.host, kMessengerTimer, UINT(MessengerAlerts::kWaitMs + 100), nullptr);
}

void HandleTaskbarCreated()
{
    g_app.trayAdded = false;
    AddTrayIcon();
    if (g_app.tray)
        g_app.tray->Refresh();
    if (g_app.chrome)
        g_app.chrome->Enforce();
    if (g_app.widgets)
        g_app.widgets->OnShellRestart();
}

bool HandleUiAndStatusMessage(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    result = 0;
    switch (m) {
    case WM_UPDATE_READY: HandleUpdateReadyMessage(); return true;
    case WM_TRAY: HandleTrayCallback(w, l); return true;
    case WM_OPEN_SETTINGS: OpenSettings(); return true;
    case WM_IME_STATE:
        if (auto sample = TakePosted<ImeWatcher::Sample>(l))
            OnImeSample(*sample);
        return true;
    case WM_WIN_KEY: HandleWinKeyMessage(w); return true;
    case WM_NOTICE: HandleNoticeMessage(l); return true;
    case WM_BADGES: HandleBadgesMessage(); return true;
    case WM_CATALOG: HandleCatalogMessage(w == 0); return true;
    case WM_VOLUME: UpdateStatus(); return true;
    case WM_STATUS: HandleStatusMessage(l); return true;
    case WM_TRAY_ICONS: HandleTrayIconsMessage(h, w); return true;
    case WM_ATRIUM_ADD_WIDGET: result = HandleAddWidgetMessage(w, l); return true;
    case WM_ATRIUM_SETTINGS: ApplySettings(g_app.server->Current()); return true;
    case WM_ATRIUM_SERVER_READY: TrySignalReadyEvent(h); return true;
    default: return false;
    }
}

bool HandleSystemAndLifecycleMessage(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    result = 0;
    switch (m) {
    case WM_POWERBROADCAST:
        HandlePowerBroadcast(w, l);
        result = TRUE;
        return true;
    case WM_DEVICECHANGE:
        if (HandleDeviceBroadcast(w, l)) {
            result = TRUE;
            return true;
        }
        return false;
    case WM_USB: HandleUsbMessage(); return true;
    case WM_TIMER: return HandleHostTimer(h, w);
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: HandleDisplayOrDpiMessage(h); return true;
    case WM_SETTINGCHANGE: HandleSettingChangeMessage(w); return true;
    case WM_CLOSE: HandleCloseMessage(h); return true;
    case WM_ENDSESSION: HandleEndSessionMessage(w); return true;
    case WM_DESTROY: HandleDestroyMessage(); return true;
    default: return false;
    }
}

} // namespace atrium
