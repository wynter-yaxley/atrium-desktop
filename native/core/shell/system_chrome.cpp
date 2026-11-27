#include "shell/system_chrome.h"
#include "shell/taskbar_visibility.h"

#include "bar_edge.h"
#include "log.h"
#include "shell/shell_taskbar.h"
#include "settings.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <winreg.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

// The shell's Do Not Disturb service (undocumented; the same one the Windows
// notification centre's button uses). After IUnknown: the selected profile as
// a CoTaskMemAlloc'd string, and setting it.
constexpr CLSID kQuietHoursSettings = {0xF53321FA, 0x34F8, 0x4B7F, {0xB9, 0xA3, 0x36, 0x18, 0x77, 0xCB, 0x94, 0xCF}};
constexpr IID kIQuietHoursSettings = {0x6BFF4732, 0x81EC, 0x4FFB, {0xAE, 0x67, 0xB6, 0xC1, 0xBC, 0x29, 0x63, 0x1F}};
struct IQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(LPWSTR* profile) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(LPCWSTR profile) = 0;
};

SystemChrome* g_chrome = nullptr;
TaskbarVisibility g_taskbarVisibility;

std::wstring StatePath()
{
    std::wstring p = SettingsPath();
    return p.substr(0, p.find_last_of(L'\\')) + L"\\system.json";
}

std::vector<HWND> Taskbars()
{
    std::vector<HWND> out;
    if (HWND main = ShellTaskbar())
        out.push_back(main);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        out.push_back(w);
    return out;
}

bool IsTaskbar(HWND w)
{
    wchar_t cls[32] = {};
    GetClassNameW(w, cls, 32);
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

std::optional<bool> QueryAutoHide()
{
    APPBARDATA abd{sizeof(abd)};
    UINT_PTR state = 0;
    if (!TryAppBarMessage(ABM_GETSTATE, abd, state))
        return std::nullopt;
    return (state & ABS_AUTOHIDE) != 0;
}

bool SetAutoHide(bool on, UINT timeoutMs = 1500, UINT sendFlags = SMTO_ABORTIFHUNG)
{
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = ShellTaskbar();
    abd.lParam = on ? ABS_AUTOHIDE : ABS_ALWAYSONTOP;
    UINT_PTR unused = 0;
    return TryAppBarMessage(ABM_SETSTATE, abd, unused, timeoutMs, sendFlags, on);
}

// Windows maximized while the taskbar still held its strip of the work area keep that gap when the
// area grows: not every app lays itself out again. Each one is stretched to its work area, its
// invisible borders outside it as Windows places a maximized window. A moment later: the shell
// moves the work area after the call returns.
void RefitMaximized()
{
    EnumWindows(
        [](HWND w, LPARAM) -> BOOL {
            MONITORINFO mi{sizeof(mi)};
            RECT r{}, f{};
            if (!IsWindowVisible(w) || !IsZoomed(w) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi) || !GetWindowRect(w, &r) ||
                FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &f, sizeof(f))))
                return TRUE;
            const RECT& a = mi.rcWork;
            if (f.left <= a.left + 1 && f.top <= a.top + 1 && f.right >= a.right - 1 && f.bottom >= a.bottom - 1)
                return TRUE;
            Log(L"chrome: a maximized window stopped short of the work area (%ld,%ld,%ld,%ld); fitted", f.left, f.top, f.right, f.bottom);
            SetWindowPos(w, nullptr, r.left + a.left - f.left, r.top + a.top - f.top, (r.right + a.right - f.right) - (r.left + a.left - f.left),
                         (r.bottom + a.bottom - f.bottom) - (r.top + a.top - f.top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            return TRUE;
        },
        0);
}

void ShowTaskbars()
{
    // Also used by the exception filter: enumerate without allocating.
    if (HWND w = ShellTaskbar())
        ShowWindowAsync(w, SW_SHOWNA);
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr;)
        ShowWindowAsync(w, SW_SHOWNA);
}

bool TaskbarHoldsExactWorkEdge(HWND w, RECT* workOut = nullptr)
{
    RECT tb{};
    MONITORINFO mi{sizeof(mi)};
    const HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi) || !GetWindowRect(w, &tb))
        return false;
    if (workOut)
        *workOut = mi.rcWork;
    return WorkHoldsTaskbarEdge(tb, mi.rcMonitor, mi.rcWork);
}

std::vector<TaskbarWorkSnapshot> SnapshotTaskbarWork(bool requireDockedEdge)
{
    std::vector<TaskbarWorkSnapshot> out;
    for (HWND w : Taskbars()) {
        RECT tb{};
        MONITORINFO mi{sizeof(mi)};
        const HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONULL);
        if (mon && GetMonitorInfoW(mon, &mi) && GetWindowRect(w, &tb))
            if (auto snap = MakeTaskbarSnapshot(mon, tb, mi.rcMonitor, mi.rcWork, requireDockedEdge))
                out.push_back(*snap);
    }
    return out;
}

template<class Done> void WaitTaskbarStep(int attempts, int stepMs, const Done& done)
{
    for (int i = 0; i < attempts && !g_taskbarVisibility.stopping && !done(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
}

void WaitWorkAreaSync(const std::vector<TaskbarWorkSnapshot>& before, bool release)
{
    const HWND shell = ShellTaskbar();
    if (before.empty() || !ShellResponsive(shell, 200))
        return;
    WaitTaskbarStep(24, 15, [&] {
        return (release && !g_taskbarVisibility.ShouldHide()) || !ShellResponsive(shell, 150) || std::ranges::all_of(before, [&](const TaskbarWorkSnapshot& s) {
                   MONITORINFO mi{sizeof(mi)};
                   return !GetMonitorInfoW(s.monitor, &mi) || WorkAreaSnapshotSynced(s, mi.rcWork, release);
               });
    });
}

bool SetAutoHideSynced(bool on, bool wait)
{
    const auto before = wait ? SnapshotTaskbarWork(on) : std::vector<TaskbarWorkSnapshot>{};
    const bool ok = SetAutoHide(on);
    if (ok && wait)
        WaitWorkAreaSync(before, on);
    return ok;
}

void WaitTaskbarsVisible(bool visible)
{
    WaitTaskbarStep(20, 10, [&] {
        return (!visible && !g_taskbarVisibility.ShouldHide()) ||
               std::ranges::none_of(Taskbars(), [&](HWND w) { return (IsWindowVisible(w) != FALSE) != visible && WindowResponsive(w, 150); });
    });
}

// AutoPlay's "a drive was inserted" toast is one of the system notifications
// Windows still shows in "alarms only" (that is why the USB banner survived Do
// Not Disturb). Its per-app "show banners" switch still applies: with the banner
// off the toast stays in the notification centre, where Atrium's listener reads
// it for a card, but Windows shows no banner.
constexpr wchar_t kAutoPlayKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\Windows.SystemToast.AutoPlay";
constexpr wchar_t kShowBanner[] = L"ShowBanner";

bool AutoPlayBannerShown()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoPlayKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return true; // no per-app override: Windows shows the banner
    DWORD value = 1, size = sizeof(value), type = 0;
    const LSTATUS status = RegQueryValueExW(key, kShowBanner, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return status != ERROR_SUCCESS || type != REG_DWORD || value != 0;
}

bool SetAutoPlayBanner(bool shown)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAutoPlayKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LSTATUS status = ERROR_SUCCESS;
    if (shown) {
        // Absent is Windows' default; removing the override restores the banner.
        status = RegDeleteValueW(key, kShowBanner);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    } else {
        const DWORD value = 0;
        status = RegSetValueExW(key, kShowBanner, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void ApplyBannersEnable(BannerChromeState& state, const ChromeRecord& taskbarRecord, const BannerChromeOps& ops)
{
    const std::wstring now = ops.quietProfile();
    const QuietPlan plan = PlanQuiet(now, true, state.quietOurs, state.quietBefore);
    const bool needAutoPlay = !state.autoPlayBannerReady && ops.autoPlayBannerShown();
    ChromeRecord intended = taskbarRecord;
    intended.quiet = state.quietOurs || plan.action == QuietAction::Hide;
    intended.quietRestore = plan.action == QuietAction::Hide ? now : state.quietBefore;
    intended.autoPlayBanner = state.autoPlayBannerOurs || needAutoPlay;
    if ((plan.action == QuietAction::Hide || needAutoPlay) && !ops.saveRecord(intended)) {
        Log(L"chrome: could not save recovery record; system banners left as they are");
        return;
    }
    state.bannersHidden = true;
    if (plan.action == QuietAction::Hide) {
        if (ops.setQuietProfile(plan.profile.c_str())) {
            state.quietBefore = now;
            state.quietOurs = true;
            state.quietReady = true;
            // "Priority only" and "unrestricted" both let banners through; only
            // alarms-only hides the system's own toasts as well as apps'.
            Log(L"chrome: Do Not Disturb on (alarms only; was %s)", now.c_str());
        } else {
            state.quietReady = false;
            Log(L"chrome: Do Not Disturb could not be turned on (was %s)", now.c_str());
        }
    } else if (now.empty()) {
        state.quietReady = false;
        Log(L"chrome: Do Not Disturb is not available; banners stay");
    } else {
        state.quietReady = true;
        Log(L"chrome: Do Not Disturb already hides banners (alarms only); left as it is");
    }
    if (!state.autoPlayBannerReady) {
        if (!needAutoPlay) {
            state.autoPlayBannerReady = true;
            Log(L"chrome: AutoPlay banners already off; left as it is");
        } else if (ops.setAutoPlayBanner(false)) {
            state.autoPlayBannerOurs = true;
            state.autoPlayBannerReady = true;
            Log(L"chrome: AutoPlay banners off (the drive toast bypasses Do Not Disturb)");
        } else {
            Log(L"chrome: could not turn off AutoPlay banners");
        }
    }
    ChromeRecord actual = taskbarRecord;
    actual.quiet = state.quietOurs;
    actual.quietRestore = state.quietBefore;
    actual.autoPlayBanner = state.autoPlayBannerOurs;
    if (actual != intended && !ops.saveRecord(actual))
        Log(L"chrome: could not update recovery record");
}

void ApplyBannersDisable(BannerChromeState& state, const ChromeRecord& taskbarRecord, const BannerChromeOps& ops)
{
    state.bannersHidden = false;
    const std::wstring now = ops.quietProfile();
    const QuietPlan plan = PlanQuiet(now, false, state.quietOurs, state.quietBefore);
    state.quietReady = false;
    if (plan.action == QuietAction::Restore) {
        if (now == kQuietProfile) {
            if (ops.setQuietProfile(plan.profile.c_str())) {
                Log(L"chrome: Do Not Disturb restored (%s)", plan.profile.c_str());
                state.quietOurs = false;
                state.quietBefore.clear();
            } else {
                Log(L"chrome: Do Not Disturb could not be restored (%s)", plan.profile.c_str());
            }
        } else if (!now.empty()) {
            // The user chose another profile while it was Atrium's: leave theirs.
            Log(L"chrome: Do Not Disturb changed by the user (%s); left as it is", now.c_str());
            state.quietOurs = false;
            state.quietBefore.clear();
        } else {
            // Keep ownership so system.json / next retry restores Do Not Disturb when available.
            Log(L"chrome: Do Not Disturb is not available; keeping restore record (%s)", plan.profile.c_str());
        }
    } else {
        Log(L"chrome: Do Not Disturb off (banners shown)");
    }
    state.autoPlayBannerReady = false;
    if (state.autoPlayBannerOurs) {
        if (ops.setAutoPlayBanner(true)) {
            state.autoPlayBannerOurs = false;
            Log(L"chrome: AutoPlay banners restored");
        } else {
            // Keep the ownership so system.json (and the guard) retries it.
            Log(L"chrome: could not restore AutoPlay banners");
        }
    }
    ChromeRecord after = taskbarRecord;
    after.quiet = state.quietOurs;
    after.quietRestore = state.quietBefore;
    after.autoPlayBanner = state.autoPlayBannerOurs;
    if (!ops.saveRecord(after))
        Log(L"chrome: could not update recovery record after restoring banners");
}

void UndoQuietLeftover(const json::JsonObject& o, const UndoLeftoverOps& ops, ChromeRecord& remaining)
{
    std::wstring restore = kQuietProfileOff;
    if (o.HasKey(L"quietRestore") && o.GetNamedValue(L"quietRestore").ValueType() == json::JsonValueType::String)
        restore = std::wstring(o.GetNamedString(L"quietRestore"));
    if (restore.empty() || restore == kQuietProfile)
        restore = kQuietProfileOff;
    const std::wstring now = ops.quietProfile();
    if (now == kQuietProfile) {
        if (ops.setQuietProfile(restore.c_str())) {
            Log(L"chrome: Do Not Disturb restored (%s)", restore.c_str());
        } else {
            remaining.quiet = true;
            remaining.quietRestore = restore;
            Log(L"chrome: could not restore Do Not Disturb (%s)", restore.c_str());
        }
        return;
    }
    if (now.empty()) {
        remaining.quiet = true;
        remaining.quietRestore = restore;
        Log(L"chrome: Do Not Disturb service unavailable; keeping restore record (%s)", restore.c_str());
        return;
    }
    Log(L"chrome: Do Not Disturb changed by the user (%s); left as it is", now.c_str());
}

} // namespace

bool SaveChromeRecord(const std::wstring& path, const ChromeRecord& record)
{
    if (record.Empty()) {
        if (DeleteFileW(path.c_str()) != 0)
            return true;
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return true;
        Log(L"chrome: could not remove %s (%lu)", path.c_str(), err);
        return false;
    }
    json::JsonObject o;
    o.SetNamedValue(L"taskbarHidden", json::JsonValue::CreateBooleanValue(record.taskbarHidden));
    o.SetNamedValue(L"autoHide", json::JsonValue::CreateBooleanValue(record.autoHide));
    o.SetNamedValue(L"quiet", json::JsonValue::CreateBooleanValue(record.quiet));
    if (record.quiet && !record.quietRestore.empty())
        o.SetNamedValue(L"quietRestore", json::JsonValue::CreateStringValue(record.quietRestore));
    if (record.autoPlayBanner)
        o.SetNamedValue(L"autoPlayBanner", json::JsonValue::CreateBooleanValue(true));
    if (!WriteTextFile(path, std::wstring(o.Stringify()))) {
        Log(L"chrome: could not write %s (%lu)", path.c_str(), GetLastError());
        return false;
    }
    return true;
}

void ApplyTaskbarHidden(bool hidden, TaskbarChromeState& state, const ChromeRecord& bannerRecord, const TaskbarChromeOps& ops)
{
    if (hidden == state.hidden && (hidden || !state.autoHideOurs))
        return;
    if (hidden) {
        const auto autoHide = ops.queryAutoHide();
        const bool needAutoHide = autoHide.has_value() && !*autoHide;
        ChromeRecord intended = bannerRecord;
        intended.taskbarHidden = true;
        intended.autoHide = state.autoHideOurs || needAutoHide;
        if (!ops.saveRecord(intended)) {
            Log(L"chrome: could not save recovery record; system taskbar left visible");
            return;
        }
        state.hidden = true;
        ops.setHidden(true);
        if (needAutoHide) {
            if (ops.setAutoHide(true)) {
                state.autoHideOurs = true;
                ops.scheduleRefit();
            } else {
                intended.autoHide = state.autoHideOurs;
                if (!ops.saveRecord(intended))
                    Log(L"chrome: could not update recovery record after SetAutoHide failed");
            }
        }
        ops.hideTaskbars();
        ops.watch();
    } else {
        state.hidden = false;
        ops.setHidden(false);
        ops.stopWatch();
        ops.showTaskbars();
        if (state.autoHideOurs && (ops.setAutoHide(false) || !ops.hasShellTaskbar()))
            state.autoHideOurs = false;
        ChromeRecord after = bannerRecord;
        after.taskbarHidden = false;
        after.autoHide = state.autoHideOurs;
        if (!ops.saveRecord(after))
            Log(L"chrome: could not update recovery record after restoring taskbar");
    }
    Log(L"chrome: system taskbar %s", hidden ? L"hidden" : L"shown");
}

void ApplyBannersHidden(bool hidden, BannerChromeState& state, const ChromeRecord& taskbarRecord, const BannerChromeOps& ops)
{
    if (hidden == state.bannersHidden) {
        if (hidden ? (state.quietReady && state.autoPlayBannerReady) : (!state.quietOurs && !state.autoPlayBannerOurs))
            return;
    }
    if (hidden)
        ApplyBannersEnable(state, taskbarRecord, ops);
    else
        ApplyBannersDisable(state, taskbarRecord, ops);
}

std::wstring QuietProfile()
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return {};
    LPWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(qh->get_UserSelectedProfile(&p)) && p)
        out = p;
    CoTaskMemFree(p);
    qh->Release();
    return out;
}

bool SetQuietProfile(const wchar_t* profile)
{
    IQuietHoursSettings* qh = nullptr;
    if (FAILED(CoCreateInstance(kQuietHoursSettings, nullptr, CLSCTX_ALL, kIQuietHoursSettings, reinterpret_cast<void**>(&qh))) || !qh)
        return false;
    bool ok = SUCCEEDED(qh->put_UserSelectedProfile(profile));
    qh->Release();
    return ok && QuietProfile() == profile;
}

SystemChrome::SystemChrome()
{
    g_chrome = this;
    g_taskbarVisibility.stopping = false;
    // Left over from a run that did not end cleanly: put it back first (the
    // settings then hide again what they ask for), and keep ownership of
    // anything that could not be restored yet so the next exit/toggle retries it.
    ChromeRecord leftover{};
    UndoLeftover(&leftover);
    autoHideOurs_ = leftover.autoHide;
    quietOurs_ = leftover.quiet;
    quietBefore_ = std::move(leftover.quietRestore);
    autoPlayBannerOurs_ = leftover.autoPlayBanner;
}

bool ApplyUndoLeftover(const std::wstring& statePath, const UndoLeftoverOps& ops, ChromeRecord* remainingOut)
{
    if (remainingOut)
        *remainingOut = {};
    const std::wstring left = ReadTextFile(statePath);
    if (left.empty())
        return false;
    Log(L"chrome: undoing the last run's changes: %s", left.c_str());
    ops.showTaskbars();
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(left, o)) {
        if (!ops.saveRecord({}))
            Log(L"chrome: could not remove corrupt recovery record");
        return true;
    }
    auto flag = [&](const wchar_t* key) { return o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::Boolean && o.GetNamedBoolean(key); };
    ChromeRecord remaining{};
    if (flag(L"autoHide") && !ops.setAutoHide(false)) {
        remaining.autoHide = true;
        Log(L"chrome: could not restore taskbar auto-hide");
    }
    if (flag(L"quiet"))
        UndoQuietLeftover(o, ops, remaining);
    if (flag(L"autoPlayBanner")) {
        if (ops.setAutoPlayBanner(true))
            Log(L"chrome: AutoPlay banners restored");
        else {
            remaining.autoPlayBanner = true;
            Log(L"chrome: could not restore AutoPlay banners");
        }
    }
    if (!ops.saveRecord(remaining))
        Log(L"chrome: could not update recovery record after UndoLeftover");
    if (remainingOut)
        *remainingOut = remaining;
    return true;
}

bool SystemChrome::UndoLeftover(ChromeRecord* remainingOut)
{
    const std::wstring path = StatePath();
    UndoLeftoverOps ops{
        .showTaskbars = ShowTaskbars,
        .setAutoHide = [](bool on) { return SetAutoHide(on); },
        .quietProfile = QuietProfile,
        .setQuietProfile = SetQuietProfile,
        .setAutoPlayBanner = SetAutoPlayBanner,
        .saveRecord = [&path](const ChromeRecord& record) { return SaveChromeRecord(path, record); },
    };
    return ApplyUndoLeftover(path, ops, remainingOut);
}

void SystemChrome::ShowTaskbarsNow() { ShowTaskbars(); }

// Both the event hook and the periodic/settings hide paths honour this lease.
// No mutex is held across cross-thread ShowWindowAsync calls. The state outlives
// SystemChrome, so a worker finishing during shutdown cannot use a freed owner.
bool SystemChrome::RevealForClick()
{
    if (g_taskbarVisibility.stopping)
        return false;
    ++g_taskbarVisibility.reveals;
    ShowTaskbars();
    const HWND shell = ShellTaskbar();
    for (int i = 0; i < 20 && shell && !IsWindowVisible(shell) && !g_taskbarVisibility.stopping; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_taskbarVisibility.stopping) {
        ConcealAfterClick();
        return false;
    }
    return true;
}

void SystemChrome::ConcealAfterClick()
{
    if (--g_taskbarVisibility.reveals == 0)
        HideTaskbars();
}

SystemChrome::~SystemChrome()
{
    RestoreForExit();
    g_chrome = nullptr;
}

void SystemChrome::RestoreForExit()
{
    if (exiting_)
        return;
    exiting_ = true;
    g_taskbarVisibility.stopping = true;
    SetTaskbarHidden(false);
    SetBannersHidden(false);
    StopRefit(true);
}

void SystemChrome::SetTaskbarHidden(bool hidden)
{
    if (hidden && exiting_)
        return;
    if (!hidden)
        resyncedWork_.clear();
    if (!hidden || !g_taskbarVisibility.hidden.load())
        reshowGuard_.Reset();
    TaskbarChromeState state{g_taskbarVisibility.hidden.load(), autoHideOurs_.load()};
    ChromeRecord bannerRecord{.quiet = quietOurs_, .quietRestore = quietBefore_, .autoPlayBanner = autoPlayBannerOurs_};
    TaskbarChromeOps ops{
        .queryAutoHide = QueryAutoHide,
        .setAutoHide = [this](bool on) { return SetAutoHideSynced(on, on || !exiting_); },
        .hasShellTaskbar = [] { return ShellTaskbar() != nullptr; },
        .hideTaskbars = [this] { HideTaskbars(), ResyncStuckWorkArea(); },
        .showTaskbars = [this] { ShowTaskbars(), (!exiting_ ? WaitTaskbarsVisible(true) : void()); },
        .watch = [this] { Watch(); },
        .stopWatch =
            [this] {
                StopRefit(false);
                if (hook_)
                    UnhookWinEvent(hook_);
                hook_ = nullptr, hookedPid_ = 0;
            },
        .scheduleRefit = [this] { ScheduleRefit(); },
        .setHidden = [](bool on) { g_taskbarVisibility.hidden = on; },
        .saveRecord = [this](const ChromeRecord& record) { return SaveRecord(record); },
    };
    ApplyTaskbarHidden(hidden, state, bannerRecord, ops);
    autoHideOurs_ = state.autoHideOurs;
}

void SystemChrome::SetBannersHidden(bool hidden)
{
    if (hidden && exiting_)
        return;
    BannerChromeState state{bannersHidden_, quietReady_, quietOurs_, quietBefore_, autoPlayBannerOurs_, autoPlayBannerReady_};
    ChromeRecord taskbarRecord{.taskbarHidden = g_taskbarVisibility.hidden.load(), .autoHide = autoHideOurs_.load()};
    BannerChromeOps ops{
        .quietProfile = QuietProfile,
        .setQuietProfile = SetQuietProfile,
        .autoPlayBannerShown = AutoPlayBannerShown,
        .setAutoPlayBanner = SetAutoPlayBanner,
        .saveRecord = [this](const ChromeRecord& record) { return SaveRecord(record); },
    };
    ApplyBannersHidden(hidden, state, taskbarRecord, ops);
    bannersHidden_ = state.bannersHidden;
    quietReady_ = state.quietReady;
    quietOurs_ = state.quietOurs;
    quietBefore_ = std::move(state.quietBefore);
    autoPlayBannerOurs_ = state.autoPlayBannerOurs;
    autoPlayBannerReady_ = state.autoPlayBannerReady;
}

void SystemChrome::Enforce()
{
    if (exiting_)
        return;
    if (g_taskbarVisibility.ShouldHide() && !reshowGuard_.ShouldBackOff(GetTickCount64())) {
        // Hidden but not auto-hide, the taskbar keeps its strip of the work area and maximized
        // windows stop its height short of the bottom. Auto-hide can be lost after it was set:
        // set before Explorer's taskbar was ready at sign-in, Explorer restarted, another program.
        if (const auto autoHide = QueryAutoHide(); autoHide.has_value() && !*autoHide) {
            ChromeRecord intended = CurrentRecord();
            intended.autoHide = true;
            if (!SaveRecord(intended)) {
                Log(L"chrome: could not save recovery record; taskbar auto-hide not re-applied");
            } else if (SetAutoHideSynced(true, true)) {
                autoHideOurs_ = true;
                Log(L"chrome: the taskbar was no longer auto-hide; set again");
                ScheduleRefit();
            } else {
                intended.autoHide = autoHideOurs_.load();
                if (!SaveRecord(intended))
                    Log(L"chrome: could not update recovery record after SetAutoHide failed");
            }
        }
        HideTaskbars();
        ResyncStuckWorkArea();
        Watch(); // Explorer may have been restarted
    }
    if (quietReady_) {
        const std::wstring now = QuietProfile();
        if (!now.empty() && now != kQuietProfile) {
            // The user turned it off (or chose another mode): theirs from now on.
            Log(L"chrome: Do Not Disturb changed by the user (%s); no longer hidden by Atrium", now.c_str());
            quietReady_ = false;
            quietOurs_ = false;
            quietBefore_.clear();
            if (!SaveRecord(CurrentRecord()))
                Log(L"chrome: could not update recovery record");
        }
    }
    if (autoPlayBannerOurs_ && AutoPlayBannerShown()) {
        // The user turned AutoPlay banners back on: theirs from now on.
        Log(L"chrome: AutoPlay banners changed by the user; no longer hidden by Atrium");
        autoPlayBannerOurs_ = false;
        autoPlayBannerReady_ = false;
        if (!SaveRecord(CurrentRecord()))
            Log(L"chrome: could not update recovery record");
    }
}

void SystemChrome::EmergencyRestore()
{
    g_taskbarVisibility.stopping = true;
    if (!g_chrome || !g_taskbarVisibility.hidden.exchange(false))
        return;
    // Stop OnShow/Enforce hiding Explorer again before making it visible.
    // The exception can be on a worker, so these flags are atomic. Do not
    // log or save here; system.json still restores Do Not Disturb next start.
    ShowTaskbars();
    if (g_chrome->autoHideOurs_.exchange(false))
        SetAutoHide(false, 250, SMTO_ABORTIFHUNG | SMTO_BLOCK);
}

void SystemChrome::HideTaskbars()
{
    if (!g_taskbarVisibility.ShouldHide())
        return;
    WaitTaskbarStep(16, 15, [] {
        return !g_taskbarVisibility.ShouldHide() ||
               std::ranges::none_of(Taskbars(), [](HWND w) { return IsWindowVisible(w) && WindowResponsive(w, 150) && TaskbarHoldsExactWorkEdge(w); });
    });
    for (HWND w : Taskbars())
        if (g_taskbarVisibility.ShouldHide() && IsWindowVisible(w))
            ShowWindowAsync(w, SW_HIDE);
    if (g_taskbarVisibility.ShouldHide())
        WaitTaskbarsVisible(false);
    // A concurrent reveal or exit can restore the taskbar mid-flight; restoration wins.
    if (!g_taskbarVisibility.ShouldHide())
        ShowTaskbars();
}

void SystemChrome::ResyncStuckWorkArea()
{
    if (exiting_ || !g_taskbarVisibility.ShouldHide())
        return;
    std::erase_if(resyncedWork_, [](const auto& kv) { return !IsWindow(kv.first); });
    bool needResync = false;
    for (HWND w : Taskbars()) {
        RECT work{};
        if (!WindowResponsive(w, 150) || !TaskbarHoldsExactWorkEdge(w, &work))
            continue;
        if (const auto it = resyncedWork_.find(w); it == resyncedWork_.end() || !EqualRect(&it->second, &work)) {
            resyncedWork_[w] = work;
            needResync = true;
        }
    }
    if (!needResync)
        return;
    if (const auto before = SnapshotTaskbarWork(true); SetAutoHide(false)) {
        if (!SetAutoHide(true))
            SetAutoHide(true);
        WaitWorkAreaSync(before, true), ScheduleRefit(), HideTaskbars();
        Log(L"chrome: resynced taskbar work area");
    }
}

void SystemChrome::Watch()
{
    // Explorer shows its taskbar again by itself now and then: hide it the
    // moment it does.
    DWORD pid = 0;
    GetWindowThreadProcessId(ShellTaskbar(), &pid);
    if (!pid || pid == hookedPid_)
        return;
    if (hook_)
        UnhookWinEvent(hook_);
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, pid, 0, WINEVENT_OUTOFCONTEXT);
    hookedPid_ = hook_ ? pid : 0;
}

void CALLBACK SystemChrome::OnShow(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD)
{
    if (obj != OBJID_WINDOW || !g_taskbarVisibility.ShouldHide() || !w || !IsTaskbar(w))
        return;
    if (g_chrome) {
        const ULONGLONG now = GetTickCount64();
        if (g_chrome->reshowGuard_.ShouldBackOff(now))
            return;
        if (g_chrome->reshowGuard_.Record(now)) {
            Log(L"chrome: taskbar re-shown repeatedly (possible taskbar mod conflict); backing off");
            return;
        }
    }
    HideTaskbars();
    Log(L"chrome: Explorer showed its taskbar; hidden again");
}

void SystemChrome::ScheduleRefit()
{
    {
        std::lock_guard lock(refitMutex_);
        if (refitStop_)
            return;
        refitPending_ = true;
        refitAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (!refitThread_.joinable())
            refitThread_ = std::thread([this] { RefitWorker(); });
    }
    refitCv_.notify_one();
}

void SystemChrome::StopRefit(bool stop)
{
    {
        std::lock_guard lock(refitMutex_);
        refitStop_ = refitStop_ || stop;
        refitPending_ = false;
    }
    refitCv_.notify_all();
    if (stop && refitThread_.joinable())
        refitThread_.join();
}

void SystemChrome::RefitWorker()
{
    for (;;) {
        std::unique_lock lock(refitMutex_);
        refitCv_.wait(lock, [this] { return refitStop_ || refitPending_; });
        if (refitStop_)
            break;
        const auto deadline = refitAt_;
        if (refitCv_.wait_until(lock, deadline, [this, deadline] { return refitStop_ || !refitPending_ || refitAt_ != deadline; }))
            continue;
        refitPending_ = false;
        lock.unlock();
        if (g_taskbarVisibility.hidden && !g_taskbarVisibility.stopping)
            RefitMaximized();
    }
}

ChromeRecord SystemChrome::CurrentRecord() const
{
    return {g_taskbarVisibility.hidden.load(), autoHideOurs_.load(), quietOurs_, quietBefore_, autoPlayBannerOurs_};
}

bool SystemChrome::SaveRecord(const ChromeRecord& record) const { return SaveChromeRecord(StatePath(), record); }

} // namespace atrium
