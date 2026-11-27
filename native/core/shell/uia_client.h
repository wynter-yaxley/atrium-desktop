// A UI Automation client for a reader thread (WeChat, Feishu). Right after
// Atrium starts, one of the two often fails to make its condition (E_FAIL),
// which used to end that reader for the session: a failure is retried every
// two seconds (logged once).
#pragma once

#include "log.h"

#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <utility>
#include <vector>

namespace atrium {

// Callbacks only wake the reader. Cross-process tree reads remain on its MTA
// worker.
class UiaChanges : public winrt::implements<UiaChanges, IUIAutomationPropertyChangedEventHandler, IUIAutomationStructureChangedEventHandler> {
public:
    explicit UiaChanges(HANDLE wake)
    {
        // UIA can finish an in-flight callback after unsubscription. The sink
        // keeps its own reference to the event until UIA releases that callback.
        winrt::check_bool(DuplicateHandle(GetCurrentProcess(), wake, GetCurrentProcess(), wake_.put(), 0, FALSE, DUPLICATE_SAME_ACCESS));
    }
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID, VARIANT) noexcept override
    {
        SetEvent(wake_.get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) noexcept override
    {
        SetEvent(wake_.get());
        return S_OK;
    }

private:
    winrt::handle wake_;
};

// Scoped to one client: replacing a window releases its subscriptions before
// the wake handle closes.
class UiaWatch {
public:
    UiaWatch(IUIAutomation* uia, HANDLE wake) : uia_(uia), changes_(winrt::make_self<UiaChanges>(wake)) {}
    ~UiaWatch() { Clear(); }
    UiaWatch(const UiaWatch&) = delete;
    UiaWatch& operator=(const UiaWatch&) = delete;
    bool Contains(HWND window) const
    {
        return std::ranges::any_of(roots_, [&](const auto& item) { return item.window == window; });
    }
    void Add(HWND window, IUIAutomationElement* root)
    {
        if (Contains(window))
            return;
        PROPERTYID properties[] = {UIA_NamePropertyId};
        HRESULT name = uia_->AddPropertyChangedEventHandlerNativeArray(root, TreeScope_Subtree, nullptr, changes_.get(), properties, 1);
        HRESULT structure = uia_->AddStructureChangedEventHandler(root, TreeScope_Subtree, nullptr, changes_.get());
        winrt::com_ptr<IUIAutomationElement> keep;
        keep.copy_from(root);
        roots_.push_back({window, std::move(keep)});
        Log(L"uia: subscribed window=%p name=0x%08X structure=0x%08X", window, unsigned(name), unsigned(structure));
    }
    void Clear()
    {
        for (const auto& item : roots_) {
            if (!IsHungAppWindow(item.window)) {
                uia_->RemovePropertyChangedEventHandler(item.root.get(), changes_.get());
                uia_->RemoveStructureChangedEventHandler(item.root.get(), changes_.get());
            }
        }
        roots_.clear();
    }
    void Prune()
    {
        for (size_t i = roots_.size(); i-- > 0;)
            if (!IsWindow(roots_[i].window)) {
                if (!IsHungAppWindow(roots_[i].window)) {
                    uia_->RemovePropertyChangedEventHandler(roots_[i].root.get(), changes_.get());
                    uia_->RemoveStructureChangedEventHandler(roots_[i].root.get(), changes_.get());
                }
                roots_.erase(roots_.begin() + ptrdiff_t(i));
            }
    }

private:
    struct Root {
        HWND window;
        winrt::com_ptr<IUIAutomationElement> root;
    };
    IUIAutomation* uia_;
    winrt::com_ptr<UiaChanges> changes_;
    std::vector<Root> roots_;
};

class UiaWindowWake {
public:
    UiaWindowWake(HANDLE wake, const wchar_t* windowClass, bool watchNameChanges = true, bool watchForeground = false)
        : wake_(wake),
          windowClass_(windowClass),
          watchNameChanges_(watchNameChanges),
          show_(SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, Changed, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)),
          foreground_(watchForeground ? SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, Changed, 0, 0,
                                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS) :
                                        nullptr)
    {
        g_active = this;
    }
    ~UiaWindowWake()
    {
        if (foreground_)
            UnhookWinEvent(foreground_);
        if (show_)
            UnhookWinEvent(show_);
        for (const auto& [pid, hook] : nameHooks_)
            if (hook)
                UnhookWinEvent(hook);
        g_active = nullptr;
    }
    UiaWindowWake(const UiaWindowWake&) = delete;
    UiaWindowWake& operator=(const UiaWindowWake&) = delete;

    template<typename Processes> void SetProcesses(const Processes& processes)
    {
        std::vector<DWORD> next;
        for (const auto& entry : processes)
            if (entry.first != 0 && std::ranges::find(next, entry.first) == next.end())
                next.push_back(entry.first);
        pids_ = next;
        if (!watchNameChanges_)
            return;
        for (size_t i = nameHooks_.size(); i-- > 0;)
            if (std::ranges::find(next, nameHooks_[i].first) == next.end()) {
                if (nameHooks_[i].second)
                    UnhookWinEvent(nameHooks_[i].second);
                nameHooks_.erase(nameHooks_.begin() + ptrdiff_t(i));
            }
        for (DWORD pid : next) {
            if (std::ranges::any_of(nameHooks_, [pid](const auto& item) { return item.first == pid; }))
                continue;
            HWINEVENTHOOK hook =
                SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_VALUECHANGE, nullptr, Changed, pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (hook)
                nameHooks_.emplace_back(pid, hook);
        }
    }

private:
    static void CALLBACK Changed(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD)
    {
        if (!g_active || !window)
            return;
        if (event == EVENT_SYSTEM_FOREGROUND) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            const bool ours = std::ranges::find(g_active->pids_, pid) != g_active->pids_.end();
            if (ours || std::exchange(g_active->targetForeground_, false)) {
                g_active->targetForeground_ = ours;
                SetEvent(g_active->wake_);
            }
            return;
        }
        bool appearance = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE;
        if (appearance && (object != OBJID_WINDOW || child != CHILDID_SELF || GetAncestor(window, GA_ROOT) != window))
            return;
        HWND root = GetAncestor(window, GA_ROOT);
        if (!root)
            return;
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        if (!appearance && std::ranges::none_of(g_active->nameHooks_, [pid](const auto& item) { return item.first == pid; }))
            return;
        wchar_t name[96]{};
        GetClassNameW(root, name, int(std::size(name)));
        if (wcsstr(name, g_active->windowClass_))
            SetEvent(g_active->wake_);
    }
    inline static thread_local UiaWindowWake* g_active = nullptr;
    HANDLE wake_;
    const wchar_t* windowClass_;
    bool watchNameChanges_;
    bool targetForeground_ = false;
    std::vector<DWORD> pids_;
    std::vector<std::pair<DWORD, HWINEVENTHOOK>> nameHooks_;
    HWINEVENTHOOK show_ = nullptr;
    HWINEVENTHOOK foreground_ = nullptr;
};

// Out-of-context WinEvents arrive on the installing thread, which must pump
// messages.
inline void WaitForUiaWake(HANDLE wake, DWORD timeout)
{
    const ULONGLONG until = GetTickCount64() + timeout;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until)
            return;
        DWORD result = MsgWaitForMultipleObjectsEx(1, &wake, DWORD(until - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT || result == WAIT_FAILED)
            return;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (GetTickCount64() >= until || WaitForSingleObject(wake, 0) == WAIT_OBJECT_0)
                return;
        }
    }
}

// A cache request for `properties` of the found elements alone, over the raw view,
// without live element references unless `keepLiveElements` is set (so BuildUpdatedCache
// works on held elements across close-to-tray transitions).
inline HRESULT CreateRawCache(IUIAutomation* uia, std::initializer_list<PROPERTYID> properties, winrt::com_ptr<IUIAutomationCacheRequest>& cache,
                              bool keepLiveElements = false)
{
    cache = nullptr;
    winrt::com_ptr<IUIAutomationCondition> raw;
    HRESULT hr = uia->CreateCacheRequest(cache.put());
    for (PROPERTYID property : properties)
        if (SUCCEEDED(hr))
            hr = cache->AddProperty(property);
    if (SUCCEEDED(hr))
        hr = cache->put_TreeScope(TreeScope_Element);
    if (SUCCEEDED(hr))
        hr = uia->get_RawViewCondition(raw.put());
    if (SUCCEEDED(hr))
        hr = cache->put_TreeFilter(raw.get());
    if (SUCCEEDED(hr) && !keepLiveElements)
        hr = cache->put_AutomationElementMode(AutomationElementMode_None);
    return hr;
}

// `prepare` makes what the reader needs from the client (its conditions) and
// returns an HRESULT.
template<typename Prepare> winrt::com_ptr<IUIAutomation> CreateUia(const std::atomic<bool>& stop, HANDLE wake, const wchar_t* who, Prepare prepare)
{
    DWORD retryMs = 250;
    for (bool logged = false; !stop;) {
        winrt::com_ptr<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
        if (FAILED(hr))
            hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(uia.put()));
        if (SUCCEEDED(hr)) {
            if (auto options = uia.try_as<IUIAutomation2>()) {
                options->put_ConnectionTimeout(250);
                options->put_TransactionTimeout(250);
            }
            hr = prepare(uia.get());
        }
        if (SUCCEEDED(hr))
            return uia;
        if (!logged)
            Log(L"%s: UI Automation set-up failed (0x%08X), retrying", who, unsigned(hr));
        logged = true;
        WaitForSingleObject(wake, retryMs);
        retryMs = std::min<DWORD>(retryMs * 2, 2000);
    }
    return nullptr;
}

} // namespace atrium
