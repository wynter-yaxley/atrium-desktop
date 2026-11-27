#include "shell/wechat_source.h"

#include "log.h"
#include "shell/uia_client.h"

#include <uiautomation.h>

#include <winrt/base.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

namespace {

enum OpenLogBit : uint32_t {
    LogInvoke = 1u << 0,
    LogSelect = 1u << 1,
    LogLegacy = 1u << 2,
    LogDialog = 1u << 3,
    LogHung = 1u << 4,
    LogTimeout = 1u << 5,
    LogNotFound = 1u << 6,
    LogNoPattern = 1u << 7,
};

void LogOpenOnce(uint32_t& mask, OpenLogBit bit, const wchar_t* message)
{
    if ((mask & bit) == 0) {
        mask |= bit;
        Log(L"%s", message);
    }
}

DWORD WindowPid(HWND h)
{
    DWORD pid = 0;
    if (h)
        GetWindowThreadProcessId(h, &pid);
    return pid;
}

bool IsVisibleNonIconic(HWND h) { return h && IsWindowVisible(h) && !IsIconic(h); }

std::vector<HWND> VisibleWeChatWindows(const std::vector<std::pair<DWORD, std::wstring>>& procs, const std::map<DWORD, HWND>& lastMainByPid, DWORD targetPid,
                                       bool& anyHung)
{
    std::vector<HWND> out;
    auto append = [&](HWND h) {
        if (IsVisibleNonIconic(h) && std::ranges::find(out, h) == out.end())
            out.push_back(h);
    };
    if (targetPid != 0) {
        if (auto it = lastMainByPid.find(targetPid); it != lastMainByPid.end() && detail::ShouldReadWeChatWindow(it->second, targetPid, &anyHung))
            append(it->second);
        for (HWND h : detail::WeChatWindows(targetPid, anyHung))
            append(h);
        return out;
    }
    for (const auto& [pid, last] : lastMainByPid) {
        if (std::ranges::any_of(procs, [pid](const auto& p) { return p.first == pid; }) && detail::ShouldReadWeChatWindow(last, pid, &anyHung))
            append(last);
    }
    for (const auto& [pid, program] : procs)
        for (HWND h : detail::WeChatWindows(pid, anyHung))
            append(h);
    return out;
}

bool ActivateChatCell(IUIAutomationElement* e, uint32_t& loggedMask)
{
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()))) && invoke && SUCCEEDED(invoke->Invoke())) {
        LogOpenOnce(loggedMask, LogInvoke, L"wechat: open chat via InvokePattern");
        return true;
    }
    winrt::com_ptr<IUIAutomationSelectionItemPattern> select;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(select.put()))) && select && SUCCEEDED(select->Select())) {
        LogOpenOnce(loggedMask, LogSelect, L"wechat: open chat via SelectionItemPattern");
        return true;
    }
    winrt::com_ptr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(legacy.put()))) && legacy && SUCCEEDED(legacy->DoDefaultAction())) {
        LogOpenOnce(loggedMask, LogLegacy, L"wechat: open chat via LegacyIAccessible");
        return true;
    }
    LogOpenOnce(loggedMask, LogNoPattern, L"wechat: open chat failed (no supported UIA pattern)");
    return false;
}

enum class OpenAttempt { Retry, NotFound, Done };

OpenAttempt TryOpenChatInWindow(IUIAutomation* uia, IUIAutomationCondition* cells, HWND target, const std::wstring& chat, uint32_t& loggedMask)
{
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::com_ptr<IUIAutomationElementArray> all;
    if (FAILED(uia->ElementFromHandle(target, root.put())) || !root || FAILED(root->FindAll(TreeScope_Descendants, cells, all.put())) || !all)
        return OpenAttempt::Retry;
    int n = 0;
    all->get_Length(&n);
    if (n == 0)
        return OpenAttempt::Retry;
    for (int i = 0; i < n; ++i) {
        winrt::com_ptr<IUIAutomationElement> e;
        all->GetElement(i, e.put());
        if (!e)
            continue;
        BSTR name = nullptr;
        const bool matched = SUCCEEDED(e->get_CurrentName(&name)) && name && MatchesWeChatCellName(name, chat);
        SysFreeString(name);
        if (matched) {
            ActivateChatCell(e.get(), loggedMask);
            return OpenAttempt::Done;
        }
    }
    return OpenAttempt::NotFound;
}

} // namespace

bool MatchesWeChatCellName(const std::wstring& cellName, const std::wstring& chat) { return !chat.empty() && ParseWeChatRow(cellName).chat == chat; }

void WeChatSource::OpenChat(const std::wstring& chat, DWORD pid)
{
    if (chat.empty() || stop_)
        return;
    {
        std::scoped_lock lock(openMu_);
        pendingOpenChat_ = chat;
        pendingOpenPid_ = pid;
    }
    if (wake_)
        SetEvent(wake_);
}

namespace detail {

void HandlePendingWeChatOpen(IUIAutomation* uia, IUIAutomationCondition* cells, HANDLE wake, const std::atomic<bool>& stop, std::mutex& openMu,
                             std::wstring& pendingOpenChat, DWORD& pendingOpenPid, std::vector<std::pair<DWORD, std::wstring>>& procs,
                             std::map<DWORD, HWND>& lastMainByPid, uint32_t& loggedMask)
{
    std::wstring chat;
    DWORD targetPid = 0;
    {
        std::scoped_lock lock(openMu);
        if (pendingOpenChat.empty())
            return;
        chat = std::move(pendingOpenChat);
        targetPid = pendingOpenPid;
        pendingOpenChat.clear();
        pendingOpenPid = 0;
    }
    constexpr DWORD kOpenWaitMs = 1500;
    const ULONGLONG deadline = GetTickCount64() + kOpenWaitMs;
    while (!stop) {
        if (procs.empty())
            DiscoverWeChatProcesses(procs);
        if (!procs.empty()) {
            const WeChatBusyState busy = targetPid ? QueryWeChatBusyStateForPid(targetPid) : QueryWeChatBusyState(procs);
            if (busy == WeChatBusyState::Dialog) {
                LogOpenOnce(loggedMask, LogDialog, L"wechat: open chat skipped (modal dialog)");
                return;
            }
            bool anyHung = false;
            const std::vector<HWND> targets = VisibleWeChatWindows(procs, lastMainByPid, targetPid, anyHung);
            if (anyHung) {
                LogOpenOnce(loggedMask, LogHung, L"wechat: open chat skipped (hung window)");
                return;
            }
            bool anyNotFound = false;
            for (HWND target : targets) {
                const OpenAttempt attempt = TryOpenChatInWindow(uia, cells, target, chat, loggedMask);
                if (attempt == OpenAttempt::Done) {
                    lastMainByPid[WindowPid(target)] = target;
                    return;
                }
                if (attempt == OpenAttempt::NotFound)
                    anyNotFound = true;
            }
            if (anyNotFound) {
                LogOpenOnce(loggedMask, LogNotFound, L"wechat: open chat skipped (cell not in visible list)");
                return;
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        WaitForUiaWake(wake, DWORD(std::min<ULONGLONG>(150, deadline - now)));
    }
    LogOpenOnce(loggedMask, LogTimeout, L"wechat: open chat timed out waiting for visible window");
}

} // namespace detail

} // namespace atrium
