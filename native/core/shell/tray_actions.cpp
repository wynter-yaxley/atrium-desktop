// Tray button discovery, overflow chevron invocation, click dispatch, and response observation.
#include "shell/tray_model.h"

#include "log.h"
#include "shell/keyboard_hook.h"
#include "shell/messengers.h"
#include "shell/shell_icon.h"
#include "shell/shell_taskbar.h"
#include "shell/system_chrome.h"
#include "shell/task_model.h"
#include "shell/tray_click.h"
#include "shell/tray_match.h"
#include "shell/tray_pointer.h"
#include "win32_path.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <set>

namespace atrium {

namespace {

std::atomic<DWORD> g_syntheticInput{0}; // GetLastInputInfo time of the last pointer click Atrium sent
static_assert(tray::kProviderTimeout == int32_t(UIA_E_TIMEOUT));

std::vector<int> RuntimeId(IUIAutomationElement* element)
{
    SAFEARRAY* id = nullptr;
    std::vector<int> out;
    if (element && SUCCEEDED(element->GetRuntimeId(&id)) && id) {
        LONG first = 0, last = -1;
        if (SUCCEEDED(SafeArrayGetLBound(id, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(id, 1, &last))) {
            for (LONG i = first; i <= last && i - first < 64; ++i) {
                int value = 0;
                if (FAILED(SafeArrayGetElement(id, &i, &value))) {
                    out.clear();
                    break;
                }
                out.push_back(value);
            }
        }
    }
    if (id)
        SafeArrayDestroy(id);
    return out;
}

void CollectHostButtons(IUIAutomation* uia, HWND host, bool promoted, bool xaml, std::vector<tray_detail::Found>& out, bool& available)
{
    HWND bridge = xaml && host ? FindWindowExW(host, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
    winrt::com_ptr<IUIAutomationElement> root;
    if (!host || FAILED(uia->ElementFromHandle(bridge ? bridge : host, root.put())) || !root)
        return;
    available = true;
    winrt::com_ptr<IUIAutomationCondition> cond;
    VARIANT v{};
    v.vt = VARTYPE(xaml ? VT_BSTR : VT_I4);
    if (xaml)
        v.bstrVal = SysAllocString(L"NotifyItemIcon");
    else
        v.lVal = UIA_ButtonControlTypeId;
    const HRESULT hr = uia->CreatePropertyCondition(xaml ? UIA_AutomationIdPropertyId : UIA_ControlTypePropertyId, v, cond.put());
    VariantClear(&v);
    winrt::com_ptr<IUIAutomationElementArray> all;
    if (FAILED(hr) || FAILED(root->FindAll(xaml ? TreeScope_Descendants : TreeScope_Children, cond.get(), all.put())) || !all)
        return;
    int n = 0;
    all->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        winrt::com_ptr<IUIAutomationElement> elem;
        all->GetElement(i, elem.put());
        if (!elem)
            continue;
        tray_detail::Found f;
        (xaml ? f.host : f.toolbar) = host;
        BSTR name = nullptr;
        elem->get_CurrentName(&name);
        f.tip = name ? tray::RepairMojibakeTip(name) : L"";
        SysFreeString(name);
        elem->get_CurrentBoundingRectangle(&f.bounds);
        if (xaml && f.tip.empty() && f.bounds.left == 0 && f.bounds.top == 0 && f.bounds.right == 0 && f.bounds.bottom == 0) {
            BOOL offscreen = FALSE;
            if (SUCCEEDED(elem->get_CurrentIsOffscreen(&offscreen)) && offscreen)
                continue;
        }
        f.promoted = promoted;
        f.index = int(std::count_if(out.begin(), out.end(), [&](const tray_detail::Found& o) { return o.promoted == promoted; }));
        f.runtimeId = RuntimeId(elem.get());
        f.element = elem;
        out.push_back(std::move(f));
    }
}

bool OverflowUsable(IUIAutomation* uia)
{
    HWND overflow = tray_detail::OverflowWindow();
    if (!uia || !overflow || !IsWindowVisible(overflow))
        return false;
    bool available = false;
    const auto buttons = tray_detail::FindButtons(uia, available);
    for (const auto& button : buttons) {
        auto elem = button.element.try_as<IUIAutomationElement>();
        if (button.promoted || !elem || IsRectEmpty(&button.bounds))
            continue;
        BOOL offscreen = TRUE;
        if (FAILED(elem->get_CurrentIsOffscreen(&offscreen)) || offscreen)
            continue;
        const POINT at{button.bounds.left + (button.bounds.right - button.bounds.left) / 2, button.bounds.top + (button.bounds.bottom - button.bounds.top) / 2};
        HWND hit = WindowFromPoint(at);
        if (hit == overflow || GetAncestor(hit, GA_ROOT) == overflow)
            return true;
    }
    return false;
}

bool WaitOverflowUsable(IUIAutomation* uia, HANDLE stopEvent)
{
    for (int i = 0; i < 10; ++i) {
        if (OverflowUsable(uia))
            return true;
        if (!tray_detail::WaitStep(stopEvent, 20))
            return false;
    }
    return false;
}

bool ShowClassicOverflow(IUIAutomation* uia, HWND tray, HANDLE stopEvent)
{
    // Classic shell: the unique Button directly inside TrayNotifyWnd is its chevron.
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND button = notify ? FindWindowExW(notify, nullptr, L"Button", nullptr) : nullptr;
    if (!button || FindWindowExW(notify, button, L"Button", nullptr))
        return false;
    const BOOL queued = PostMessageW(button, BM_CLICK, 0, 0);
    Log(L"tray: overflow classic BM_CLICK queued=%d error=%lu", queued, queued ? 0 : GetLastError());
    return queued && WaitOverflowUsable(uia, stopEvent);
}

winrt::com_ptr<IUIAutomationElement> FindOverflowChevron(IUIAutomation* uia, HWND bridge)
{
    winrt::com_ptr<IUIAutomationElement> root, chevron;
    winrt::com_ptr<IUIAutomationCondition> condition;
    winrt::com_ptr<IUIAutomationElementArray> elements;
    if (!uia || !bridge || FAILED(uia->ElementFromHandle(bridge, root.put())) || !root)
        return {};
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_ButtonControlTypeId;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, value, condition.put())) ||
        FAILED(root->FindAll(TreeScope_Descendants, condition.get(), elements.put())) || !elements)
        return {};
    int count = 0, before = 0, after = 0;
    bool anchor = false;
    elements->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IUIAutomationElement> element;
        elements->GetElement(i, element.put());
        if (!element)
            continue;
        BSTR id = nullptr, cls = nullptr;
        element->get_CurrentAutomationId(&id);
        element->get_CurrentClassName(&cls);
        if (id && wcscmp(id, L"NotifyItemIcon") == 0)
            anchor = true;
        if (id && cls && wcscmp(id, L"SystemTrayIcon") == 0 && wcscmp(cls, L"SystemTray.NormalButton") == 0) {
            if (!anchor) {
                ++before;
                chevron = element;
            } else {
                ++after;
            }
        }
        SysFreeString(id);
        SysFreeString(cls);
    }
    if (!anchor || before != 1 || after < 1 || !chevron) {
        Log(L"tray: overflow chevron ambiguous anchor=%d before=%d after=%d", anchor, before, after);
        return {};
    }
    return chevron;
}

const wchar_t* RouteName(tray::Route route)
{
    switch (route) {
    case tray::Route::Invoke: return L"uia-invoke";
    case tray::Route::ContextMenu: return L"uia-context-menu";
    case tray::Route::LegacyDefault: return L"legacy-default";
    default: return L"toolbar-messages";
    }
}

const wchar_t* ActionName(tray::Action a)
{
    return a == tray::Action::Right              ? L"right" :
           a == tray::Action::Double             ? L"double" :
           a == tray::Action::DoubleContinuation ? L"double-continuation" :
                                                   L"left";
}

HRESULT ToolbarClick(const tray_detail::Found& target, tray::Action action)
{
    wchar_t cls[80]{};
    if (!IsWindow(target.toolbar) || !GetClassNameW(target.toolbar, cls, 80) || wcscmp(cls, L"ToolbarWindow32") != 0)
        return E_INVALIDARG;
    HWND root = GetAncestor(target.toolbar, GA_ROOT);
    GetClassNameW(root, cls, 80);
    if (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"NotifyIconOverflowWindow") != 0)
        return E_INVALIDARG;
    DWORD pid = 0, shellPid = 0;
    GetWindowThreadProcessId(root, &pid);
    GetWindowThreadProcessId(ShellTaskbar(), &shellPid);
    if (!pid || pid != shellPid)
        return E_INVALIDARG;
    auto elem = target.element.try_as<IUIAutomationElement>();
    RECT bounds{}, client{};
    if (!elem || FAILED(elem->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) || !GetClientRect(target.toolbar, &client))
        return UIA_E_ELEMENTNOTAVAILABLE;
    POINT at{bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    if (!ScreenToClient(target.toolbar, &at) || !PtInRect(&client, at) || at.x > 32767 || at.y > 32767)
        return E_INVALIDARG;
    const LPARAM position = MAKELPARAM(at.x, at.y);
    DWORD error = 0;
    const auto sent = tray::PostClassicGesture(action, [&](tray::MouseEvent event) {
        UINT message = WM_MOUSEMOVE;
        WPARAM keys = 0;
        switch (event) {
        case tray::MouseEvent::LeftDown:
            message = WM_LBUTTONDOWN;
            keys = MK_LBUTTON;
            break;
        case tray::MouseEvent::LeftUp: message = WM_LBUTTONUP; break;
        case tray::MouseEvent::LeftDouble:
            message = WM_LBUTTONDBLCLK;
            keys = MK_LBUTTON;
            break;
        case tray::MouseEvent::RightDown:
            message = WM_RBUTTONDOWN;
            keys = MK_RBUTTON;
            break;
        case tray::MouseEvent::RightUp: message = WM_RBUTTONUP; break;
        default: break;
        }
        if (PostMessageW(target.toolbar, message, keys, position))
            return true;
        error = GetLastError();
        return false;
    });
    Log(L"tray: toolbar queued=%zu complete=%d actionQueued=%d error=%lu", sent.queued, sent.complete, sent.actionQueued, error);
    // S_FALSE records partial delivery as uncertain, so Dispatch will not send another action.
    return sent.complete ? S_OK : sent.actionQueued ? S_FALSE : error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

struct ResponseState {
    std::set<HWND> visible, menus;
    HWND foreground = nullptr;
};

enum class ResponseOwner { Unknown, Match, Mismatch, AccessDenied };

ResponseOwner ClassifyResponsePid(DWORD pid, DWORD targetPid, const std::wstring& exe, std::map<DWORD, ResponseOwner>& owned)
{
    if (auto it = owned.find(pid); it != owned.end())
        return it->second;
    ResponseOwner owner = ResponseOwner::Mismatch;
    if (targetPid != 0 && pid == targetPid) {
        owner = ResponseOwner::Match;
    } else if (!exe.empty()) {
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[32768];
            DWORD size = DWORD(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size) && _wcsicmp(path, exe.c_str()) == 0)
                owner = ResponseOwner::Match;
            CloseHandle(process);
        } else if (GetLastError() == ERROR_ACCESS_DENIED) {
            owner = ResponseOwner::AccessDenied;
        }
    }
    owned.emplace(pid, owner);
    return owner;
}

bool IsElevatedPopupCandidate(HWND hwnd, const wchar_t* cls)
{
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & WS_POPUP) != 0 && (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0 && (ex & WS_EX_APPWINDOW) == 0 &&
           _wcsicmp(cls, L"tooltips_class32") != 0 && _wcsicmp(cls, L"Shell_TrayWnd") != 0 && _wcsicmp(cls, L"TopLevelWindowForOverflowXamlIsland") != 0;
}

ResponseState CaptureResponse(const std::wstring& exe, DWORD targetPid, tray::Action action)
{
    ResponseState state;
    if (exe.empty() && targetPid == 0 && action != tray::Action::Right)
        return state;
    struct Context {
        const std::wstring& exe;
        DWORD targetPid;
        tray::Action action;
        ResponseState& state;
        std::map<DWORD, ResponseOwner> owned;
    } ctx{exe, targetPid, action, state, {}};
    EnumWindows(
        [](HWND h, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Context*>(param);
            if (!IsWindowVisible(h))
                return TRUE;
            wchar_t cls[32]{};
            const bool nativeMenu = GetClassNameW(h, cls, 32) != 0 && wcscmp(cls, L"#32768") == 0;
            if (nativeMenu)
                c.state.menus.insert(h);
            DWORD pid = 0;
            const DWORD thread = GetWindowThreadProcessId(h, &pid);
            const ResponseOwner owner = pid ? ClassifyResponsePid(pid, c.targetPid, c.exe, c.owned) : ResponseOwner::Mismatch;
            if (owner != ResponseOwner::Match) {
                if (tray::ShouldObserveUnattributedMenu(nativeMenu, owner == ResponseOwner::AccessDenied, c.action == tray::Action::Right,
                                                        IsElevatedPopupCandidate(h, cls)))
                    c.state.menus.insert(h);
                return TRUE;
            }
            c.state.visible.insert(h);
            if (GetForegroundWindow() == h)
                c.state.foreground = h;
            GUITHREADINFO gui{sizeof(gui)};
            if (GetGUIThreadInfo(thread, &gui) && (gui.flags & GUI_INMENUMODE) && gui.hwndMenuOwner)
                c.state.menus.insert(gui.hwndMenuOwner);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return state;
}

bool ResponseChanged(const ResponseState& before, const ResponseState& after, tray::Action action)
{
    for (HWND h : after.menus)
        if (!before.menus.contains(h))
            return true;
    for (HWND h : after.visible)
        if (!before.visible.contains(h))
            return true;
    return action != tray::Action::Right && after.foreground && before.foreground != after.foreground;
}

bool ForegroundExisting(const std::wstring& exe, HANDLE stopEvent)
{
    const auto state = CaptureResponse(exe, 0, tray::Action::Left);
    for (HWND h : state.visible) {
        if (GetWindow(h, GW_OWNER) || (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
            continue;
        if (IsIconic(h))
            ShowWindowAsync(h, SW_RESTORE);
        SetForegroundWindow(h);
        for (int i = 0; i < 5; ++i) {
            if (GetForegroundWindow() == h && !IsIconic(h))
                return true;
            if (!tray_detail::WaitStep(stopEvent, 20))
                return false;
        }
    }
    return false;
}

struct TaskbarClickReveal {
    bool active = SystemChrome::RevealForClick();
    TaskbarClickReveal() = default;
    TaskbarClickReveal(const TaskbarClickReveal&) = delete;
    TaskbarClickReveal& operator=(const TaskbarClickReveal&) = delete;
    ~TaskbarClickReveal()
    {
        if (active)
            SystemChrome::ConcealAfterClick();
    }
};

struct TrayPointerDesktop {
    IUIAutomation* uia;
    IUIAutomationElement* element;
    HWND host;
    HANDLE stopEvent;

    bool Cursor(POINT& p) const { return GetCursorPos(&p) != FALSE; }
    bool Move(POINT p) const { return SetCursorPos(p.x, p.y) != FALSE; }
    void Wait(DWORD ms) const { tray_detail::WaitStep(stopEvent, ms); }
    bool Interacting() const
    {
        constexpr int kKeys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN};
        return std::ranges::any_of(kKeys, [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; });
    }
    bool Target(POINT& p) const
    {
        RECT bounds{}, on{};
        if (!element || !host || !IsWindowVisible(host) || FAILED(element->get_CurrentBoundingRectangle(&bounds)) || IsRectEmpty(&bounds) ||
            !GetWindowRect(host, &on))
            return false;
        p = {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
        return PtInRect(&on, p) && MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
    }
    bool TargetAt(POINT p) const
    {
        HWND under = WindowFromPoint(p);
        if (!under || (GetAncestor(under, GA_ROOT) != host && !IsChild(host, under)))
            return false;
        winrt::com_ptr<IUIAutomationElement> hit;
        winrt::com_ptr<IUIAutomationTreeWalker> walker;
        if (FAILED(uia->ElementFromPoint(p, hit.put())) || !hit || FAILED(uia->get_RawViewWalker(walker.put())) || !walker)
            return false;
        for (int depth = 0; hit && depth < 12; ++depth) {
            BOOL same = FALSE;
            if (SUCCEEDED(uia->CompareElements(element, hit.get(), &same)) && same)
                return true;
            winrt::com_ptr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(hit.get(), parent.put())))
                break;
            hit = std::move(parent);
        }
        return false;
    }
    bool Send(POINT p) const
    {
        const LONG x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const RECT desktop{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        std::array<INPUT, 3> input{};
        if (!tray::RightClickInputs(p, desktop, input))
            return false;
        for (INPUT& item : input)
            item.mi.dwExtraInfo = kAtriumSyntheticKey;
        const UINT sent = SendInput(UINT(input.size()), input.data(), sizeof(INPUT));
        if (sent == 2) {
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            release.mi.dwExtraInfo = kAtriumSyntheticKey;
            SendInput(1, &release, sizeof(INPUT));
        }
        LASTINPUTINFO last{sizeof(last)};
        if (GetLastInputInfo(&last))
            g_syntheticInput = last.dwTime;
        return sent == input.size();
    }
};

HRESULT PointerRightClick(IUIAutomation* uia, const tray_detail::Found& target, HANDLE stopEvent, HWND notify, UINT msg)
{
    auto targetEl = target.element.try_as<IUIAutomationElement>();
    if (!targetEl)
        return E_ABORT;
    TaskbarClickReveal reveal;
    if (!reveal.active)
        return E_ABORT;
    if (notify && msg) {
        DWORD_PTR ack = 0;
        SendMessageTimeoutW(notify, msg, 6, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &ack);
    }
    HWND opened = nullptr;
    if (!target.promoted) {
        HWND before = tray_detail::OverflowWindow();
        const bool visible = before && IsWindowVisible(before);
        if (!tray_detail::ShowOverflow(uia, stopEvent))
            return E_FAIL;
        if (!visible)
            opened = tray_detail::OverflowWindow();
    }
    std::unique_ptr<HWND__, void (*)(HWND)> hide(opened, [](HWND h) {
        if (IsWindow(h))
            ShowWindowAsync(h, SW_HIDE);
    });
    bool available = false;
    auto buttons = tray_detail::FindButtons(uia, available);
    auto it = std::ranges::find_if(buttons, [&](const tray_detail::Found& f) {
        auto elem = f.element.try_as<IUIAutomationElement>();
        BOOL same = FALSE;
        return elem && SUCCEEDED(uia->CompareElements(targetEl.get(), elem.get(), &same)) && same;
    });
    if (it == buttons.end())
        return E_ABORT;
    auto itEl = it->element.try_as<IUIAutomationElement>();
    TrayPointerDesktop desktop{uia, itEl.get(), it->host, stopEvent};
    const auto result = tray::RightClick(desktop);
    return result == tray::PointerClick::Sent ? S_OK : result == tray::PointerClick::Cancelled ? E_ABORT : E_FAIL;
}

tray::ResolvedTarget ResolveClickTarget(const TrayIcon& icon, const tray::ShellSession& session, std::vector<tray_detail::Found>& buttons,
                                        const std::vector<int>& match, const std::vector<tray_detail::Record>& records)
{
    std::vector<tray::TargetKey> candidates;
    candidates.reserve(buttons.size());
    for (size_t b = 0; b < buttons.size(); ++b)
        candidates.push_back({session, buttons[b].runtimeId, buttons[b].recordId, match[b] >= 0 ? records[size_t(match[b])].exe : L"", buttons[b].tip});
    const auto resolved = tray::ResolveTarget(icon.ActionTarget(), tray_detail::ReadShellSession(), candidates);
    Log(L"tray: click resolve=%s shellPid=%lu shellStarted=%llu shellWindow=%llu", tray::ResolutionName(resolved.reason), DWORD(session.pid), session.started,
        session.window);
    return resolved;
}

bool ObserveTrayResponse(const std::wstring& exe, DWORD targetPid, tray::Action action, const ResponseState& before, bool accepted, HANDLE stopEvent)
{
    const ULONGLONG until = GetTickCount64() + (!accepted ? 40 : action == tray::Action::Right ? 500 : 280);
    do {
        if (ResponseChanged(before, CaptureResponse(exe, targetPid, action), action))
            return true;
        if (!tray_detail::WaitStep(stopEvent, 20))
            return false;
    } while (GetTickCount64() < until);
    return false;
}

int32_t InvokeTrayRoute(tray_detail::Found* target, const tray::ShellSession& session, tray::Action action, tray::Route route)
{
    HRESULT hr = UIA_E_ELEMENTNOTAVAILABLE;
    auto elem = target ? target->element.try_as<IUIAutomationElement>() : winrt::com_ptr<IUIAutomationElement>{};
    if (target && elem && session == tray_detail::ReadShellSession()) {
        const BOOL allowed = AllowSetForegroundWindow(session.pid);
        Log(L"tray: click foreground-delegate pid=%lu allowed=%d error=%lu", DWORD(session.pid), allowed, allowed ? 0 : GetLastError());
        if (route == tray::Route::Toolbar) {
            hr = ToolbarClick(*target, action);
        } else if (route == tray::Route::ContextMenu) {
            auto e3 = elem.try_as<IUIAutomationElement3>();
            hr = e3 ? e3->ShowContextMenu() : E_NOINTERFACE;
        } else if (route == tray::Route::Invoke) {
            winrt::com_ptr<IUIAutomationInvokePattern> p;
            hr = elem->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(p.put()));
            if (SUCCEEDED(hr))
                hr = p ? p->Invoke() : E_NOINTERFACE;
        } else {
            winrt::com_ptr<IUIAutomationLegacyIAccessiblePattern> p;
            hr = elem->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(p.put()));
            if (SUCCEEDED(hr))
                hr = p ? p->DoDefaultAction() : E_NOINTERFACE;
        }
    }
    Log(L"tray: click attempt route=%s hr=0x%08X", RouteName(route), unsigned(hr));
    return int32_t(hr);
}

void LogClickBegin(tray::Action action, const TrayIcon& icon, const tray_detail::Found* target)
{
    HWND taskbar = ShellTaskbar(), overflow = tray_detail::OverflowWindow();
    APPBARDATA abd{sizeof(abd)};
    auto elem = target ? target->element.try_as<IUIAutomationElement>() : winrt::com_ptr<IUIAutomationElement>{};
    BOOL offscreen = TRUE, enabled = FALSE;
    HRESULT state = elem ? elem->get_CurrentIsOffscreen(&offscreen) : UIA_E_ELEMENTNOTAVAILABLE;
    if (elem)
        elem->get_CurrentIsEnabled(&enabled);
    Log(L"tray: click begin action=%s tip='%s' exe='%s' identity=%s source=%s match=%s taskbarVisible=%d autoHide=%d overflowExists=%d "
        L"overflowVisible=%d offscreen=%d enabled=%d stateHr=0x%08X",
        ActionName(action), TrayModel::Title(icon.tip).c_str(), icon.exe.c_str(), icon.recordId.c_str(), elem ? L"uia" : L"record",
        target ? tray::MethodName(target->method) : L"missing", IsWindowVisible(taskbar), (AppBarMessage(ABM_GETSTATE, abd) & ABS_AUTOHIDE) != 0,
        overflow != nullptr, IsWindowVisible(overflow), offscreen, enabled, unsigned(state));
}

} // namespace

namespace tray_detail {

bool WaitStep(HANDLE stopEvent, DWORD ms)
{
    if (stopEvent)
        return WaitForSingleObject(stopEvent, ms) != WAIT_OBJECT_0;
    if (HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr)) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, ms + 50);
        CloseHandle(timer);
    }
    return true;
}

tray::ShellSession ReadShellSession()
{
    tray::ShellSession session;
    const HWND window = ShellTaskbar();
    DWORD pid = 0;
    if (!window || !GetWindowThreadProcessId(window, &pid))
        return session;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user))
            session = {pid, (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime, uint64_t(reinterpret_cast<uintptr_t>(window))};
        CloseHandle(process);
    }
    return session;
}

std::vector<Found> FindButtons(IUIAutomation* uia, bool& available)
{
    std::vector<Found> out;
    available = false;
    CollectHostButtons(uia, ShellTaskbar(), true, true, out, available);
    CollectHostButtons(uia, FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr), false, true, out, available);
    if (!out.empty())
        return out;
    HWND tray = ShellTaskbar();
    HWND notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    HWND pager = notify ? FindWindowExW(notify, nullptr, L"SysPager", nullptr) : nullptr;
    CollectHostButtons(uia, pager ? FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr) : nullptr, true, false, out, available);
    HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    CollectHostButtons(uia, overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr, false, false, out, available);
    return out;
}

HWND OverflowWindow()
{
    HWND h = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    return h ? h : FindWindowW(L"NotifyIconOverflowWindow", nullptr);
}

bool ShowOverflow(IUIAutomation* uia, HANDLE stopEvent)
{
    if (OverflowUsable(uia))
        return true;
    HWND tray = ShellTaskbar();
    HWND bridge = tray ? FindWindowExW(tray, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) : nullptr;
    if (tray && !bridge)
        return ShowClassicOverflow(uia, tray, stopEvent);
    winrt::com_ptr<IUIAutomationElement> chevron = FindOverflowChevron(uia, bridge);
    if (!chevron)
        return false;
    winrt::com_ptr<IUIAutomationInvokePattern> invoke;
    HRESULT hr = chevron->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.put()));
    if (SUCCEEDED(hr))
        hr = invoke ? invoke->Invoke() : E_NOINTERFACE;
    Log(L"tray: overflow chevron hr=0x%08X taskbarVisible=%d", unsigned(hr), IsWindowVisible(tray));
    if (FAILED(hr))
        return false;
    const bool usable = WaitOverflowUsable(uia, stopEvent);
    Log(usable ? L"tray: overflow verified visible content and hit target" : L"tray: overflow accepted but content not usable");
    return usable;
}

} // namespace tray_detail

DWORD TrayModel::LastSyntheticInput() { return g_syntheticInput; }

void TrayModel::MiddleClick(const TrayIcon& icon, RECT cell)
{
    POINT at{};
    if (host_ && !icon.hostKey.empty() && GetCursorPos(&at))
        host_->Hover(L"", at), host_->Send(icon.hostKey, tray::HostAction::Middle, at, cell);
}

void TrayModel::Hover(const TrayIcon* icon, RECT cell)
{
    POINT at{};
    if (host_ && GetCursorPos(&at))
        host_->Hover(icon ? icon->hostKey : L"", at, cell);
}

void TrayModel::EnqueueUrgent(Job job)
{
    std::lock_guard g(mutex_);
    jobs_.insert(std::ranges::find_if(jobs_, [](const Job& j) { return j.refresh; }), std::move(job));
    cv_.notify_all();
}

bool TrayModel::ClickHosted(const TrayIcon& icon, tray::HostAction hostAction, RECT cell, bool allowProgramWake)
{
    if (icon.hostKey.empty())
        return false;
    POINT at{};
    GetCursorPos(&at);
    if (host_)
        host_->Hover(L"", at);
    const HostSendStatus status = host_ ? host_->Send(icon.hostKey, hostAction, at, cell) : HostSendStatus::NotSent;
    if (status == HostSendStatus::Sent || status == HostSendStatus::Partial)
        return true;
    const tray::Action action = hostAction == tray::HostAction::Right              ? tray::Action::Right :
                                hostAction == tray::HostAction::Double             ? tray::Action::Double :
                                hostAction == tray::HostAction::DoubleContinuation ? tray::Action::DoubleContinuation :
                                                                                     tray::Action::Left;
    // Fall back to Explorer only on NotSent and only through a strict 1-to-1 Explorer binding.
    TrayIcon fallback;
    if (icon.explorerTarget) {
        fallback.tip = icon.explorerTarget->tip;
        fallback.exe = icon.explorerTarget->exe;
        fallback.recordId = icon.explorerTarget->identity;
        fallback.runtimeId = icon.explorerTarget->runtime;
        fallback.shell = icon.explorerTarget->shell;
        fallback.explorerTarget = icon.explorerTarget;
    } else {
        fallback.tip = icon.tip;
        fallback.exe = icon.exe;
        fallback.recordId = icon.recordId;
        fallback.shell = icon.shell;
    }
    EnqueueUrgent({false, std::move(fallback), action, false, allowProgramWake});
    return true;
}

void TrayModel::Click(const TrayIcon& icon, bool right, RECT cell)
{
    if (!ClickHosted(icon, right ? tray::HostAction::Right : tray::HostAction::Left, cell))
        EnqueueUrgent({false, icon, right ? tray::Action::Right : tray::Action::Left});
}

void TrayModel::DoubleClick(const TrayIcon& icon, bool continuation, RECT cell)
{
    if (!ClickHosted(icon, continuation ? tray::HostAction::DoubleContinuation : tray::HostAction::Double, cell))
        EnqueueUrgent({false, icon, continuation ? tray::Action::DoubleContinuation : tray::Action::Double});
}

void TrayModel::OpenSystemTray() { EnqueueUrgent({false, {}, tray::Action::Left, true}); }

bool TrayModel::OpenExisting(const std::wstring& exe, DWORD preferredPid)
{
    if (exe.empty())
        return false;
    const std::vector<TrayIcon> current = Icons();
    const TrayIcon* chosen = nullptr;
    for (const TrayIcon& icon : current) {
        if (icon.exe.empty() || _wcsicmp(AppExecutable(icon.exe).c_str(), exe.c_str()) != 0)
            continue;
        if (preferredPid && icon.pid == preferredPid) {
            chosen = &icon;
            break;
        }
        if (!chosen || (!chosen->flashing && icon.flashing))
            chosen = &icon;
    }
    if (chosen)
        return ClickHosted(*chosen, tray::HostAction::Open, {}, true) || (EnqueueUrgent({false, *chosen, tray::Action::Left, false, true}), true);
    std::wstring lowerExe = exe;
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, lowerExe.c_str(), int(lowerExe.size()), lowerExe.data(), int(lowerExe.size()), nullptr, nullptr, 0);
    std::wstring fileName = PathFindFileNameW(lowerExe.c_str());
    if (MessengerOfProgram(fileName) == Messenger::None || !detail::ProcessRuns(exe))
        return false;
    TrayIcon icon;
    icon.exe = std::move(lowerExe);
    for (const TrayIcon& t : current)
        if (t.exe == icon.exe) {
            icon = t;
            break;
        }
    return ClickHosted(icon, tray::HostAction::Open, {}, true) || (EnqueueUrgent({false, icon, tray::Action::Left, false, true}), true);
}

void TrayModel::RunClickJob(IUIAutomation* uia, const Job& job, const tray::ShellSession& initialSession, ULONGLONG findStart)
{
    tray::ShellSession session = initialSession;
    bool available = false, ordered = false;
    std::vector<tray_detail::Record> records;
    std::vector<tray_detail::Found> buttons;
    tray::ResolvedTarget resolved;
    const auto appNames = appNames_ ? appNames_() : std::vector<PinnedApp>{};
    auto acquire = [&]() {
        buttons = tray_detail::FindButtons(uia, available);
        auto match = tray_detail::MatchButtons(buttons, records, ordered, appNames);
        resolved = ResolveClickTarget(job.icon, session, buttons, match, records);
        return resolved.index == tray::kNone ? nullptr : &buttons[resolved.index];
    };
    tray_detail::Found* target = acquire();
    LogClickBegin(job.action, job.icon, target);
    const DWORD targetPid = job.icon.pid ? job.icon.pid : tray::RecordPid(job.icon.recordId);
    const auto before = CaptureResponse(job.icon.exe, targetPid, job.action);
    auto observe = [&](bool accepted) { return ObserveTrayResponse(job.icon.exe, targetPid, job.action, before, accepted, stopEvent_); };
    auto invoke = [&](tray::Route route) { return InvokeTrayRoute(target, session, job.action, route); };
    auto result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
    if (target && target->element && result.outcome == tray::Outcome::Unavailable && !result.attempts.empty() &&
        result.attempts.front().status == int32_t(UIA_E_ELEMENTNOTAVAILABLE)) {
        session = tray_detail::ReadShellSession();
        target = acquire();
        Log(L"tray: click reacquire resolved=%d", target != nullptr);
        result = tray::Dispatch(job.action, target && target->toolbar, invoke, observe);
    }
    if (job.action == tray::Action::Right && target && target->element && tray::ShouldAttemptPointerRightFallback(result, target->toolbar != nullptr) &&
        session == tray_detail::ReadShellSession()) {
        const HRESULT hr = PointerRightClick(uia, *target, stopEvent_, notify_, msg_);
        Log(L"tray: click attempt route=pointer hr=0x%08X", unsigned(hr));
        if (hr == S_OK)
            result.outcome = observe(true) ? tray::Outcome::Observed : tray::Outcome::AcceptedUnconfirmed;
        else if (hr == E_ABORT && result.outcome == tray::Outcome::Unavailable)
            result.outcome = tray::Outcome::AcceptedUnconfirmed;
    }
    for (const auto& attempt : result.attempts)
        Log(L"tray: click evidence route=%s observed=%d", RouteName(attempt.route), attempt.observed);
    if (result.outcome == tray::Outcome::AcceptedUnconfirmed) {
        Log(L"tray: click end result=accepted-unconfirmed elapsed=%llu ms", GetTickCount64() - findStart);
        PostMessageW(notify_, msg_, 4, 0);
        return;
    }
    bool observed = result.outcome == tray::Outcome::Observed;
    if (!observed && job.action != tray::Action::Right && !job.icon.exe.empty() && !tray::IsShellExplorerExe(job.icon.exe) &&
        tray::ShouldAllowExeForegroundFallback(job.allowProgramWake))
        observed = ForegroundExisting(job.icon.exe, stopEvent_), Log(L"tray: click foreground-existing observed=%d", observed);
    const bool shown = !observed && tray_detail::ShowOverflow(uia, stopEvent_);
    Log(L"tray: click end result=%s elapsed=%llu ms", observed ? L"observed" : shown ? L"system-tray" : L"needs-user", GetTickCount64() - findStart);
    PostMessageW(notify_, msg_, observed ? 1 : shown ? 2 : 3, 0);
}

} // namespace atrium
