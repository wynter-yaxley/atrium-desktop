// WeChat (Weixin) messages as Atrium cards. WeChat does not publish Windows
// notifications, but its main window's chat list is readable through UI
// Automation, read only: each row (mmui::ChatSessionCell) names the chat, the
// unread count "[N条]", the newest message's summary and whether the chat is
// muted ("消息免打扰"). Name/structure/window events wake an MTA reader;
// a two-second reconciliation covers providers that omit events.
// The first read is the baseline, then a rising unread count in a chat that
// is not muted posts a card. Only the rows WeChat has realised (about ten,
// the top of the list, where new messages land) are seen; nothing in WeChat
// is changed, no accessibility switches are turned on (D29).
// Closed to the tray, the window's rows drop out of the tree; the rows held from
// when it was last shown still read, so cards keep coming. Before the window was
// ever shown, WeChat's flash for a message brings a plain card (shell/messengers.h);
// while the list reads, its cards are the only ones (a flash comes before the list
// shows the message, and WeChat flashes again while messages stay unread).
#pragma once

#include "notice.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct IUIAutomation;
struct IUIAutomationCacheRequest;
struct IUIAutomationCondition;

namespace atrium {

struct WeChatRow {
    std::wstring chat;    // the chat's name
    std::wstring summary; // the newest message as the list shows it
    int unread = 0;
    bool muted = false;
    bool collapsed = false;
    bool official = false; // the official accounts' row (公众号, 订阅号, 服务号): never in the badge; wechatBlockOfficial governs the cards
};

struct WeChatChatState {
    int unread = 0;
    std::wstring summary;
};

bool IsWeChatTime(const std::wstring& s);
bool IsCollapsedWeChatRow(const std::wstring& chat);
// WeChat lists its official accounts' messages in rows of their own name (its UI Automation names a row
// by its chat's shown name only, so a service account listed as a chat of its own is not told apart).
bool IsOfficialWeChatRow(const std::wstring& chat);
WeChatRow ParseWeChatRow(const std::wstring& name, const std::wstring& fallbackText = {}, const std::wstring& itemStatus = {});
bool MatchesWeChatCellName(const std::wstring& cellName, const std::wstring& chat);
std::vector<Notice> DiffWeChatRows(const std::vector<std::pair<std::wstring, WeChatRow>>& rows, bool baseline, bool blockOfficial,
                                   std::map<std::wstring, WeChatChatState>& states, bool allowOpenSummary = true);
// The badge counts chats only: the official accounts' bulk unread (a service account's 349) never
// becomes the taskbar's; wechatBlockOfficial governs the cards, not this sum.
int SumWeChatUnread(const std::vector<std::pair<std::wstring, WeChatRow>>& rows);

enum class WeChatBusyState { None, Foreground, ForegroundShown, Dialog };

struct WeChatThrottleInput {
    WeChatBusyState busy = WeChatBusyState::None;
    bool hasHeld = false;
    int unread = 0;
    ULONGLONG tick = 0;
    ULONGLONG lastRead = 0;
    ULONGLONG busyUntilTick = 0;
    bool sampledWhileBusy = false;
    bool flashWake = false;
    ULONGLONG followUpTick = 0;
    bool breakCooldown = false;
};

bool ShouldBreakWeChatCooldown(WeChatBusyState prev, WeChatBusyState now);

struct WeChatThrottleOutput {
    DWORD waitMs = 0;
    bool requeueFlashWake = false;
    // Retained only for throttle-contract tests; PollWeChatInstance preserves reading/baseline (#190).
    bool clearReadingAndBaseline = false;
    ULONGLONG nextBusyUntilTick = 0;
    bool nextSampledWhileBusy = false;
    ULONGLONG nextFollowUpTick = 0;
};

WeChatThrottleOutput EvaluateWeChatThrottle(const WeChatThrottleInput& in);

class WeChatSource {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the
    // receiver deletes it), and `badgeMsg` (if non-zero) when Unread() changes.
    WeChatSource(HWND notify, UINT msg, UINT badgeMsg = 0);
    ~WeChatSource();
    WeChatSource(const WeChatSource&) = delete;
    WeChatSource& operator=(const WeChatSource&) = delete;
    // WeChat flashed for a message: read its chat list now.
    void Wake()
    {
        wakePending_ = true;
        SetEvent(wake_);
    }
    // The chat list reads (a shown window's, or the rows held from it).
    bool Reading() const { return reading_; }
    bool ReadingPid(DWORD pid) const
    {
        if (pid == 0)
            return reading_;
        std::lock_guard lock(readingMu_);
        return readingPids_.contains(pid);
    }
    int Unread() const { return unread_; }
    // wechatBlockOfficial: no cards or badge count for the official accounts' rows.
    void SetBlockOfficial(bool on)
    {
        if (blockOfficial_.exchange(on) != on && wake_)
            SetEvent(wake_);
    }
    void SetPostCards(bool on) { postCards_ = on; }
    void OpenChat(const std::wstring& chat, DWORD pid = 0);

private:
    struct LoopState;
    void Run();
    void PublishStatus(std::set<DWORD> activeReading, int totalUnread);
    DWORD StepInstances(IUIAutomation* uia, IUIAutomationCondition* cells, IUIAutomationCacheRequest* cache, LoopState& loop, ULONGLONG tick);
    HWND notify_;
    UINT msg_;
    UINT badgeMsg_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reading_{false};
    std::atomic<bool> blockOfficial_{false};
    std::atomic<bool> postCards_{true};
    std::atomic<int> unread_{0};
    std::atomic<bool> wakePending_{false};
    mutable std::mutex readingMu_;
    std::set<DWORD> readingPids_;
    std::mutex openMu_;
    std::wstring pendingOpenChat_;
    DWORD pendingOpenPid_ = 0;
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

namespace detail {
WeChatBusyState QueryWeChatBusyStateForPid(DWORD pid);
WeChatBusyState QueryWeChatBusyState(const std::vector<std::pair<DWORD, std::wstring>>& procs);
bool ShouldReadWeChatWindow(HWND h, DWORD pid, bool* hung = nullptr);
std::vector<HWND> WeChatWindows(DWORD pid, bool& anyHung);
void DiscoverWeChatProcesses(std::vector<std::pair<DWORD, std::wstring>>& procs);
void HandlePendingWeChatOpen(IUIAutomation* uia, IUIAutomationCondition* cells, HANDLE wake, const std::atomic<bool>& stop, std::mutex& openMu,
                             std::wstring& pendingOpenChat, DWORD& pendingOpenPid, std::vector<std::pair<DWORD, std::wstring>>& procs,
                             std::map<DWORD, HWND>& lastMainByPid, uint32_t& loggedMask);
} // namespace detail

} // namespace atrium
