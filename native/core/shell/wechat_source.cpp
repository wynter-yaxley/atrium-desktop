#include "shell/wechat_source.h"

#include "localization.h"

#include "log.h"
#include "shell/uia_client.h"
#include "../posted.h"
#include "win32_path.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium {

namespace {

struct HeldRow {
    winrt::com_ptr<IUIAutomationElement> element;
    int failures = 0;
};

std::wstring Trim(std::wstring s)
{
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    size_t a = s.find_first_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a);
}

bool IsDigits(const std::wstring& s)
{
    return !s.empty() && std::ranges::all_of(s, [](wchar_t c) { return iswdigit(c) != 0; });
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// A line that marks a muted chat: 消息免打扰 (or a variant) as a word of its own, as WeChat may put it beside
// the time; the English marks only as the whole line. Never a substring: a summary may well say 免打扰.
bool IsMutedText(const std::wstring& line)
{
    constexpr std::wstring_view kWords[] = {L"消息免打扰", L"免打扰", L"訊息免打擾", L"免打擾"};
    constexpr const wchar_t* kLines[] = {L"Mute Notifications", L"Notifications Muted", L"Muted"};
    const std::wstring t = Trim(line);
    if (std::ranges::any_of(kLines, [&](const wchar_t* m) { return _wcsicmp(t.c_str(), m) == 0; }))
        return true;
    for (size_t from = 0; from < t.size();) {
        const size_t end = std::min(t.find(L' ', from), t.size());
        const std::wstring_view word(t.data() + from, end - from);
        if (std::ranges::find(kWords, word) != std::end(kWords))
            return true;
        from = end + 1;
    }
    return false;
}

// UIA's ItemStatus is the cell's own state, not message text: the marker anywhere in it.
bool ContainsMuteStatus(const std::wstring& status)
{
    return status.find(L"\u514d\u6253\u6270") != std::wstring::npos || status.find(L"\u514d\u6253\u64fe") != std::wstring::npos ||
           FoldCase(status).find(L"mute") != std::wstring::npos;
}

bool IsPinLine(const std::wstring& line) { return line == L"已置顶" || _wcsicmp(line.c_str(), L"Pinned") == 0; }

int ParseUnreadTag(std::wstring tag)
{
    tag = Trim(std::move(tag));
    constexpr const wchar_t* kSuffixes[] = {L"条", L" unread", L" messages", L" message"};
    std::wstring num;
    for (const wchar_t* suffix : kSuffixes) {
        if (EndsWithNoCase(tag, suffix)) {
            num = Trim(tag.substr(0, tag.size() - wcslen(suffix)));
            break;
        }
    }
    if (num.empty())
        return 0;
    if (num.back() == L'+')
        num.pop_back();
    if (!IsDigits(num))
        return 0;
    return _wtoi(num.c_str());
}

bool ParseUnreadPrefix(const std::wstring& line, int& unread, std::wstring& rest)
{
    if (line.size() < 4 || line[0] != L'[')
        return false;
    const size_t close = line.find(L']');
    if (close == std::wstring::npos || close <= 1)
        return false;
    const int count = ParseUnreadTag(line.substr(1, close - 1));
    if (count > 0) {
        unread = count;
        rest = Trim(line.substr(close + 1));
        return true;
    }
    // Badge prefixes like [有人@我] can precede [N条] on the same line.
    const std::wstring tail = Trim(line.substr(close + 1));
    if (tail.size() >= 4 && tail[0] == L'[')
        return ParseUnreadPrefix(tail, unread, rest);
    return false;
}

bool IsAmPmTime(const std::wstring& t)
{
    if (t.size() < 4 || (!EndsWithNoCase(t, L"AM") && !EndsWithNoCase(t, L"PM")))
        return false;
    const std::wstring head = Trim(t.substr(0, t.size() - 2));
    return head.find(L':') != std::wstring::npos && std::ranges::all_of(head, [](wchar_t c) { return iswdigit(c) || c == L':'; });
}

bool IsCjkDate(const std::wstring& t)
{
    if (!t.ends_with(L"日") || t.find(L'月') == std::wstring::npos)
        return false;
    bool hasDigit = false;
    for (wchar_t c : t) {
        if (iswdigit(c))
            hasDigit = true;
        else if (c != L'年' && c != L'月' && c != L'日')
            return false;
    }
    return hasDigit;
}

DWORD WindowPid(HWND h)
{
    if (!h)
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

bool IsWeChatPid(const std::vector<std::pair<DWORD, std::wstring>>& procs, DWORD pid)
{
    return pid != 0 && std::ranges::any_of(procs, [pid](const auto& p) { return p.first == pid; });
}

} // namespace

namespace detail {

WeChatBusyState QueryWeChatBusyState(const std::vector<std::pair<DWORD, std::wstring>>& procs)
{
    struct Ctx {
        const std::vector<std::pair<DWORD, std::wstring>>& procs;
        bool dialog = false, hasShownEnabledMain = false;
    } ctx{procs};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            if (!IsWindowVisible(h))
                return TRUE;
            const bool ownPid = IsWeChatPid(c->procs, WindowPid(h));
            HWND owner = GetWindow(h, GW_OWNER);
            wchar_t cls[96]{};
            GetClassNameW(h, cls, 96);
            if (owner && (ownPid || IsWeChatPid(c->procs, WindowPid(owner)))) {
                if (wcscmp(cls, L"#32770") == 0 || !IsWindowEnabled(owner) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_DLGMODALFRAME) != 0)
                    return c->dialog = true, FALSE;
            } else if (ownPid && wcsstr(cls, L"QWindowIcon")) {
                if (!IsWindowEnabled(h))
                    return c->dialog = true, FALSE;
                c->hasShownEnabledMain = true;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.dialog)
        return WeChatBusyState::Dialog;
    if (HWND fg = GetForegroundWindow(); fg && (IsWeChatPid(procs, WindowPid(fg)) || IsWeChatPid(procs, WindowPid(GetWindow(fg, GW_OWNER))) ||
                                                IsWeChatPid(procs, WindowPid(GetAncestor(fg, GA_ROOTOWNER)))))
        return ctx.hasShownEnabledMain ? WeChatBusyState::ForegroundShown : WeChatBusyState::Foreground;
    return WeChatBusyState::None;
}

bool ShouldReadWeChatWindow(HWND h, DWORD pid, bool* hung)
{
    if (WindowPid(h) != pid || GetWindow(h, GW_OWNER) != nullptr)
        return false;
    if ((GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0 || !IsWindowEnabled(h))
        return false;
    wchar_t cls[96]{};
    GetClassNameW(h, cls, 96);
    if (!wcsstr(cls, L"QWindowIcon"))
        return false;
    if (IsHungAppWindow(h)) {
        if (hung)
            *hung = true;
        return false;
    }
    return true;
}

// WeChat's top-level Qt main windows (shown, minimised, or hidden in the tray).
std::vector<HWND> WeChatWindows(DWORD pid, bool& anyHung)
{
    struct Ctx {
        DWORD pid;
        bool& anyHung;
        std::vector<HWND> out;
    } ctx{pid, anyHung, {}};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            if (ShouldReadWeChatWindow(h, c->pid, &c->anyHung))
                c->out.push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.out;
}

void DiscoverWeChatProcesses(std::vector<std::pair<DWORD, std::wstring>>& procs)
{
    procs.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, L"Weixin.exe") == 0 || _wcsicmp(pe.szExeFile, L"WeChat.exe") == 0)
            procs.emplace_back(pe.th32ProcessID, ProcessImagePath(pe.th32ProcessID));
    if (snap != INVALID_HANDLE_VALUE)
        CloseHandle(snap);
}

} // namespace detail

std::vector<Notice> DiffWeChatRows(const std::vector<std::pair<std::wstring, WeChatRow>>& rows, bool baseline, bool blockOfficial,
                                   std::map<std::wstring, WeChatChatState>& states)
{
    std::vector<Notice> events;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& [key, r] = rows[i];
        auto old = states.find(key);
        const int oldUnread = old != states.end() ? old->second.unread : 0;
        const bool suppressed = r.muted || r.collapsed || (blockOfficial && r.official);
        // A chat not seen before counts only at the top of the list,
        // where a new message puts it (scrolling brings old ones in).
        const bool countRose = old != states.end() ? r.unread > oldUnread : (r.unread > 0 && i < 3);
        const bool saturatedRose = old != states.end() && r.unread >= 99 && oldUnread >= 99 && !r.summary.empty() && r.summary != old->second.summary;
        const bool rose = baseline && !suppressed && (countRose || saturatedRose);
        // Not gated on baseline: WeChat in front clears it, and reading happens there (#79).
        const bool read = old != states.end() && oldUnread > 0 && r.unread == 0;
        if (rose || read) {
            Notice ev;
            ev.source = L"wechat";
            ev.title = r.chat;
            ev.body = r.summary;
            ev.unread = rose;
            events.push_back(std::move(ev));
        }
        states[key] = WeChatChatState{r.unread, r.summary};
    }
    if (states.size() > 512) {
        std::erase_if(states, [&](const auto& kv) { return std::ranges::none_of(rows, [&](const auto& row) { return row.first == kv.first; }); });
        while (states.size() > 512)
            states.erase(states.begin());
    }
    return events;
}

int SumWeChatUnread(const std::vector<std::pair<std::wstring, WeChatRow>>& rows)
{
    int total = 0;
    // Official accounts publish in bulk (a service account may hold 349 at once): that is not the
    // user's mail, so the badge never counts them however the card switch is set.
    for (const auto& [key, r] : rows) {
        if (!r.muted && !r.collapsed && !r.official && r.unread > 0)
            total += r.unread;
    }
    return total;
}

namespace {

void UpdateUnread(const std::vector<std::pair<std::wstring, WeChatRow>>& rows, bool baseline, bool blockOfficial, bool postCards, const std::wstring& exe,
                  HWND notify, UINT msg, std::map<std::wstring, WeChatChatState>& states)
{
    for (Notice& ev : DiffWeChatRows(rows, baseline, blockOfficial, states)) {
        if (!postCards)
            continue;
        auto n = std::make_unique<Notice>(std::move(ev));
        if (n->unread) {
            n->app = Tr(L"WeChat");
            n->appId = exe;
            if (n->body.empty())
                n->body = Tr(L"New message");
            if (!exe.empty())
                n->action = L"app:" + exe;
        }
        const bool rose = n->unread;
        if (PostOwned(notify, msg, 0, std::move(n)) && rose)
            Log(L"wechat: new message");
    }
}

void ReadRow(IUIAutomationElement* e, std::vector<std::pair<std::wstring, WeChatRow>>& rows)
{
    if (!e)
        return;
    BSTR id = nullptr, name = nullptr;
    if (SUCCEEDED(e->get_CachedAutomationId(&id)) && SUCCEEDED(e->get_CachedName(&name)) && name) {
        std::wstring fallback;
        VARIANT val{};
        if (SUCCEEDED(e->GetCachedPropertyValue(UIA_ValueValuePropertyId, &val))) {
            if (val.vt == VT_BSTR && val.bstrVal && *val.bstrVal)
                fallback = val.bstrVal;
            VariantClear(&val);
        }
        if (fallback.empty()) {
            BSTR help = nullptr;
            if (SUCCEEDED(e->get_CachedHelpText(&help)) && help && *help)
                fallback = help;
            SysFreeString(help);
        }
        std::wstring itemStatus;
        BSTR status = nullptr;
        if (SUCCEEDED(e->get_CachedItemStatus(&status)) && status && *status)
            itemStatus = status;
        SysFreeString(status);
        WeChatRow r = ParseWeChatRow(name, fallback, itemStatus);
        std::wstring key = id && *id ? id : r.chat;
        if (!r.chat.empty())
            rows.emplace_back(std::move(key), std::move(r));
    }
    SysFreeString(id);
    SysFreeString(name);
}

bool ReadLiveWindow(IUIAutomation* uia, IUIAutomationCondition* cells, IUIAutomationCacheRequest* cache, HWND h, const std::wstring& program,
                    std::vector<std::pair<std::wstring, WeChatRow>>& rows, std::vector<HeldRow>& held, std::wstring& heldProgram, std::wstring& exe,
                    HWND& lastMainHwnd)
{
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::com_ptr<IUIAutomationElementArray> all;
    if (FAILED(uia->ElementFromHandle(h, root.put())) || !root || FAILED(root->FindAllBuildCache(TreeScope_Descendants, cells, cache, all.put())) || !all)
        return false;
    int n = 0;
    all->get_Length(&n);
    std::vector<HeldRow> elements;
    for (int i = 0; i < n; ++i) {
        winrt::com_ptr<IUIAutomationElement> e;
        all->GetElement(i, e.put());
        ReadRow(e.get(), rows);
        if (e)
            elements.push_back({std::move(e), 0});
    }
    if (n == 0)
        return false;
    held = std::move(elements);
    heldProgram = exe = program;
    lastMainHwnd = h;
    return true;
}

bool ReadLiveWindows(IUIAutomation* uia, IUIAutomationCondition* cells, IUIAutomationCacheRequest* cache,
                     const std::vector<std::pair<DWORD, std::wstring>>& procs, std::vector<std::pair<std::wstring, WeChatRow>>& rows,
                     std::vector<HeldRow>& held, std::wstring& heldProgram, std::wstring& exe, HWND& lastMainHwnd, bool& anyHung)
{
    if (lastMainHwnd) {
        auto it = std::ranges::find_if(procs, [pid = WindowPid(lastMainHwnd)](const auto& p) { return p.first == pid; });
        if (it != procs.end() && detail::ShouldReadWeChatWindow(lastMainHwnd, it->first, &anyHung) &&
            ReadLiveWindow(uia, cells, cache, lastMainHwnd, it->second, rows, held, heldProgram, exe, lastMainHwnd))
            return true;
    }
    for (const auto& [p, program] : procs)
        for (HWND h : detail::WeChatWindows(p, anyHung))
            if (h != lastMainHwnd && ReadLiveWindow(uia, cells, cache, h, program, rows, held, heldProgram, exe, lastMainHwnd))
                return true;
    return false;
}

bool ReadHeldRows(IUIAutomationCacheRequest* cache, std::vector<HeldRow>& held, std::vector<std::pair<std::wstring, WeChatRow>>& rows)
{
    for (size_t i = 0; i < held.size();) {
        winrt::com_ptr<IUIAutomationElement> fresh;
        const HRESULT hr = held[i].element->BuildUpdatedCache(cache, fresh.put());
        if (SUCCEEDED(hr) && fresh) {
            held[i++].failures = 0;
            ReadRow(fresh.get(), rows);
        } else if (hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) || ++held[i].failures >= 3) {
            held.erase(held.begin() + ptrdiff_t(i)); // gone (WeChat restarted, the list rebuilt)
        } else {
            ++i;
        }
    }
    return !rows.empty();
}

} // namespace

bool IsWeChatTime(const std::wstring& s)
{
    const std::wstring t = Trim(s);
    if (t.empty() || t.size() > 24)
        return false;
    if (t == L"刚刚" || t == L"昨天" || t == L"前天" || t.starts_with(L"星期") || _wcsicmp(t.c_str(), L"Yesterday") == 0)
        return true;
    if (t.ends_with(L"分钟前") && t.size() > 3 && IsDigits(t.substr(0, t.size() - 3)))
        return true;
    if (t.ends_with(L"小时前") && t.size() > 3 && IsDigits(t.substr(0, t.size() - 3)))
        return true;
    if (IsAmPmTime(t) || IsCjkDate(t))
        return true;
    return std::ranges::any_of(t, [](wchar_t c) { return iswdigit(c) != 0; }) &&
           std::ranges::any_of(t, [](wchar_t c) { return c == L':' || c == L'/' || c == L'-'; }) &&
           std::ranges::all_of(t, [](wchar_t c) { return iswdigit(c) || c == L':' || c == L'/' || c == L'-'; });
}

bool IsCollapsedWeChatRow(const std::wstring& chat)
{
    const std::wstring t = Trim(chat);
    return t == L"折叠的群聊" || t == L"折叠置顶聊天" || t == L"折叠的聊天" || _wcsicmp(t.c_str(), L"Collapsed Chats") == 0 ||
           _wcsicmp(t.c_str(), L"Folded Group Chats") == 0;
}

bool IsOfficialWeChatRow(const std::wstring& chat)
{
    const std::wstring t = Trim(chat);
    constexpr const wchar_t* kNames[] = {
        L"公众号", L"订阅号", L"订阅号消息", L"服务号", L"服务号消息", L"Official Accounts", L"Subscriptions", L"Subscription Accounts", L"Service Accounts"};
    return std::ranges::any_of(kNames, [&](const wchar_t* n) { return _wcsicmp(t.c_str(), n) == 0; });
}

// A row's name, line by line: the chat; optional tag/pin lines; "[N条] summary" or
// "[N条]" and the summary on the following line, or only the summary when read; the time;
// and "消息免打扰" for a muted chat.
WeChatRow ParseWeChatRow(const std::wstring& name, const std::wstring& fallbackText, const std::wstring& itemStatus)
{
    std::vector<std::wstring> lines;
    size_t from = 0;
    for (;;) {
        const size_t nl = name.find(L'\n', from);
        lines.push_back(Trim(name.substr(from, nl == std::wstring::npos ? std::wstring::npos : nl - from)));
        if (nl == std::wstring::npos)
            break;
        from = nl + 1;
    }
    WeChatRow r;
    if (lines.empty())
        return r;
    r.chat = lines[0];
    r.collapsed = IsCollapsedWeChatRow(r.chat);
    r.official = IsOfficialWeChatRow(r.chat);
    r.muted = r.collapsed || IsMutedText(fallbackText) || ContainsMuteStatus(itemStatus);
    bool sawUnread = false;
    bool summaryAfterUnread = false;
    std::wstring preTag;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        if (l.empty() || IsPinLine(l))
            continue;
        if (IsMutedText(l)) {
            r.muted = true;
            continue;
        }
        int count = 0;
        std::wstring rest;
        if (r.unread == 0 && ParseUnreadPrefix(l, count, rest)) {
            r.unread = count;
            sawUnread = true;
            if (!r.summary.empty() && !summaryAfterUnread) {
                preTag = std::move(r.summary);
                r.summary.clear();
            }
            if (!rest.empty() && !IsWeChatTime(rest)) {
                r.summary = std::move(rest);
                summaryAfterUnread = true;
            }
            continue;
        }
        if (!IsWeChatTime(l)) {
            if (sawUnread && !summaryAfterUnread) {
                r.summary = l;
                summaryAfterUnread = true;
            } else if (r.summary.empty()) {
                r.summary = l;
            }
        }
    }
    if (r.summary.empty()) {
        const std::wstring fb = Trim(fallbackText);
        if (!fb.empty() && fb != r.chat && !IsWeChatTime(fb) && !IsMutedText(fb) && !IsPinLine(fb))
            r.summary = fb;
        else if (!preTag.empty())
            r.summary = std::move(preTag);
    }
    return r;
}

WeChatSource::WeChatSource(HWND notify, UINT msg, UINT badgeMsg)
    : notify_(notify),
      msg_(msg),
      badgeMsg_(badgeMsg),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
{
    thread_ = std::thread([this] { Run(); });
}

WeChatSource::~WeChatSource()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

bool ShouldBreakWeChatCooldown(WeChatBusyState prev, WeChatBusyState now)
{
    return (prev == WeChatBusyState::None && now == WeChatBusyState::ForegroundShown) ||
           ((prev == WeChatBusyState::ForegroundShown || prev == WeChatBusyState::Foreground) && now == WeChatBusyState::None);
}

WeChatThrottleOutput EvaluateWeChatThrottle(const WeChatThrottleInput& in)
{
    constexpr ULONGLONG kForegroundUnreadGapMs = 2000;
    constexpr DWORD kBusyWaitMs = 1000;

    WeChatThrottleOutput out{};
    out.nextBusyUntilTick = in.busyUntilTick;
    out.nextSampledWhileBusy = in.sampledWhileBusy;
    out.nextFollowUpTick = in.followUpTick;

    if (in.breakCooldown && (in.busy == WeChatBusyState::ForegroundShown || in.busy == WeChatBusyState::None)) {
        if (in.busy == WeChatBusyState::ForegroundShown) {
            out.nextSampledWhileBusy = true;
            out.nextBusyUntilTick = in.tick + 300;
            out.clearReadingAndBaseline = true;
        }
        out.nextFollowUpTick = 0;
        return out;
    }

    const bool initialSample = in.busy == WeChatBusyState::ForegroundShown && !in.hasHeld && !in.sampledWhileBusy;
    const bool foregroundUnreadSample = in.busy == WeChatBusyState::ForegroundShown && in.unread > 0;
    if (in.busy == WeChatBusyState::None)
        out.nextSampledWhileBusy = false;
    else if (initialSample)
        out.nextSampledWhileBusy = true;

    if (foregroundUnreadSample && !initialSample) {
        out.nextBusyUntilTick = in.tick + 300;
        out.clearReadingAndBaseline = true;
        out.nextFollowUpTick = 0;
        if (in.lastRead && in.tick - in.lastRead < kForegroundUnreadGapMs)
            out.waitMs = DWORD(std::min<ULONGLONG>(kBusyWaitMs, kForegroundUnreadGapMs - (in.tick - in.lastRead)));
        return out;
    }

    if ((in.busy != WeChatBusyState::None && !initialSample) || (!initialSample && in.tick < in.busyUntilTick)) {
        if (in.busy != WeChatBusyState::None)
            out.nextBusyUntilTick = in.tick + 300;
        out.clearReadingAndBaseline = true;
        out.nextFollowUpTick = 0;
        out.waitMs = in.busy != WeChatBusyState::None ? kBusyWaitMs : DWORD(std::min<ULONGLONG>(kBusyWaitMs, in.busyUntilTick - in.tick));
        return out;
    }

    const DWORD minGap = (in.flashWake || (in.followUpTick && in.tick >= in.followUpTick)) ? 200 : 1000;
    if (in.followUpTick && in.tick >= in.followUpTick)
        out.nextFollowUpTick = 0;
    if (in.lastRead && in.tick - in.lastRead < minGap) {
        out.requeueFlashWake = in.flashWake;
        out.waitMs = DWORD(minGap - (in.tick - in.lastRead));
        return out;
    }
    if (in.flashWake)
        out.nextFollowUpTick = in.tick + 450;
    return out;
}

namespace {

DWORD WeChatPollWait(int state, ULONGLONG followUpTick)
{
    const DWORD waitMs = state >= 2 ? 2000 : (state == 1 ? 10000 : 6000);
    return followUpTick ? std::min(waitMs, GetTickCount64() < followUpTick ? DWORD(followUpTick - GetTickCount64()) : 0) : waitMs;
}

DWORD WeChatBusyOrThrottleWait(const std::vector<std::pair<DWORD, std::wstring>>& procs, bool hasHeld, int unread, ULONGLONG tick, ULONGLONG& busyUntilTick,
                               ULONGLONG lastRead, ULONGLONG& followUpTick, bool& sampledWhileBusy, std::atomic<bool>& reading, std::atomic<bool>& wakePending,
                               bool& baseline, WeChatBusyState& prevBusy)
{
    const WeChatBusyState busy = detail::QueryWeChatBusyState(procs);
    const bool breakCooldown = ShouldBreakWeChatCooldown(prevBusy, busy);
    prevBusy = busy;
    const WeChatThrottleInput input{busy,         hasHeld,      unread, tick, lastRead, busyUntilTick, sampledWhileBusy, wakePending.exchange(false),
                                    followUpTick, breakCooldown};
    const WeChatThrottleOutput out = EvaluateWeChatThrottle(input);
    busyUntilTick = out.nextBusyUntilTick;
    sampledWhileBusy = out.nextSampledWhileBusy;
    followUpTick = out.nextFollowUpTick;
    if (out.clearReadingAndBaseline)
        reading = baseline = false;
    if (out.requeueFlashWake)
        wakePending = true;
    return out.waitMs;
}

void ResetUnreadState(std::map<std::wstring, WeChatChatState>& states, std::atomic<int>& unread, HWND notify, UINT badgeMsg)
{
    states.clear();
    if (unread.exchange(0) != 0 && badgeMsg)
        PostMessageW(notify, badgeMsg, 0, 0);
}

} // namespace

void WeChatSource::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    winrt::com_ptr<IUIAutomationCondition> cells;
    winrt::com_ptr<IUIAutomationCacheRequest> cache;
    winrt::com_ptr<IUIAutomation> uia = CreateUia(stop_, wake_, L"wechat", [&](IUIAutomation* u) {
        cells = nullptr;
        VARIANT v{};
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(L"mmui::ChatSessionCell");
        HRESULT hr = u->CreatePropertyCondition(UIA_ClassNamePropertyId, v, cells.put());
        VariantClear(&v);
        if (FAILED(hr))
            return hr;
        return CreateRawCache(u, {UIA_AutomationIdPropertyId, UIA_NamePropertyId, UIA_ValueValuePropertyId, UIA_HelpTextPropertyId, UIA_ItemStatusPropertyId},
                              cache, true);
    });
    std::map<std::wstring, WeChatChatState> states;
    bool baseline = false;
    int state = -1; // 0 no WeChat, 1 no list, 2 reading, 3 reading the held rows (logged on change)
    auto report = [&](int now, const wchar_t* text) {
        if (now != state)
            Log(L"wechat: %s", text);
        state = now;
        reading_ = false;
    };
    std::vector<std::pair<DWORD, std::wstring>> procs;
    ULONGLONG listed = 0;
    ULONGLONG busyUntilTick = 0;
    ULONGLONG lastRead = 0;
    ULONGLONG followUpTick = 0;
    bool sampledWhileBusy = false;
    WeChatBusyState prevBusy = WeChatBusyState::None;
    uint32_t openLoggedMask = 0;
    HWND lastMainHwnd = nullptr;
    // The chat rows last found in a shown window. Closed to the tray, WeChat's window leaves its
    // rows out of the tree (Qt skips hidden controls), but rows already held still read via
    // BuildUpdatedCache so messages are seen there too.
    std::vector<HeldRow> held;
    std::wstring heldProgram;
    {
        UiaWindowWake windows(wake_, L"QWindowIcon", false);
        while (!stop_ && uia && cells) {
            try {
                const ULONGLONG tick = GetTickCount64();
                if (!listed || tick - listed >= 10000 || (state < 2 && tick - listed >= 2000)) {
                    detail::DiscoverWeChatProcesses(procs);
                    listed = tick;
                    windows.SetProcesses(procs);
                }
                detail::HandlePendingWeChatOpen(uia.get(), cells.get(), wake_, stop_, openMu_, pendingOpenChat_, procs, lastMainHwnd, openLoggedMask);
                if (procs.empty()) {
                    busyUntilTick = 0;
                    lastRead = 0;
                    followUpTick = 0;
                    sampledWhileBusy = wakePending_ = false;
                    prevBusy = WeChatBusyState::None;
                    lastMainHwnd = nullptr;
                    report(0, L"WeChat is not running");
                    held.clear();
                    baseline = false; // after a restart its list is a new baseline
                    ResetUnreadState(states, unread_, notify_, badgeMsg_);
                    WaitForUiaWake(wake_, 6000);
                    continue;
                }
                if (const DWORD skipMs = WeChatBusyOrThrottleWait(procs, !held.empty(), unread_.load(), tick, busyUntilTick, lastRead, followUpTick,
                                                                  sampledWhileBusy, reading_, wakePending_, baseline, prevBusy)) {
                    WaitForUiaWake(wake_, skipMs);
                    continue;
                }
                lastRead = tick;
                std::wstring exe;
                std::vector<std::pair<std::wstring, WeChatRow>> rows;
                bool anyHung = false;
                bool found = ReadLiveWindows(uia.get(), cells.get(), cache.get(), procs, rows, held, heldProgram, exe, lastMainHwnd, anyHung);
                bool fromHeld = false;
                if (!found && !anyHung && !held.empty()) {
                    found = fromHeld = ReadHeldRows(cache.get(), held, rows);
                    if (found) {
                        exe = heldProgram;
                        if (state != 3)
                            Log(L"wechat: window hidden, reading the %zu chats held from it", held.size());
                    }
                }
                if (!found && !anyHung) {
                    report(1, L"WeChat's chat list is not readable (open its window once)");
                    baseline = false;
                    if (held.empty())
                        ResetUnreadState(states, unread_, notify_, badgeMsg_);
                } else if (found) {
                    if (state < 2 || (state == 3 && !fromHeld))
                        Log(L"wechat: reading WeChat's chat list (%zu chats in view)", rows.size());
                    state = fromHeld ? 3 : 2;
                    reading_ = true;
                    UpdateUnread(rows, baseline, blockOfficial_, postCards_, exe, notify_, msg_, states);
                    const int total = SumWeChatUnread(rows);
                    if (unread_.exchange(total) != total && badgeMsg_)
                        PostMessageW(notify_, badgeMsg_, WPARAM(total), 0);
                    baseline = true;
                } else {
                    reading_ = false;
                }
            } catch (...) {
                reading_ = false;
                Log(L"wechat: reading failed");
            }
            if (!stop_)
                WaitForUiaWake(wake_, WeChatPollWait(state, followUpTick));
        }
    }
    reading_ = false;
    held.clear();
    cells = nullptr;
    cache = nullptr;
    uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
