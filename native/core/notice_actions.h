// What a notification card does when it is clicked, and when its sender's app comes to the front.
#pragma once

#include "notice.h"
#include "shell/messengers.h"

#include <windows.h>

#include <string_view>

namespace atrium {

class Notifications;
class TaskModel;
class TrayModel;
class WeChatSource;

enum class NoticeAppRoute { OpenSystemNotificationCenter, ActivateTitleWindow, WeChatOpenChat, ClickTrayIcon, ShowAppWindow, ActivateAumid, ShellOpen };

struct NoticeAppRouteInput {
    bool hasMatchingTitleWindow = false, isWeChatChat = false, hasTrayIcon = false, trayFlashing = false, hasAppWindow = false, isPackagedAumid = false,
         openSystemNotificationCenter = false;
};

bool ShouldOpenSystemNotificationCenter(std::wstring_view source, std::wstring_view appId);
NoticeAppRoute ChooseNoticeAppRoute(const NoticeAppRouteInput& in);

struct NoticeActionEnv {
    TrayModel* tray = nullptr;
    TaskModel* tasks = nullptr;
    WeChatSource* wechat = nullptr;
    void (*openSettings)(const wchar_t* page) = nullptr;
};
bool RunNoticeAction(const NoticeActionEnv& env, const Notice& n);
void WithdrawForegroundMessenger(MessengerAlerts& alerts, Notifications* notes);
Notice BuildMessengerCardNotice(Messenger m, DWORD pid, const MessengerSourceMeta& meta);
} // namespace atrium
