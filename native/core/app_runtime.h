// Shared resident host state, timer/notification constants, bar orchestration, and lifecycle APIs.
#pragma once

#include "app_menus.h"
#include "notice_actions.h"
#include "settings.h"
#include "shell/ime_status.h"
#include "shell/messengers.h"
#include "shell/system_status.h"

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace atrium {

class Bar;
class CalendarPanel;
class ControlCenter;
class DesktopWidgets;
class ExtrasHost;
class FeishuSource;
class GenieMinimize;
class ImeBubble;
class ImeWatcher;
class NotificationListener;
class Notifications;
class SettingsServer;
class StageManager;
class StartMenu;
class SystemChrome;
class SystemStatusReader;
class TrayModel;
class TrayPanel;
class UpdateChecker;
class UsbVolumeMonitor;
class WeChatSource;
class WinKey;
class WindowPreview;

inline constexpr UINT_PTR kStatusTimer = 0x52;        // the bar's status icon
inline constexpr UINT_PTR kTrayFocusTimer = 0x53;     // after a forwarded tray click
inline constexpr UINT_PTR kReadyTimer = 0x54;         // only during an update startup handshake
inline constexpr UINT_PTR kDisplaySettleTimer = 0x56; // once more after a display change, the topology settled
inline constexpr UINT_PTR kMessengerTimer = 0x57;     // a chat app's flash: did a card with its text follow?
inline constexpr UINT kStatusMs = 3000;
inline constexpr UINT kTrayId = 1;

// GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS: Windows reports every change of the
// built-in screen's brightness (keys, quick settings, our slider), and the
// current level once on registration.
inline constexpr GUID kBrightnessSetting = {0x8ffee2c6, 0x2d01, 0x46be, {0xad, 0xb9, 0x39, 0x8a, 0xdd, 0xc5, 0xb4, 0xff}};
inline constexpr GUID kVolumeInterface = {0x53f5630d, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}}; // GUID_DEVINTERFACE_VOLUME

struct AppState {
    AppState();
    ~AppState();
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    HINSTANCE instance = nullptr;
    HWND host = nullptr;
    UINT taskbarCreated = 0;
    bool trayAdded = false;
    std::unique_ptr<Bar> bar;
    std::vector<std::unique_ptr<Bar>> secondaryBars, drainingBars;
    Bar* activeBar = nullptr; // the last taskbar the pointer interacted with
    SystemStatus lastStatus;
    bool capturable = false;
    bool displaysReady = false;
    ULONGLONG switchedBarAt = 0; // switching displays must not suppress the first popup click
    std::unique_ptr<SystemStatusReader> status;
    std::unique_ptr<SettingsServer> server;
    std::unique_ptr<TaskModel> tasks;
    std::unique_ptr<AppCatalog> catalog;
    std::unique_ptr<StartMenu> start;
    std::unique_ptr<Notifications> notes;
    std::unique_ptr<NotificationListener> listener;
    std::unique_ptr<UsbVolumeMonitor> usb;
    std::unique_ptr<UpdateChecker> updates;
    std::unique_ptr<WeChatSource> wechat;
    std::unique_ptr<FeishuSource> feishu;
    MessengerAlerts alerts;
    std::map<std::pair<Messenger, DWORD>, MessengerSourceMeta> messengerSources;
    std::unique_ptr<TrayModel> tray;
    std::unique_ptr<TrayPanel> trayPanel;
    std::unique_ptr<ExtrasHost> extras; // Launchpad, the now-playing entry, the folder stack
    std::unique_ptr<ControlCenter> control;
    std::unique_ptr<CalendarPanel> calendar;
    std::unique_ptr<DesktopWidgets> widgets;
    std::unique_ptr<SystemChrome> chrome;
    std::unique_ptr<GlassMenu> menu;
    std::unique_ptr<WindowPreview> preview;
    std::unique_ptr<StageManager> stage;
    std::unique_ptr<GenieMinimize> genie;
    std::unique_ptr<WinKey> winKey;
    std::unique_ptr<ImeWatcher> imeWatcher; // the events that may change the input method (issue #56)
    ImeWatch imeWatch;                      // what announces (a foreground change only reseeds it)
    std::unique_ptr<ImeBubble> ime;         // the bubble that announces while the bar is hidden
    bool imeOn = true;                      // the "show the input language" setting, as last applied
    bool imeBubble = true;                  // imeBubble, as last applied
    HANDLE guard = nullptr;                 // the guard process (EnsureGuard)
    bool winKeyStart = false, winNumbers = false; // what it watches
    bool widgetsOn = false;
    std::wstring desktopDisplay; // effective setting, also available during display broadcasts
    Settings displaySettings;
    std::optional<bool> barOn; // the bar module, as last applied
    int brightness = -1;       // the built-in screen's, from GUID_VIDEO_CURRENT_MONITOR_BRIGHTNESS
    HPOWERNOTIFY brightnessNotify = nullptr;
    HDEVNOTIFY usbNotify = nullptr; // volume interface events, for the USB card
    std::wstring readyEvent;
};

extern AppState g_app;

Bar& ActiveBar();
Bar* WinKeyBar();

template<class Action> void ForBars(const Action& action)
{
    action(*g_app.bar);
    for (const auto& bar : g_app.secondaryBars)
        action(*bar);
}

void AddTrayIcon();
void RemoveTrayIcon();
void OpenTray();
void OpenSystem(const std::wstring& what);
void UpdateStatus();

void ToggleTray(RECT);
void ToggleControl(RECT);
void ToggleCalendar();
void ToggleStart(RECT);

void SetBarModule(bool on);
void SetWidgetsModule(bool on);
void SetStageManager(bool on);

AppMenuEnv MenuEnv();
NoticeActionEnv NoticeEnv();
void ShowMenu(POINT pt);
void ShowStartMenu(POINT pt);
void ShowItemMenu(const std::wstring& key, POINT pt);
void ShowCatalogMenu(const CatalogApp& app, POINT pt, std::function<void(bool)> done, std::vector<AppMenuExtra> extras);
void ShowOverflow(const std::vector<std::wstring>& keys, RECT anchor);
void Reorder(const std::wstring& key, int to);

void ApplySettings(const Settings& s);
bool BarPopupOpen();
void CloseBarPopups();
void ConfigureBar(Bar& bar);
std::vector<TaskItem> BarItems();
void SyncDisplayBars(const Settings& settings, bool refresh = false, bool workAreaOnly = false);
void UpdateDesktopMonitor(bool notify);

void InitializeSubsystems(HINSTANCE instance, Settings& settings);
void ShutdownApp(HANDLE mutex, std::thread& startupThread);

} // namespace atrium
