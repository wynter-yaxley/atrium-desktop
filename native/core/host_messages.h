// AtriumCoreHost window messages shared by main.cpp and settings_server.h.
#pragma once

#include <windows.h>

namespace atrium {

// Effective settings changed; lParam unused, host reads SettingsServer::Current().
constexpr UINT WM_ATRIUM_SETTINGS = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2;
constexpr UINT WM_OPEN_SETTINGS = WM_APP + 3; // sent by a second Atrium.exe
constexpr UINT WM_CATALOG = WM_APP + 4;       // the app catalog has news
constexpr UINT WM_NOTICE = WM_APP + 5;        // lParam: Notice* from the listener thread
constexpr UINT WM_TRAY_ICONS = WM_APP + 6;    // the tray model has read the icons
constexpr UINT WM_VOLUME = WM_APP + 7;        // the default playback device's volume changed
// Sent by the pipe thread: WidgetKind in wParam, WidgetSize in lParam (scalars).
constexpr UINT WM_ATRIUM_ADD_WIDGET = WM_APP + 9;
constexpr UINT WM_STATUS = WM_APP + 10;       // lParam: SystemStatus*, read off the UI thread
// Background package is validated and ready; also posted cross-process as 0x8000 + 11 by settings/Atrium.Settings/ReleaseInstaller.cs.
constexpr UINT WM_UPDATE_READY = WM_APP + 11;
constexpr UINT WM_WIN_KEY = WM_APP + 12;      // Windows key alone (0, winKeyStart) or Win + 1..9, 0 (1..10, winNumbers)
constexpr UINT WM_USB = WM_APP + 13;          // the device worker has queued removal notices
constexpr UINT WM_IME_STATE = WM_APP + 20;    // the input method may have changed (ImeWatcher)
constexpr UINT WM_BADGES = WM_APP + 21;       // an app's unread badge count changed (WeChatSource)
constexpr UINT WM_ATRIUM_SERVER_READY = WM_APP + 22; // settings server pipe is listening and Ready() is true
constexpr UINT WM_ATRIUM_AGENT_DRAG_OVER = WM_APP + 23;
constexpr UINT WM_ATRIUM_AGENT_DRAG_LEAVE = WM_APP + 24;
constexpr UINT WM_ATRIUM_AGENT_DROP_FILES = WM_APP + 25;

} // namespace atrium
