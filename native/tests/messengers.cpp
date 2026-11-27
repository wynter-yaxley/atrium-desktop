#include "notice_actions.h"
#include "shell/messengers.h"
#include "shell/notification_listener.h"
#include "shell/wechat_source.h"
#include "ui/notification_layout.h"

#include <iostream>

using namespace atrium;

namespace {
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "messengers: " << message << '\n';
        g_failed = true;
    }
}

void CheckWeChatTime()
{
    Require(IsWeChatTime(L"17:06"), "24h clock time");
    Require(IsWeChatTime(L"10/27"), "month/day date");
    Require(IsWeChatTime(L"2025/10/27"), "slash date");
    Require(IsWeChatTime(L"2025-10-27"), "dash date");
    Require(IsWeChatTime(L"刚刚"), "just now");
    Require(IsWeChatTime(L"昨天"), "yesterday in Chinese");
    Require(IsWeChatTime(L"前天"), "day before yesterday in Chinese");
    Require(IsWeChatTime(L"星期三"), "weekday in Chinese");
    Require(IsWeChatTime(L"Yesterday"), "yesterday in English");
    Require(IsWeChatTime(L"3:45 PM"), "12h PM time");
    Require(IsWeChatTime(L"11:05 am"), "12h am time");
    Require(IsWeChatTime(L"5分钟前"), "minutes ago");
    Require(IsWeChatTime(L"2小时前"), "hours ago");
    Require(IsWeChatTime(L"2025年10月27日"), "CJK full date");
    Require(!IsWeChatTime(L""), "empty string is not a time");
    Require(!IsWeChatTime(L"你好"), "greeting text is not a time");
    Require(!IsWeChatTime(L"[图片]"), "media tag is not a time");
    Require(!IsWeChatTime(L"---"), "punctuation alone is not a time");
    Require(!IsWeChatTime(L"1") && !IsWeChatTime(L"666") && !IsWeChatTime(L"123456"), "pure numbers are not times");
}

void CheckWeChatRows()
{
    Require(IsCollapsedWeChatRow(L"折叠的群聊"), "collapsed group chats");
    Require(IsCollapsedWeChatRow(L"折叠置顶聊天"), "collapsed pinned chats");
    Require(IsCollapsedWeChatRow(L"折叠的聊天"), "collapsed chats");
    Require(IsCollapsedWeChatRow(L"Collapsed Chats"), "English collapsed chats");
    Require(IsCollapsedWeChatRow(L"Folded Group Chats"), "English folded group chats");
    Require(!IsCollapsedWeChatRow(L"产品讨论群"), "normal group is not collapsed");
    Require(IsOfficialWeChatRow(L"公众号") && IsOfficialWeChatRow(L" 服务号 ") && IsOfficialWeChatRow(L"Official Accounts"), "official accounts' rows");
    Require(!IsOfficialWeChatRow(L"公众号运营群") && !IsOfficialWeChatRow(L"Alice"), "a chat is not the official accounts' row");
    Require(ParseWeChatRow(L"订阅号消息\n[3条] 某号: 今日推送\n17:06").official, "an official accounts' row parses as such");

    const WeChatRow plain = ParseWeChatRow(L"Alice\n你好吗\n17:06");
    Require(plain.chat == L"Alice" && plain.summary == L"你好吗" && plain.unread == 0 && !plain.muted && !plain.collapsed, "plain text row");

    const WeChatRow sticker = ParseWeChatRow(L"Alice\n[1条] [动画表情]\n17:06");
    Require(sticker.unread == 1 && sticker.summary == L"[动画表情]" && !sticker.muted, "inline sticker after unread tag");

    const WeChatRow splitLink = ParseWeChatRow(L"Bob\n[2条]\n[链接] 标题\n3:45 PM");
    Require(splitLink.unread == 2 && splitLink.summary == L"[链接] 标题", "split unread line and link summary");

    const WeChatRow mention = ParseWeChatRow(L"工作群\n[有人@我]\n[1条] 请看下文档\n5分钟前");
    Require(mention.unread == 1 && mention.summary == L"请看下文档", "mention badge before unread tag");

    const WeChatRow pinnedFile = ParseWeChatRow(L"工作群\n已置顶\n[2条] [文件] report.pdf\n昨天");
    Require(pinnedFile.unread == 2 && pinnedFile.summary == L"[文件] report.pdf", "pinned badge before unread file tag");

    const WeChatRow enUnread = ParseWeChatRow(L"Team\n[3 unread] Standup at 4\nYesterday");
    Require(enUnread.unread == 3 && enUnread.summary == L"Standup at 4", "English unread tag");

    const WeChatRow enMessages = ParseWeChatRow(L"Team\n[2 messages]\n[图片]\n10:20 AM");
    Require(enMessages.unread == 2 && enMessages.summary == L"[图片]", "English messages tag with image");

    const WeChatRow saturated = ParseWeChatRow(L"大群\n[99+条] 收到\n刚刚");
    Require(saturated.unread == 99 && saturated.summary == L"收到", "99+ saturated unread count");

    const WeChatRow muted = ParseWeChatRow(L"通知群\n[5条] 周报提醒\n消息免打扰\n10:00");
    Require(muted.unread == 5 && muted.muted && !muted.collapsed && muted.summary == L"周报提醒", "muted row");

    const WeChatRow collapsed = ParseWeChatRow(L"折叠的群聊\n[4条] 群A: 收到\n2小时前");
    Require(collapsed.collapsed && collapsed.muted && collapsed.unread == 4 && collapsed.summary == L"群A: 收到", "collapsed row is muted");

    const WeChatRow collapsedPinned = ParseWeChatRow(L"折叠置顶聊天\n[1条] 群B: 测试\n2025年10月27日");
    Require(collapsedPinned.collapsed && collapsedPinned.muted && collapsedPinned.unread == 1, "collapsed pinned row is muted");

    const WeChatRow fallbackUsed = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"[语音]");
    Require(fallbackUsed.unread == 1 && fallbackUsed.summary == L"[语音]", "fallback text fills missing summary");

    const WeChatRow fallbackIgnored = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"Alice");
    Require(fallbackIgnored.unread == 1 && fallbackIgnored.summary.empty(), "fallback matching chat name is ignored");

    const WeChatRow fallbackTimeIgnored = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"10:00");
    Require(fallbackTimeIgnored.unread == 1 && fallbackTimeIgnored.summary.empty(), "fallback matching time is ignored");

    const WeChatRow numericSummary = ParseWeChatRow(L"Alice\n[1条] 666\n17:06");
    Require(numericSummary.unread == 1 && numericSummary.summary == L"666", "pure numeric summary is not dropped as time");

    Require(ParseWeChatRow(L"通知群\n[2条] hi\n10:00 消息免打扰").muted, "substring mute on line");
    Require(ParseWeChatRow(L"通知群\n已置顶 免打扰\n[2条] hi").muted, "short mute variant");
    Require(ParseWeChatRow(L"通知群\n訊息免打擾\n[2条] hi").muted, "traditional mute variant");
    Require(ParseWeChatRow(L"通知群\n免打擾\n[2条] hi").muted, "traditional short mute variant");
    Require(ParseWeChatRow(L"Team\n[1 unread] hi\nMute Notifications").muted, "English Mute Notifications");
    Require(ParseWeChatRow(L"Team\nnotifications muted\n10:00").muted, "English Notifications Muted case-insensitive");
    Require(ParseWeChatRow(L"Team\nMuted\n10:00").muted, "English Muted line");
    Require(!ParseWeChatRow(L"免打扰群\n[1条] hi\n10:00").muted, "a chat named with the mark is not muted");
    const WeChatRow saysMute = ParseWeChatRow(L"Alice\n[1条] 我把群设成免打扰了\n10:00");
    Require(!saysMute.muted && saysMute.summary == L"我把群设成免打扰了", "a summary mentioning the mark is not muted");
    Require(!ParseWeChatRow(L"Team\n[1 unread] I muted the call\n10:00").muted, "an English summary mentioning muted is not muted");

    const WeChatRow mutedFallback = ParseWeChatRow(L"通知群\n[1条] 周报\n10:00", L"消息免打扰");
    Require(mutedFallback.muted && mutedFallback.summary == L"周报", "fallback mute applies even when summary is present");
    Require(ParseWeChatRow(L"Team\n[1 unread] hi\n10:00", L"Notifications Muted").muted, "fallback English mute");

    const WeChatRow mutedStatus = ParseWeChatRow(L"通知群\n[1条] 周报\n10:00", L"", L"免打扰");
    Require(mutedStatus.muted && mutedStatus.summary == L"周报", "itemStatus mute applies");
    Require(ParseWeChatRow(L"Team\n[1 unread] hi\n10:00", L"", L"muted").muted, "itemStatus English mute");

    Require(MatchesWeChatCellName(L"Alice\n[1条] hi\n10:00", L"Alice"), "cell match exact");
    Require(MatchesWeChatCellName(L"Alice\n已置顶\n[2条] hello\n消息免打扰\n10:00", L"Alice"), "cell match with badges");
    Require(!MatchesWeChatCellName(L"Alice Smith\n[1条] hi\n10:00", L"Alice"), "cell match is not substring");
    Require(!MatchesWeChatCellName(L"Alice\n[1条] hi\n10:00", L""), "cell match rejects empty chat");
    Require(!MatchesWeChatCellName(L"", L"Alice"), "cell match rejects empty cell name");
}

void CheckWeChatDiff()
{
    std::map<std::wstring, WeChatChatState> s{{L"off", {2, L"old"}}, {L"big", {99, L"m1"}}};
    Require(DiffWeChatRows({{L"a", {L"A", L"hi", 0}}}, false, false, s).empty() && s[L"off"].unread == 2, "keep");
    Require(DiffWeChatRows({{L"off", {L"Off", L"old", 2}}}, true, false, s).empty(), "no re-fire");
    auto c = DiffWeChatRows({{L"off", {L"Off", L"old", 0}}}, false, false, s);
    Require(c.size() == 1 && !c[0].unread && s[L"off"].unread == 0, "unread -> 0 clears");
    Require(DiffWeChatRows({{L"big", {L"Big", L"m1", 99}}}, true, false, s).empty(), "99+ same");
    auto sat = DiffWeChatRows({{L"big", {L"Big", L"m2", 99}}}, true, false, s);
    Require(sat.size() == 1 && sat[0].unread && sat[0].body == L"m2", "99+ new summary");

    std::vector<std::pair<std::wstring, WeChatRow>> rows{
        {L"a", {L"Alice", L"hi", 3, false, false, false}},
        {L"m", {L"Muted", L"hi", 5, true, false, false}},
        {L"c", {L"Collapsed", L"hi", 4, false, true, false}},
        {L"o", {L"公众号", L"news", 2, false, false, true}},
    };
    Require(SumWeChatUnread(rows, false) == 5, "sum excludes muted and collapsed");
    Require(SumWeChatUnread(rows, true) == 3, "sum excludes official when blocked");
    Require(SumWeChatUnread({}, true) == 0, "empty rows sum to 0");

    // Unmuting a previously muted chat alerts again (no sticky lastMuted).
    std::map<std::wstring, WeChatChatState> muteStates;
    Require(DiffWeChatRows({{L"g", {L"Group", L"m1", 1, true}}}, true, false, muteStates).empty(), "muted chat suppressed");
    auto unmuted = DiffWeChatRows({{L"g", {L"Group", L"m2", 2, false}}}, true, false, muteStates);
    Require(unmuted.size() == 1 && unmuted[0].unread && unmuted[0].title == L"Group", "unmuted chat alerts again");

    // #190: open-chat (unread == 0) summary change alerts once; unchanged/unseen/muted/collapsed/blocked ignored;
    // oldUnread > 0 -> 0 stays withdrawal; baseline == false syncs silently.
    std::map<std::wstring, WeChatChatState> openStates{
        {L"open", {0, L"msg1"}}, {L"muted0", {0, L"m1"}}, {L"collapsed0", {0, L"c1"}}, {L"official0", {0, L"o1"}}, {L"wasunread", {2, L"old"}},
    };
    Require(DiffWeChatRows({{L"open", {L"OpenChat", L"msg1", 0}}}, true, false, openStates).empty(), "#190: unchanged unread==0 summary is ignored");
    Require(DiffWeChatRows({{L"unseen0", {L"Unseen", L"hi", 0}}}, true, false, openStates).empty(),
            "#190: unseen unread==0 chat does not alert on first appearance");
    Require(DiffWeChatRows({{L"muted0", {L"Muted0", L"m2", 0, true}}}, true, false, openStates).empty(), "#190: muted unread==0 summary change is ignored");
    Require(DiffWeChatRows({{L"collapsed0", {L"Collapsed0", L"c2", 0, false, true}}}, true, false, openStates).empty(),
            "#190: collapsed unread==0 summary change is ignored");
    Require(DiffWeChatRows({{L"official0", {L"Official0", L"o2", 0, false, false, true}}}, true, true, openStates).empty(),
            "#190: blocked official unread==0 summary change is ignored");

    const auto openAlert = DiffWeChatRows({{L"open", {L"OpenChat", L"msg2", 0}}}, true, false, openStates);
    Require(openAlert.size() == 1 && openAlert[0].unread && openAlert[0].title == L"OpenChat" && openAlert[0].body == L"msg2",
            "#190: open-chat unread==0 summary change alerts once");
    Require(DiffWeChatRows({{L"open", {L"OpenChat", L"msg2", 0}}}, true, false, openStates).empty(),
            "#190: open-chat unread==0 summary change does not repeat");

    const auto withdrawn = DiffWeChatRows({{L"wasunread", {L"WasUnread", L"new summary", 0}}}, true, false, openStates);
    Require(withdrawn.size() == 1 && !withdrawn[0].unread, "#190: oldUnread > 0 -> 0 stays withdrawal even when summary changes");

    // baseline == false (e.g. ForegroundShown -> None silent sync) updates summary without posting a card.
    Require(DiffWeChatRows({{L"open", {L"OpenChat", L"sent in foreground", 0}}}, false, false, openStates).empty(),
            "#190: baseline==false suppresses open-chat unread==0 summary alert");
    Require(openStates[L"open"].summary == L"sent in foreground" &&
                DiffWeChatRows({{L"open", {L"OpenChat", L"sent in foreground", 0}}}, true, false, openStates).empty(),
            "#190: baseline==false silently syncs summary so next baseline==true poll stays quiet");

    // ForegroundShown -> None sync frame (allowOpenSummary = false) suppresses openSummaryRose
    // while still alerting when real unread messages arrive immediately after leaving WeChat.
    std::map<std::wstring, WeChatChatState> switchStates{{L"open", {0, L"before"}}, {L"peer", {0, L"p0"}}};
    const auto switchEv =
        DiffWeChatRows({{L"open", {L"OpenChat", L"typed in front", 0}}, {L"peer", {L"Peer", L"new msg", 1}}}, true, false, switchStates, false);
    Require(switchEv.size() == 1 && switchEv[0].unread && switchEv[0].title == L"Peer" && switchStates[L"open"].summary == L"typed in front",
            "#190: ForegroundShown -> None sync silences open-chat summary while delivering real unread 0 -> 1");
}

void CheckWeChatThrottle()
{
    // Initial foreground sample when nothing is held yet.
    {
        WeChatThrottleInput in{WeChatBusyState::ForegroundShown, false, 0, 5000, 1000, 0, false, false, 0};
        const auto out = EvaluateWeChatThrottle(in);
        Require(out.waitMs == 0 && out.nextSampledWhileBusy && !out.clearReadingAndBaseline, "initial foreground sample reads immediately");
    }
    // ForegroundShown with unread == 0 stays blocked when held rows already exist.
    {
        WeChatThrottleInput in{WeChatBusyState::ForegroundShown, true, 0, 5000, 1000, 0, true, false, 0};
        const auto out = EvaluateWeChatThrottle(in);
        Require(out.waitMs == 1000 && out.clearReadingAndBaseline, "foreground with zero unread does not poll UIA");
    }
    // ForegroundShown with unread > 0 debounces at 2s so reading inside WeChat clears Atrium's badge.
    {
        WeChatThrottleInput soon{WeChatBusyState::ForegroundShown, true, 3, 2500, 1000, 0, true, false, 0};
        const auto waitSoon = EvaluateWeChatThrottle(soon);
        Require(waitSoon.waitMs == 500 && waitSoon.clearReadingAndBaseline, "foreground unread read debounces until 2s");

        WeChatThrottleInput due{WeChatBusyState::ForegroundShown, true, 3, 3000, 1000, 0, true, false, 0};
        const auto allowDue = EvaluateWeChatThrottle(due);
        Require(allowDue.waitMs == 0 && allowDue.clearReadingAndBaseline, "foreground unread read proceeds after 2s");
    }
    // Modal dialogs stay blocked even when unread > 0.
    {
        WeChatThrottleInput dlg{WeChatBusyState::Dialog, true, 3, 5000, 1000, 0, true, false, 0};
        const auto out = EvaluateWeChatThrottle(dlg);
        Require(out.waitMs == 1000 && out.clearReadingAndBaseline, "dialog state never polls UIA");
    }
    // Cool-down break transitions and throttle bypass.
    {
        Require(ShouldBreakWeChatCooldown(WeChatBusyState::None, WeChatBusyState::ForegroundShown), "None -> ForegroundShown breaks");
        Require(ShouldBreakWeChatCooldown(WeChatBusyState::ForegroundShown, WeChatBusyState::None), "ForegroundShown -> None breaks");
        Require(ShouldBreakWeChatCooldown(WeChatBusyState::Foreground, WeChatBusyState::None), "Foreground -> None breaks");
        Require(!ShouldBreakWeChatCooldown(WeChatBusyState::None, WeChatBusyState::Foreground), "None -> Foreground does not break");
        Require(!ShouldBreakWeChatCooldown(WeChatBusyState::ForegroundShown, WeChatBusyState::ForegroundShown), "same state does not break");
        Require(!ShouldBreakWeChatCooldown(WeChatBusyState::Dialog, WeChatBusyState::None), "Dialog -> None does not break");

        WeChatThrottleInput enter{WeChatBusyState::ForegroundShown, true, 3, 1500, 1000, 0, true, false, 0, true};
        const auto enterOut = EvaluateWeChatThrottle(enter);
        Require(enterOut.waitMs == 0 && enterOut.nextSampledWhileBusy && enterOut.clearReadingAndBaseline, "breakCooldown bypasses 2s wait on ForegroundShown");

        WeChatThrottleInput leave{WeChatBusyState::None, true, 3, 1500, 1000, 1700, true, false, 0, true};
        const auto leaveOut = EvaluateWeChatThrottle(leave);
        Require(leaveOut.waitMs == 0, "breakCooldown bypasses busyUntilTick and 1s gap on None");

        // #190: ForegroundShown with unread==0 keeps 1000 ms wait and 0 polls even on breakCooldown.
        WeChatThrottleInput fgZeroBreak{WeChatBusyState::ForegroundShown, true, 0, 1500, 1000, 0, true, false, 0, true};
        const auto fgZeroBreakOut = EvaluateWeChatThrottle(fgZeroBreak);
        Require(fgZeroBreakOut.waitMs == 1000 && fgZeroBreakOut.clearReadingAndBaseline,
                "#190: ForegroundShown with unread==0 keeps 1000 ms wait and zero UIA polls on breakCooldown");

        // #190: None 300 ms cooldown wait keeps !clearReadingAndBaseline.
        WeChatThrottleInput noneCooldown{WeChatBusyState::None, true, 0, 1100, 1000, 1300, false, false, 0, false};
        const auto noneCooldownOut = EvaluateWeChatThrottle(noneCooldown);
        Require(noneCooldownOut.waitMs == 200 && !noneCooldownOut.clearReadingAndBaseline, "#190: None 300 ms cooldown keeps !clearReadingAndBaseline");
    }
}

void CheckNoticeRouting()
{
    Require(ChooseNoticeAppRoute({true, true, true, true, true, true}) == NoticeAppRoute::ActivateTitleWindow &&
                ChooseNoticeAppRoute({false, true, true, true, true, true}) == NoticeAppRoute::WeChatOpenChat &&
                ChooseNoticeAppRoute({false, false, true, true, true, false}) == NoticeAppRoute::ClickTrayIcon &&
                ChooseNoticeAppRoute({false, false, true, false, false, false}) == NoticeAppRoute::ClickTrayIcon &&
                ChooseNoticeAppRoute({false, false, true, false, true, false}) == NoticeAppRoute::ShowAppWindow &&
                ChooseNoticeAppRoute({false, false, false, false, true, true}) == NoticeAppRoute::ShowAppWindow &&
                ChooseNoticeAppRoute({false, false, false, false, false, true}) == NoticeAppRoute::ActivateAumid &&
                ChooseNoticeAppRoute({}) == NoticeAppRoute::ShellOpen &&
                ChooseNoticeAppRoute({true, false, true, false, true, true, true}) == NoticeAppRoute::OpenSystemNotificationCenter,
            "notice app routing");
    Require(ShouldOpenSystemNotificationCenter(L"windows", L"Microsoft.ScreenSketch_8wekyb3d8bbwe!App") &&
                ShouldOpenSystemNotificationCenter(L"windows", L"MicrosoftWindows.Client.CBS_cw5n1h2txyewy!ScreenClippingHost") &&
                !ShouldOpenSystemNotificationCenter(L"wechat", L"Microsoft.ScreenSketch_8wekyb3d8bbwe!App") &&
                !ShouldOpenSystemNotificationCenter(L"windows", L"Microsoft.WindowsStore_8wekyb3d8bbwe!App"),
            "#179: snipping tool Windows toasts route to Windows notification center");
}

void CheckToastDiff()
{
    const std::map<uint32_t, SeenToast> seen{{1, {10, true, L"QQ", L"QQ"}}, {2, {20, false, L"Mail", L"mail"}}, {3, {30, true, L"WeCom", L"wxwork.exe"}}};
    const std::map<uint32_t, SeenToast> now{{3, {30, true, L"WeCom", L"wxwork.exe"}}};
    auto never = [](const SeenToast&) { return false; };
    const auto normal = DiffGoneToasts(seen, now, false, false, never);
    const auto front = DiffGoneToasts(seen, now, false, true, [](const SeenToast& t) { return t.appId == L"QQ"; });
    Require(DiffGoneToasts(seen, now, true, false, never).empty() && DiffGoneToasts({}, now, false, false, never).empty() && normal.size() == 1 &&
                normal[0].source == L"windows" && normal[0].externalId == 1 && !normal[0].unread && DiffGoneToasts(seen, now, false, true, never).empty() &&
                front.size() == 1 && front[0].externalId == 1 && !front[0].unread,
            "toast diff");
}

} // namespace

int main()
{
    auto require = Require;
    require(MessengerOfProgram(L"D:\\Download App\\Tencent\\Weixin\\Weixin.exe") == Messenger::WeChat, "WeChat 4 by its program");
    require(MessengerOfProgram(L"C:\\Program Files (x86)\\Tencent\\TIM\\Bin\\TIM.exe") == Messenger::QQ, "TIM recognized as QQ");
    require(MessengerOfNotice(L"windows", L"TIM", L"TIM") == Messenger::QQ, "TIM notification recognized as QQ");
    require(MessengerOfProgram(L"c:\\program files\\wxwork\\WXWork.exe") == Messenger::WeCom, "WeCom by its program");
    require(MessengerOfProgram(L"D:\\Tencent\\WXWorkLocalPro\\WXWorkLocalPro.exe") == Messenger::WeCom, "WeCom private edition");
    require(MessengerOfProgram(L"DingTalk.exe") == Messenger::DingTalk, "DingTalk by its file name");
    require(MessengerOfProgram(L"C:\\Windows\\explorer.exe") == Messenger::None, "other programs are none of them");
    require(MessengerOfNotice(L"windows", L"QQ", L"QQ") == Messenger::QQ, "QQ's Windows notification (AUMID QQ)");
    require(MessengerOfNotice(L"windows", L"\u9489\u9489", L"") == Messenger::DingTalk, "a notification by its shown name");
    require(MessengerOfNotice(L"wechat", L"WeChat", L"") == Messenger::WeChat, "the WeChat reader's card");
    require(MessengerOfNotice(L"windows", L"Mail", L"microsoft.windowscommunicationsapps_8wekyb3d8bbwe!microsoft.windowslive.mail") == Messenger::None,
            "other apps' notifications follow notifyListener");
    require(MessengerOfNotice(L"windows", L"QQ Music", L"QQMusic") == Messenger::None, "a name only containing QQ is not QQ");

    MessengerAlerts alerts;
    require(alerts.Flash(Messenger::WeCom, 1000), "a first flash begins a burst");
    require(!alerts.Flash(Messenger::WeCom, 1500), "flashes within a burst add nothing");
    require(alerts.Due(3000).empty(), "nothing is due before the wait");
    auto due = alerts.Due(4000);
    require(due.size() == 1 && due[0] == Messenger::WeCom, "a burst with no card gets a plain one");
    require(alerts.Due(9000).empty(), "once");
    require(!alerts.Flash(Messenger::WeCom, 9000), "a flash soon after the last is the same burst");
    require(alerts.Flash(Messenger::WeCom, 30000), "after a quiet spell a new burst");
    alerts.Noticed(Messenger::WeCom, 31000);
    require(alerts.Due(34000).empty(), "a card with the text follows: no plain card");
    alerts.Noticed(Messenger::QQ, 50000);
    require(alerts.Flash(Messenger::QQ, 52000), "a QQ burst");
    require(alerts.Due(56000).empty(), "a card just before the flash counts too");
    bool waiting = false;
    require(alerts.Flash(Messenger::WeChat, 60000) && alerts.Due(61000, &waiting).empty() && waiting, "still waiting");
    alerts.Cancel(Messenger::WeChat);
    require(alerts.Due(65000, &waiting).empty() && !waiting, "cancelled burst produces no plain card");
    require(alerts.Flash(Messenger::WeChat, 66000), "flash after cancel begins a new burst");

    // Multi-instance isolation (two WeChat/QQ PIDs).
    MessengerAlerts multi;
    require(multi.Flash(Messenger::QQ, 1000, 101) && multi.Flash(Messenger::QQ, 1100, 202), "two QQ PIDs begin independent flash bursts");
    multi.Noticed(Messenger::QQ, 1200, 101);
    multi.Cancel(Messenger::WeChat, 303);
    const auto dueMulti = multi.DueInstances(5000);
    require(dueMulti.size() == 1 && dueMulti[0] == (MessengerAlerts::DueAlert{Messenger::QQ, 202}), "noticed/cancel on PID 101 does not suppress PID 202");
    require(multi.Flash(Messenger::WeChat, 10000, 111) && multi.Flash(Messenger::WeChat, 10100, 222), "two WeChat PIDs begin independent flash bursts");
    multi.Cancel(Messenger::WeChat, 111);
    const auto dueWeChat = multi.DueInstances(14000);
    require(dueWeChat.size() == 1 && dueWeChat[0] == (MessengerAlerts::DueAlert{Messenger::WeChat, 222}),
            "cancelling one WeChat PID keeps sibling PID pending");

    Notice cardPid1{.source = L"wechat", .title = L"文件传输助手", .pid = 111};
    Notice cardPid2{.source = L"wechat", .title = L"文件传输助手", .pid = 222};
    const Notice withdrawPid1{.source = L"wechat", .title = L"文件传输助手", .unread = false, .pid = 111};
    require(notification::WithdrawNotice(cardPid1, withdrawPid1), "matching PID withdraws notice");
    require(!notification::WithdrawNotice(cardPid2, withdrawPid1), "mismatched PID must not withdraw sibling account notice");

    CheckWeChatTime();
    CheckWeChatRows();
    CheckWeChatDiff();
    CheckWeChatThrottle();
    CheckNoticeRouting();
    CheckToastDiff();
    if (g_failed)
        return 1;
    std::cout << "messengers: all checks passed\n";
    return 0;
}
