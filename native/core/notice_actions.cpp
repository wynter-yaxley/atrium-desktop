#include "notice_actions.h"

#include "desktop_helpers.h"
#include "localization.h"
#include "shell/shell_launch.h"
#include "shell/task_model.h"
#include "shell/tray_model.h"
#include "shell/wechat_source.h"
#include "ui/notifications.h"
#include "win32_path.h"

#include <algorithm>
#include <array>
#include <cwctype>

namespace atrium {

namespace {

std::wstring StemOf(const std::wstring& s)
{
    std::wstring v = FoldCase(s);
    if (const size_t bang = v.find(L'!'); bang != std::wstring::npos)
        v.resize(bang);
    v = v.substr(v.find_last_of(L"\\/") + 1);
    return (v.size() > 4 && v.ends_with(L".exe")) ? v.substr(0, v.size() - 4) : v;
}

std::optional<TrayIcon> TrayIconOf(const TrayModel* tray, const Notice& n)
{
    if (!tray)
        return std::nullopt;
    const auto icons = tray->Icons();
    if (!n.trayHostKey.empty())
        for (const TrayIcon& icon : icons)
            if (icon.hostKey == n.trayHostKey)
                return icon;
    const std::wstring a = StemOf(n.app), b = StemOf(n.appId);
    auto matchesName = [&](const TrayIcon& icon) {
        const std::wstring stem = icon.exe.empty() ? L"" : StemOf(icon.exe), tip = FoldCase(TrayModel::Title(icon.tip));
        return std::ranges::any_of(std::array<const std::wstring*, 2>{&a, &b}, [&](const std::wstring* ptr) {
            const std::wstring& name = *ptr;
            return name.size() >= 2 && (stem == name || (tip.starts_with(name) && (tip.size() == name.size() || !iswalnum(tip[name.size()]))));
        });
    };
    if (n.pid != 0)
        for (const TrayIcon& icon : icons)
            if (icon.pid == n.pid && matchesName(icon))
                return icon;
    std::optional<TrayIcon> firstMatch;
    for (const TrayIcon& icon : icons) {
        if (!matchesName(icon))
            continue;
        if (icon.flashing)
            return icon;
        if (!firstMatch)
            firstMatch = icon;
    }
    return firstMatch;
}

bool SameAppItem(const TaskItem& t, const PinnedApp& pin, const std::wstring& key, Messenger m, const TaskModel* tasks)
{
    if (t.key == key || (!pin.aumid.empty() && FoldCase(pin.aumid) == key) || (!pin.path.empty() && FoldCase(pin.path) == key) ||
        (tasks && FoldCase(tasks->FileLocation(t.key)) == key))
        return true;
    const std::wstring aumid = FoldCase(!pin.aumid.empty() ? pin.aumid : t.key);
    return m != Messenger::None && (MessengerOfProgram(pin.path) == m || MessengerOfProgram(t.key) == m || MessengerOfNotice(L"windows", t.name, aumid) == m ||
                                    (m == Messenger::QQ && (aumid.find(L"tencentqq") != std::wstring::npos || aumid.find(L".qq") != std::wstring::npos ||
                                                            aumid.find(L"qqdesktop") != std::wstring::npos || aumid.find(L"qqnt") != std::wstring::npos)));
}

bool LaunchNoticeApp(const std::wstring& aumid)
{
    AllowSetForegroundWindow(ASFW_ANY);
    const bool isDrivePath = aumid.size() > 2 && aumid[1] == L':';
    LaunchRequest req;
    req.target = aumid;
    req.verb = L"open";
    req.kind = isDrivePath ? LaunchTargetKind::Executable : LaunchTargetKind::AppUserModelId;
    req.privilege = LaunchPrivilege::ShellDefault;
    return ShellLaunch(req).outcome == LaunchOutcome::Succeeded;
}

} // namespace

bool ShouldOpenSystemNotificationCenter(std::wstring_view source, std::wstring_view appId)
{
    if (source != L"windows" || appId.empty())
        return false;
    const std::wstring folded = FoldCase(std::wstring(appId));
    return folded.find(L"screensketch") != std::wstring::npos || folded.find(L"screenclippinghost") != std::wstring::npos;
}

NoticeAppRoute ChooseNoticeAppRoute(const NoticeAppRouteInput& in)
{
    if (in.openSystemNotificationCenter)
        return NoticeAppRoute::OpenSystemNotificationCenter;
    if (in.hasMatchingTitleWindow)
        return NoticeAppRoute::ActivateTitleWindow;
    if (in.isWeChatChat)
        return NoticeAppRoute::WeChatOpenChat;
    if (in.hasTrayIcon && (in.trayFlashing || !in.hasAppWindow))
        return NoticeAppRoute::ClickTrayIcon;
    return in.hasAppWindow ? NoticeAppRoute::ShowAppWindow : (in.isPackagedAumid ? NoticeAppRoute::ActivateAumid : NoticeAppRoute::ShellOpen);
}

std::pair<std::wstring, HWND> FindAppTarget(const TaskModel* tasks, const std::wstring& key, Messenger messenger, const std::wstring& title, DWORD pid = 0)
{
    if (!tasks)
        return {};
    std::wstring appTaskKey;
    HWND titleHwnd = nullptr;
    for (const TaskItem& t : tasks->Snapshot(true)) {
        if (t.windows.empty() || !SameAppItem(t, tasks->PinFor(t.key), key, messenger, tasks))
            continue;
        for (HWND h : t.windows) {
            DWORD winPid = 0;
            if (pid != 0 && (!GetWindowThreadProcessId(h, &winPid) || winPid != pid))
                continue;
            if (appTaskKey.empty())
                appTaskKey = t.key;
            wchar_t buf[512]{};
            // InternalGetWindowText never sends WM_GETTEXT, so a hung window cannot block the UI thread.
            if (!title.empty() && !titleHwnd && InternalGetWindowText(h, buf, ARRAYSIZE(buf)) > 0 && buf == title)
                titleHwnd = h;
        }
    }
    return {appTaskKey, titleHwnd};
}

bool ShowNoticeAppTarget(TaskModel* tasks, const std::wstring& key, DWORD pid) { return !key.empty() && tasks && tasks->Show(key, pid); }

bool RunNoticeAction(const NoticeActionEnv& env, const Notice& n)
{
    if (n.action == L"settings:updates")
        return env.openSettings ? (env.openSettings(L"about"), true) : true;
    if (n.action.starts_with(L"uri:")) {
        LaunchRequest req;
        req.target = n.action.substr(4);
        req.verb = L"open";
        req.kind = LaunchTargetKind::Uri;
        req.privilege = LaunchPrivilege::ShellDefault;
        return ShellLaunch(req).outcome == LaunchOutcome::Succeeded;
    }
    if (!n.action.starts_with(L"app:"))
        return true;

    const std::wstring aumid = n.action.substr(4);
    const std::optional<TrayIcon> trayIcon = TrayIconOf(env.tray, n);
    const auto [appTaskKey, titleHwnd] = FindAppTarget(env.tasks, FoldCase(aumid), MessengerOfNotice(n.source, n.app, n.appId), n.title, n.pid);
    auto clickTray = [&] {
        if (env.tray && trayIcon)
            AllowSetForegroundWindow(ASFW_ANY), env.tray->Click(*trayIcon, false);
    };
    const bool openSystemCenter = ShouldOpenSystemNotificationCenter(n.source, n.appId.empty() ? aumid : n.appId);
    switch (ChooseNoticeAppRoute({titleHwnd != nullptr, n.source == L"wechat" && (n.title != Tr(L"New message") || !n.body.empty()), trayIcon.has_value(),
                                  trayIcon && trayIcon->flashing, !appTaskKey.empty(), aumid.find(L'!') != std::wstring::npos, openSystemCenter})) {
    case NoticeAppRoute::OpenSystemNotificationCenter: SendWinShortcut('N'); return true;
    case NoticeAppRoute::ActivateTitleWindow: TaskModel::ActivateWindow(titleHwnd); return true;
    case NoticeAppRoute::WeChatOpenChat:
        if (!ShowNoticeAppTarget(env.tasks, appTaskKey, n.pid)) {
            if (trayIcon && env.tray)
                clickTray();
            else if (!(env.tray && env.tray->OpenExisting(aumid, n.pid)))
                LaunchNoticeApp(aumid);
        }
        if (env.wechat)
            env.wechat->OpenChat(n.title, n.pid);
        return true;
    case NoticeAppRoute::ClickTrayIcon: clickTray(); return true;
    case NoticeAppRoute::ShowAppWindow: ShowNoticeAppTarget(env.tasks, appTaskKey, n.pid); return true;
    case NoticeAppRoute::ActivateAumid:
    case NoticeAppRoute::ShellOpen: return LaunchNoticeApp(aumid);
    }
    return true;
}

void WithdrawForegroundMessenger(MessengerAlerts& alerts, Notifications* notes)
{
    const HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg)
        GetWindowThreadProcessId(fg, &fgPid);
    if (const Messenger m = fg ? MessengerOfProgram(ProgramOf(fg)) : Messenger::None; m != Messenger::None) {
        alerts.Cancel(m, fgPid);
        if (notes)
            notes->Post(Notice{.source = InfoOf(m).key, .title = Tr(L"New message"), .unread = false, .pid = fgPid});
    }
}

Notice BuildMessengerCardNotice(Messenger m, DWORD pid, const MessengerSourceMeta& meta)
{
    const MessengerInfo& info = InfoOf(m);
    Notice n;
    n.source = info.key;
    n.app = Tr(info.name);
    n.appId = meta.program;
    n.title = Tr(L"New message");
    n.pid = pid;
    n.trayHostKey = meta.hostKey;
    if (!meta.program.empty())
        n.action = L"app:" + meta.program;
    return n;
}

} // namespace atrium
