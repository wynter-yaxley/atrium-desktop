#include "shell/tray_host.h"

#include "log.h"
#include "shell/shell_icon.h"
#include "shell/shell_taskbar.h"
#include "shell/tray_host_wire.h"
#include "shell/tray_match.h"
#include "win32_path.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <iterator>
#include <optional>

namespace atrium {

namespace {

using tray_wire::Handle, tray_wire::HashIconImage, tray_wire::KeyOf, tray_wire::kSignature;
using tray_wire::NotifyIconData32, tray_wire::NotifyIconIdentifier32, tray_wire::TrayNotifyData;

std::atomic<TrayHost*> g_host{nullptr}; // the running host, for its event hook

constexpr UINT_PTR kKeepTimer = 1;      // keep the host first among Shell_TrayWnd windows; post changes
constexpr UINT kKeepMs = 100;
constexpr UINT_PTR kPruneTimer = 2;     // icons of windows that are gone without a NIM_DELETE
constexpr UINT kPruneMs = 2000;
constexpr UINT_PTR kPopupTimer = 3;     // the dwell before a popup request (NIN_POPUPOPEN)
constexpr UINT kPopupMs = 500;
constexpr UINT_PTR kRecollectTimer = 4; // after Explorer's own TaskbarCreated, the host's
constexpr UINT kRecollectMs = 1500;
constexpr DWORD kForwardMs = 4000;      // as long as an app waits for Explorer anyway
constexpr uint64_t kImageBase = 1ull << 62; // apart from TrayModel's own picture versions
constexpr wchar_t kTaskband[] = L"TaskbandHWND"; // Explorer's MSTaskSwWClass, on its Shell_TrayWnd

std::wstring ProcessPath(DWORD pid)
{
    std::wstring out;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD size = DWORD(std::size(path));
        if (QueryFullProcessImageNameW(p, 0, path, &size))
            out.assign(path, size);
        CloseHandle(p);
    }
    return out;
}

DWORD PidOf(HWND h)
{
    DWORD pid = 0;
    if (h)
        GetWindowThreadProcessId(h, &pid);
    return pid;
}

// The shell's process: the desktop's (Explorer, or whatever shell Windows runs).
DWORD ShellPid() { return PidOf(GetShellWindow()); }

bool HasStartAllBackModule(DWORD shellPid)
{
    HANDLE snap = shellPid ? CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, shellPid) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    MODULEENTRY32W me{sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = _wcsnicmp(me.szModule, L"StartAllBack", 12) == 0 || _wcsnicmp(me.szModule, L"StartIsBack", 11) == 0;
    return CloseHandle(snap), found;
}

bool StartAllBackDisabled()
{
    DWORD disabled = 0, size = sizeof(disabled);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\StartIsBack", L"Disabled", RRF_RT_REG_DWORD, nullptr, &disabled, &size) == ERROR_SUCCESS &&
           disabled != 0;
}

const wchar_t* Name(DWORD message)
{
    return message == NIM_ADD        ? L"add" :
           message == NIM_MODIFY     ? L"modify" :
           message == NIM_DELETE     ? L"delete" :
           message == NIM_SETFOCUS   ? L"focus" :
           message == NIM_SETVERSION ? L"version" :
                                       L"?";
}

const wchar_t* Name(tray::HostAction action)
{
    return action == tray::HostAction::Left               ? L"click" :
           action == tray::HostAction::Right              ? L"right click" :
           action == tray::HostAction::Middle             ? L"middle click" :
           action == tray::HostAction::Double             ? L"double click" :
           action == tray::HostAction::DoubleContinuation ? L"second click" :
           action == tray::HostAction::Open               ? L"open" :
                                                            L"hover";
}

} // namespace

TrayHost::TrayHost(HWND notify, UINT msg, DWORD readyWaitMs, DWORD startDelayMs, std::function<void()> onYieldChange, std::function<void()> onIconsChange)
    : notify_(notify),
      msg_(msg),
      startDelayMs_(startDelayMs),
      onYieldChange_(std::move(onYieldChange)),
      onIconsChange_(std::move(onIconsChange)),
      taskbarCreated_(RegisterWindowMessageW(L"TaskbarCreated")),
      quit_(RegisterWindowMessageW(L"Atrium.TrayHost.Quit")),
      hover_(RegisterWindowMessageW(L"Atrium.TrayHost.Hover")),
      ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    thread_ = std::thread([this] { Run(); });
    if (ready_)
        WaitForSingleObject(ready_, readyWaitMs);
}

TrayHost::~TrayHost()
{
    stopping_ = true;
    if (HWND w = window_.load()) {
        // Behind Explorer's taskbar first, so that calls from now on go to Explorer; the
        // ones already on their way are answered before the window goes.
        SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (stop_)
            WaitForSingleObject(stop_, 100);
        PostMessageW(w, quit_, 0, 0);
    }
    if (stop_)
        SetEvent(stop_);
    if (thread_.joinable())
        thread_.join();
    if (ready_)
        CloseHandle(ready_);
    if (stop_)
        CloseHandle(stop_);
}

void TrayHost::Run()
{
    if (!stop_ || WaitForSingleObject(stop_, startDelayMs_) == WAIT_OBJECT_0) {
        if (ready_)
            SetEvent(ready_);
        return;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc), 0, Proc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"Shell_TrayWnd", nullptr};
    WNDCLASSEXW child{sizeof(child), 0, DefWindowProcW, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"TrayNotifyWnd", nullptr};
    const bool registered = RegisterClassExW(&wc) != 0;
    DWORD error = registered ? 0 : GetLastError();
    RegisterClassExW(&child);
    RECT r{};
    if (HWND shell = ShellTaskbar())
        GetWindowRect(shell, &r);
    HWND w = nullptr;
    if (registered && WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"Shell_TrayWnd", L"", WS_POPUP, r.left, r.top, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, instance, this);
        error = w ? error : GetLastError();
    }
    if (!w) {
        if (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0)
            Log(L"tray host: no window (%lu)", error);
        UnregisterClassW(L"Shell_TrayWnd", instance), UnregisterClassW(L"TrayNotifyWnd", instance);
        if (ready_)
            SetEvent(ready_);
        return;
    }
    CreateWindowExW(0, L"TrayNotifyWnd", L"", WS_CHILD, 0, 0, r.right - r.left, r.bottom - r.top, w, nullptr, instance, nullptr);
    ChangeWindowMessageFilterEx(w, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(w, taskbarCreated_, MSGFLT_ALLOW, nullptr);
    window_ = w;
    // Explorer's taskbar comes before the host when it is shown or activated: back at
    // once on those events (the timer below is the fallback), so that no app's call
    // goes past the host meanwhile.
    g_host = this;
    HWINEVENTHOOK shown = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, ShellEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK foreground =
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ShellEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    KeepFirst();
    SetTimer(w, kKeepTimer, kKeepMs, nullptr), SetTimer(w, kPruneTimer, kPruneMs, nullptr);
    if (ready_)
        SetEvent(ready_);
    if (!yielded_ && !stopping_)
        Broadcast();
    MSG m;
    for (bool quit = false; !quit && WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0;) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT || WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) {
                quit = true;
                break;
            }
            TranslateMessage(&m), DispatchMessageW(&m);
        }
        if (!quit && WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(1, &stop_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
                break;
        }
    }
    if (IsWindow(w))
        RemovePropW(w, kTaskband), DestroyWindow(w);
    if (shown)
        UnhookWinEvent(shown);
    if (foreground)
        UnhookWinEvent(foreground);
    g_host = nullptr, window_ = nullptr;
    UnregisterClassW(L"Shell_TrayWnd", instance), UnregisterClassW(L"TrayNotifyWnd", instance);
    Log(L"tray host: off");
}

// The apps already running add their icons again, now through the host.
void TrayHost::Broadcast()
{
    broadcastAt_ = GetTickCount64();
    SendNotifyMessageW(HWND_BROADCAST, taskbarCreated_, 0, 0);
    Log(L"tray host: on, apps asked to add their icons again");
}

void CALLBACK TrayHost::ShellEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG, DWORD, DWORD)
{
    if (object != OBJID_WINDOW || !hwnd)
        return;
    wchar_t cls[32] = {};
    if (!GetClassNameW(hwnd, cls, 32) || wcscmp(cls, L"Shell_TrayWnd") != 0)
        return;
    if (TrayHost* host = g_host.load())
        host->KeepFirst(); // the host's own thread: the hook is out of context
}

LRESULT CALLBACK TrayHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<TrayHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    return self->Message(h, m, w, l);
}

void TrayHost::OnTimer(HWND h, WPARAM w)
{
    if (w == kKeepTimer) {
        KeepFirst();
        {
            std::lock_guard g(mutex_);
            const ULONGLONG now = GetTickCount64();
            std::vector<std::wstring> flashing;
            for (const auto& [key, icon] : icons_)
                if (icon.flash.until > now)
                    flashing.push_back(key);
            if (flashing != flashingKeys_)
                flashingKeys_ = std::move(flashing), dirty_ = true;
        }
        if (dirty_.exchange(false)) {
            LogList();
            if (onIconsChange_)
                onIconsChange_();
            else if (notify_)
                PostMessageW(notify_, msg_, 0, 0);
        }
    } else if (w == kPruneTimer) {
        Prune();
    } else if (w == kPopupTimer) {
        KillTimer(h, kPopupTimer), Popup(true);
    } else if (w == kRecollectTimer) {
        KillTimer(h, kRecollectTimer);
        if (!yielded_)
            Broadcast();
    }
}

LRESULT TrayHost::Message(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
        if (cds && cds->dwData == 1)
            return OnNotifyIcon(w, cds);
        if (cds && cds->dwData == 3)
            return OnIconRect(w, cds);
        return Forward(m, w, l); // app bars (0) and the rest: Explorer's
    }
    case WM_TIMER: OnTimer(h, w); return 0;
    case WM_WINDOWPOSCHANGING: reinterpret_cast<WINDOWPOS*>(l)->flags &= ~SWP_SHOWWINDOW; return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    if (m == quit_ && InSendMessageEx(nullptr) == ISMEX_NOSEND) { // posted by ~TrayHost
        RemovePropW(h, kTaskband), DestroyWindow(h);
        return 0;
    }
    if (m == hover_) {
        std::wstring key;
        POINT at{};
        {
            std::lock_guard g(mutex_);
            key = hoverKey_, at = hoverAt_;
        }
        if (key != popupKey_)
            Popup(false);
        KillTimer(h, kPopupTimer);
        if (!key.empty())
            Send(key, tray::HostAction::HoverIn, at), SetTimer(h, kPopupTimer, kPopupMs, nullptr);
        return 0;
    }
    if (m == taskbarCreated_) {
        // The host's own broadcast comes back here; any other is Explorer restarting. Its
        // new taskbar may have come first for a moment, and the apps' new icons gone to it
        // only: once in front again the host asks for them once more.
        if (GetTickCount64() - broadcastAt_ > 3000) {
            Log(L"tray host: Explorer restarted");
            KeepFirst();
            if (!yielded_)
                SetTimer(h, kRecollectTimer, kRecollectMs, nullptr);
        }
        return 0;
    }
    // A close, a command, a private message of the taskbar: Explorer's, as if sent to it.
    if (m == WM_CLOSE || m == WM_COMMAND || m == WM_SYSCOMMAND || m >= WM_USER)
        return Forward(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT TrayHost::Forward(UINT m, WPARAM w, LPARAM l, BOOL* sent, DWORD* err)
{
    HWND shell = ShellTaskbar();
    if (!shell)
        return 0;
    if (m == WM_COPYDATA || InSendMessage()) {
        DWORD_PTR result = 0;
        const LRESULT ok = SendMessageTimeoutW(shell, m, w, l, SMTO_ABORTIFHUNG, kForwardMs, &result);
        if (sent)
            *sent = ok != 0;
        if (err)
            *err = ok ? 0 : GetLastError();
        return ok ? LRESULT(result) : 0;
    }
    PostMessageW(shell, m, w, l);
    return 0;
}

LRESULT TrayHost::OnNotifyIcon(WPARAM w, const COPYDATASTRUCT* cds)
{
    const auto* data = static_cast<const TrayNotifyData*>(cds->lpData);
    if (!data || cds->cbData < offsetof(TrayNotifyData, nid.szInfo) || data->signature != kSignature)
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    const NotifyIconData32& nid = data->nid;
    const bool hasGuid = (nid.uFlags & NIF_GUID) && cds->cbData >= offsetof(TrayNotifyData, nid.guidItem) + sizeof(GUID) && nid.guidItem != GUID{};
    const HWND hwnd = Handle(nid.hWnd);
    const std::wstring key = KeyOf(hwnd, nid.uID, hasGuid ? &nid.guidItem : nullptr);

    // The picture is the app's: copied now, while the app waits for this call.
    std::shared_ptr<const IconImage> image;
    if ((data->message == NIM_ADD || data->message == NIM_MODIFY) && (nid.uFlags & NIF_ICON) && nid.hIcon) {
        IconImage img;
        if (HIconToImage(reinterpret_cast<HICON>(static_cast<ULONG_PTR>(nid.hIcon)), img))
            image = std::make_shared<const IconImage>(std::move(img));
    }

    // Explorer first: what it answers is what the app is told.
    HWND shell = ShellTaskbar();
    BOOL sent = FALSE;
    DWORD err = 0;
    LRESULT forwardResult = Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds), &sent, &err);
    if (data->message == NIM_ADD && !forwardResult && shell) {
        // The host's TaskbarCreated (not a new Explorer): Explorer has the icon already
        // and refuses a second add. It is the same icon, brought up to date.
        std::vector<BYTE> copy(static_cast<const BYTE*>(cds->lpData), static_cast<const BYTE*>(cds->lpData) + cds->cbData);
        auto* again32 = reinterpret_cast<TrayNotifyData*>(copy.data());
        again32->message = NIM_MODIFY;
        // Apps adding their icon again often leave the tooltip empty (QQ, Feishu): Explorer
        // keeps the one it has rather than losing it to the host's broadcast.
        if ((again32->nid.uFlags & NIF_TIP) && !again32->nid.szTip[0])
            again32->nid.uFlags &= ~NIF_TIP;
        COPYDATASTRUCT again{cds->dwData, cds->cbData, copy.data()};
        forwardResult = Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(&again), &sent, &err);
    }
    const bool knownIcon = !forwardResult && HasIcon(key, hasGuid, hwnd, nid.uID);
    const bool validWindow = hwnd && IsWindow(hwnd) && PidOf(hwnd) != 0 && PidOf(hwnd) != ShellPid();
    const LRESULT result = tray::DecideHostForwardResult(data->message, shell != nullptr, forwardResult, knownIcon, validWindow);
    if (shell && !forwardResult && !loggedRefusedForward_) {
        loggedRefusedForward_ = true;
        const DWORD targetPid = PidOf(shell);
        const std::wstring targetPath = ProcessPath(targetPid);
        Log(L"tray host: forward refused msg=%s target=0x%p(%s pid=%lu) sent=%d err=%lu res=%ld accepted=%d", Name(data->message), static_cast<void*>(shell),
            targetPath.empty() ? L"-" : PathFindFileNameW(targetPath.c_str()), targetPid, sent ? 1 : 0, err, long(forwardResult), result ? 1 : 0);
    }
    Record(data->message, cds->cbData, &nid, hwnd, hasGuid, key, std::move(image), result);
    return result;
}

void TrayHost::Record(DWORD message, DWORD cbData, const void* nidPtr, HWND hwnd, bool hasGuid, const std::wstring& key, std::shared_ptr<const IconImage> image,
                      LRESULT result)
{
    const auto& nid = *static_cast<const NotifyIconData32*>(nidPtr);
    const bool hasVersion = cbData >= offsetof(TrayNotifyData, nid.szInfoTitle);

    std::lock_guard g(mutex_);
    auto it = FindIconLocked(key, hasGuid, hwnd, nid.uID);
    if (message == NIM_DELETE) {
        if (it != icons_.end()) {
            icons_.erase(it);
            dirty_ = true;
        }
        return;
    }
    if (message == NIM_SETVERSION) {
        if (it != icons_.end() && hasVersion) {
            it->second.version = nid.uVersion;
            dirty_ = true;
        }
        return;
    }
    if (message != NIM_ADD && message != NIM_MODIFY)
        return;
    if (it == icons_.end()) {
        // A change of an icon the host has not seen (added before it started, the app
        // ignored TaskbarCreated) that Explorer took: the host learns it from here.
        if (!result)
            return;
        // The shell's own icons (volume, network...) stay Explorer's: Atrium has its control centre.
        const DWORD pid = PidOf(hwnd);
        if (!pid || pid == ShellPid())
            return;
        HostIcon icon;
        icon.key = key, icon.added = ++nextAdded_;
        it = icons_.emplace(key, std::move(icon)).first;
    }
    HostIcon& icon = it->second;
    if (hwnd && icon.hwnd != hwnd) {
        icon.hwnd = hwnd, icon.pid = PidOf(hwnd);
        if (const std::wstring path = ProcessPath(icon.pid); !path.empty())
            icon.exe = FoldCase(AppExecutable(path));
    }
    icon.uid = nid.uID;
    if (nid.uFlags & NIF_MESSAGE)
        icon.callback = nid.uCallbackMessage;
    if ((nid.uFlags & NIF_ICON) && image) {
        icon.imageVersion = kImageBase + ++nextImage_;
        tray::RecordIconPicture(icon.flash, GetTickCount64(), HashIconImage(*image));
        icon.image = std::move(image), icon.flashUntil = icon.flash.until;
    }
    if (nid.uFlags & NIF_TIP) {
        std::wstring tip = tray::RepairMojibakeTip(std::wstring(nid.szTip, wcsnlen(nid.szTip, std::size(nid.szTip))));
        if (!tip.empty() || icon.tip.empty())
            icon.tip = std::move(tip);
    }
    icon.showTip = (nid.uFlags & NIF_SHOWTIP) != 0 || icon.showTip;
    if ((nid.uFlags & NIF_STATE) && (nid.dwStateMask & NIS_HIDDEN))
        icon.hidden = (nid.dwState & NIS_HIDDEN) != 0;
    if (message == NIM_ADD)
        Log(L"tray host: add '%s' (%s) -> %ld", icon.tip.substr(0, icon.tip.find(L'\n')).c_str(), icon.exe.empty() ? L"-" : PathFindFileNameW(icon.exe.c_str()),
            long(result));
    dirty_ = true;
}

LRESULT TrayHost::OnIconRect(WPARAM w, const COPYDATASTRUCT* cds)
{
    const auto* id = static_cast<const NotifyIconIdentifier32*>(cds->lpData);
    if (!id || cds->cbData < sizeof(NotifyIconIdentifier32) || id->signature != kSignature || g_shellQuestions > 0)
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    const HWND hwnd = Handle(id->hWnd);
    const bool hasGuid = id->guidItem != GUID{};
    RECT r{};
    {
        std::lock_guard g(mutex_);
        auto it = FindIconLocked(KeyOf(hwnd, id->uID, hasGuid ? &id->guidItem : nullptr), hasGuid, hwnd, id->uID);
        r = tray::ResolveHostIconRect(it != icons_.end() ? it->first : std::wstring{}, clickedKey_, clicked_, hoverKey_, hovered_, anchor_);
    }
    if (IsRectEmpty(&r))
        return Forward(WM_COPYDATA, w, reinterpret_cast<LPARAM>(cds));
    return id->message == 1 ? MAKELONG(WORD(SHORT(r.left)), WORD(SHORT(r.top))) : MAKELONG(WORD(SHORT(r.right)), WORD(SHORT(r.bottom)));
}

// Shell_NotifyIcon finds the first Shell_TrayWnd in the z-order. Explorer's comes
// before the host now and then (its taskbar is topmost too): the host moves back.
// Another program's (RetroBar, ExplorerPatcher's, a second shell) is left in front:
// two hosts pushing in turn would take the icons from both.
void TrayHost::KeepFirst()
{
    HWND w = window_.load();
    if (!w || stopping_ || g_hostAside > 0)
        return;
    HWND shell = ShellTaskbar();
    RECT want{}, have{};
    if (shell && GetWindowRect(shell, &want) && GetWindowRect(w, &have) && !EqualRect(&want, &have))
        SetWindowPos(w, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    // ITaskbarList::HrInit takes Explorer's taskband from the first Shell_TrayWnd, else fails with
    // E_NOTIMPL: apps that show progress on their button would not start (TeraCopy #90, NanaZip #86).
    if (HANDLE band = shell ? GetPropW(shell, kTaskband) : nullptr; band != GetPropW(w, kTaskband))
        SetPropW(w, kTaskband, band);
    const DWORD shellPid = ShellPid();
    const bool wasYieldedBefore = yielded_.load();
    if (shellPid != yieldPolicy_.shellPid) {
        if (yieldPolicy_.OnShellPid(shellPid, HasStartAllBackModule(shellPid), StartAllBackDisabled()))
            yielded_ = false, dirty_ = true;
    } else if (yieldPolicy_.hasStartAllBackModule && yieldPolicy_.OnStartAllBackActive(ShouldYieldToStartAllBack(true, StartAllBackDisabled()))) {
        yielded_ = false, dirty_ = true;
        Log(L"tray host: StartAllBack disabled for current user; resuming");
    }
    HWND first = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (yieldPolicy_.hasStartAllBack) {
        yielded_ = true;
        KillTimer(w, kRecollectTimer);
        if (!wasYieldedBefore)
            Log(L"tray host: StartAllBack detected in explorer.exe (pid %lu); yielding", shellPid), dirty_ = true;
        if (first == w || (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
            SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            first = FindWindowW(L"Shell_TrayWnd", nullptr);
        }
    } else if (first == w) {
        if (yieldPolicy_.OnFirst() || wasYieldedBefore) {
            yielded_ = false, dirty_ = true;
            Log(L"tray host: in front again");
            SetTimer(w, kRecollectTimer, 200, nullptr); // updates went past the host meanwhile
        }
    } else if (const DWORD owner = PidOf(first); first && owner != GetCurrentProcessId() && owner != shellPid) {
        if (yieldPolicy_.OnOtherProcess()) {
            yielded_ = true, dirty_ = true;
            Log(L"tray host: another program's tray (pid %lu) is in front: left there", owner);
        }
    } else {
        HandleExplorerFront(w, GetTickCount64(), wasYieldedBefore);
    }
    FinishKeepFirst(first, shell, shellPid, wasYieldedBefore);
}

void TrayHost::HandleExplorerFront(HWND w, ULONGLONG now, bool wasYielded)
{
    const bool inAsideGrace = now < g_hostAsideGraceUntil.load(std::memory_order_acquire);
    if (!inAsideGrace || yieldPolicy_.Reason() != TrayHostYieldReason::None) {
        bool newlyYielded = false;
        if (!yieldPolicy_.OnExplorerFirst(now, newlyYielded)) {
            yielded_ = true;
            if (newlyYielded) {
                const ULONGLONG backoffSec = yieldPolicy_.retryAt > now ? (yieldPolicy_.retryAt - now) / 1000 : 0;
                Log(L"tray host: Explorer's taskbar repeatedly reclaimed first (%d times); backing off %llus", yieldPolicy_.rapidLosses, backoffSec);
                dirty_ = true;
            }
            return;
        }
    }
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    bool again = FindWindowW(L"Shell_TrayWnd", nullptr) == w;
    if (!again && (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        SetWindowPos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        again = FindWindowW(L"Shell_TrayWnd", nullptr) == w;
    }
    if (again)
        yieldPolicy_.OnFirst();
    const bool nowYielded = yieldPolicy_.Yielded();
    if (nowYielded != wasYielded) {
        yielded_ = nowYielded, dirty_ = true;
        if (!nowYielded && again)
            Log(L"tray host: in front again"), SetTimer(w, kRecollectTimer, 200, nullptr);
    }
    if (now - lostAt_ > 60000)
        Log(L"tray host: Explorer's taskbar came first; moved back (%s)", again ? L"first again" : L"still second");
    lostAt_ = now;
}

const wchar_t* YieldReasonName(TrayHostYieldReason reason)
{
    return reason == TrayHostYieldReason::OtherProcess ? L"other-process" :
           reason == TrayHostYieldReason::RapidLoss    ? L"rapid-loss" :
           reason == TrayHostYieldReason::StartAllBack ? L"startallback" :
                                                         L"none";
}

void LogShellTrayWindows()
{
    std::vector<std::wstring> lines;
    std::wstring signature;
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)) != nullptr;) {
        const DWORD pid = PidOf(h);
        const std::wstring path = ProcessPath(pid);
        wchar_t line[256];
        swprintf_s(line, L"tray diag: Shell_TrayWnd[%zu] hwnd=0x%p pid=%lu exe=%s visible=%d", lines.size(), static_cast<void*>(h), pid,
                   path.empty() ? L"-" : PathFindFileNameW(path.c_str()), IsWindowVisible(h) ? 1 : 0);
        lines.emplace_back(line);
        signature += std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L":" + std::to_wstring(pid) + (IsWindowVisible(h) ? L"+;" : L"-;");
    }
    static std::mutex mutex;
    static std::optional<std::wstring> logged;
    {
        std::lock_guard g(mutex);
        if (logged == signature)
            return;
        logged = signature;
    }
    if (lines.empty())
        Log(L"tray diag: no Shell_TrayWnd");
    for (const std::wstring& line : lines)
        Log(L"%s", line.c_str());
}

void TrayHost::FinishKeepFirst(HWND first, HWND shell, DWORD shellPid, bool wasYielded)
{
    const TrayHostYieldReason reason = yieldPolicy_.Reason();
    yieldReason_ = reason;
    if (!loggedState_ || reason != lastLoggedReason_) {
        loggedState_ = true, lastLoggedReason_ = reason;
        Log(L"tray host: yielded=%d reason=%s first=0x%p shell=0x%p (pid %lu)", yielded_.load() ? 1 : 0, YieldReasonName(reason), static_cast<void*>(first),
            static_cast<void*>(shell), shellPid);
        LogShellTrayWindows();
    }
    if (yielded_.load() != wasYielded && onYieldChange_)
        onYieldChange_();
}

// The list in the log whenever its make-up changes (not on a picture or tooltip change).
void TrayHost::LogList()
{
    std::wstring keys;
    std::vector<std::wstring> lines;
    {
        std::lock_guard g(mutex_);
        for (const auto& [key, icon] : icons_) {
            keys += key + (icon.hidden ? L"h;" : L";");
            wchar_t line[400];
            swprintf_s(line, L"tray host:   %s %s v%u cb=0x%X%s%s img=%dx%d '%s'", key.c_str(), icon.exe.empty() ? L"-" : PathFindFileNameW(icon.exe.c_str()),
                       icon.version, icon.callback, icon.hidden ? L" hidden" : L"", icon.showTip ? L" showtip" : L"", icon.image ? icon.image->width : 0,
                       icon.image ? icon.image->height : 0, icon.tip.substr(0, icon.tip.find(L'\n')).substr(0, 60).c_str());
            lines.emplace_back(line);
        }
    }
    if (keys == loggedKeys_)
        return;
    loggedKeys_ = keys;
    Log(L"tray host: %zu icons", lines.size());
    for (const auto& line : lines)
        Log(L"%s", line.c_str());
}

void TrayHost::Prune()
{
    std::lock_guard g(mutex_);
    if (std::erase_if(icons_, [](const auto& item) { return item.second.hwnd && !IsWindow(item.second.hwnd); }))
        dirty_ = true;
}

std::vector<HostIcon> TrayHost::Icons() const
{
    std::lock_guard g(mutex_);
    std::vector<HostIcon> out;
    out.reserve(icons_.size());
    for (const auto& [key, icon] : icons_)
        out.push_back(icon);
    std::ranges::sort(out, [](const HostIcon& a, const HostIcon& b) { return a.added < b.added; });
    return out;
}

std::vector<std::wstring> TrayHost::Flashing() const
{
    std::lock_guard g(mutex_);
    std::vector<std::wstring> out;
    const ULONGLONG now = GetTickCount64();
    for (const auto& [key, icon] : icons_)
        if (icon.flash.until > now && !icon.exe.empty())
            out.push_back(icon.exe);
    return out;
}

std::vector<FlashingTrayIcon> TrayHost::FlashingIcons() const
{
    std::lock_guard g(mutex_);
    std::vector<FlashingTrayIcon> out;
    const ULONGLONG now = GetTickCount64();
    for (const auto& [key, icon] : icons_)
        if (icon.flash.until > now && (!icon.exe.empty() || icon.pid != 0))
            out.push_back({key, icon.exe, icon.tip, icon.pid, icon.hwnd});
    return out;
}

void TrayHost::Hover(const std::wstring& key, POINT at, RECT cell)
{
    HWND w = window_.load();
    if (!w)
        return;
    {
        std::lock_guard g(mutex_);
        if (key == hoverKey_)
            return;
        hoverKey_ = key, hoverAt_ = at, hovered_ = key.empty() ? RECT{} : tray::CellOrBox(cell, at);
    }
    PostMessageW(w, hover_, 0, 0);
}

// Host thread: a version 4 app that draws its own tooltip is asked to show it after
// the dwell (NIN_POPUPOPEN) and to close it when the pointer leaves (NIN_POPUPCLOSE).
void TrayHost::Popup(bool open)
{
    std::wstring key;
    POINT at{};
    {
        std::lock_guard g(mutex_);
        key = open ? hoverKey_ : popupKey_, at = hoverAt_;
        if (open) {
            auto it = icons_.find(key);
            if (key.empty() || it == icons_.end() || it->second.showTip || it->second.version < 4)
                return;
        }
    }
    if (!key.empty()) {
        Send(key, open ? tray::HostAction::PopupOpen : tray::HostAction::PopupClose, at);
        popupKey_ = open ? key : L"";
    }
}

HostSendStatus TrayHost::Send(const std::wstring& key, tray::HostAction action, POINT at, RECT cell)
{
    const bool click = action != tray::HostAction::HoverIn && action != tray::HostAction::PopupOpen && action != tray::HostAction::PopupClose;
    HWND hwnd = nullptr;
    UINT uid = 0, callback = 0, version = 0;
    DWORD pid = 0;
    {
        std::lock_guard g(mutex_);
        auto it = icons_.find(key);
        if (it == icons_.end())
            return HostSendStatus::NotSent;
        hwnd = it->second.hwnd, uid = it->second.uid, callback = it->second.callback, version = it->second.version, pid = it->second.pid;
        if (click)
            clickedKey_ = key, clicked_ = tray::CellOrBox(cell, at);
    }
    if (!callback || !hwnd || !IsWindow(hwnd))
        return HostSendStatus::NotSent;
    // Atrium has the foreground (its panel): the app may take it for its window or menu.
    if (click)
        AllowSetForegroundWindow(pid);
    const std::vector<UINT> seq = tray::CallbackSequence(action, version);
    if (seq.empty())
        return HostSendStatus::NotSent;
    size_t sentCount = 0;
    for (UINT notification : seq) {
        WPARAM wp = 0;
        LPARAM lp = 0;
        tray::EncodeCallback(version, uid, notification, at, wp, lp);
        if (!SendNotifyMessageW(hwnd, callback, wp, lp))
            break; // an elevated app's window refuses Atrium's messages (UIPI)
        ++sentCount;
    }
    const HostSendStatus status = sentCount == 0 ? HostSendStatus::NotSent : sentCount == seq.size() ? HostSendStatus::Sent : HostSendStatus::Partial;
    if (click)
        Log(L"tray host: %s on %s v%u -> %s", Name(action), key.c_str(), version,
            status == HostSendStatus::Sent    ? L"sent" :
            status == HostSendStatus::Partial ? L"partial" :
                                                L"refused");
    return status;
}

} // namespace atrium
