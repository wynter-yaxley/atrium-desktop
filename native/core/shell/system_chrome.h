// The parts of Windows' own shell that Atrium can stand in for, hidden on the
// user's request (both settings are off by default): the system taskbar
// (Shell_TrayWnd and the secondary ones are hidden while Atrium runs, and set
// to auto-hide so they leave no gap) and the system's notification banners
// (Do Not Disturb through the shell's QuietHoursSettings service: the
// notifications still reach the notification centre, where the listener reads
// them for Atrium's cards). Everything is put back when the setting is turned
// off or Atrium exits; what was changed is kept in system.json, so a run that
// did not end cleanly is undone by the next start, or at once by the guard
// (`Atrium.exe --guard`, main.cpp) that watches a run hiding them.
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace atrium {

// The shell's Do Not Disturb profiles Atrium uses.
inline constexpr wchar_t kQuietProfile[] = L"Microsoft.QuietHoursProfile.AlarmsOnly";
inline constexpr wchar_t kQuietProfileOff[] = L"Microsoft.QuietHoursProfile.Unrestricted";

// What SetBannersHidden does to the shell's Do Not Disturb profile. Anything
// short of alarms-only lets priority notifications through: a "priority only"
// profile still shows e.g. the AutoPlay toast for a newly inserted drive, which
// reads as the setting not hiding system banners at all.
enum class QuietAction {
    None,
    Hide,
    Restore,
};

struct QuietPlan {
    QuietAction action = QuietAction::None;
    std::wstring profile; // hide: alarms-only; restore: the profile to put back
    bool owns = false;    // the change is Atrium's, to undo when the setting goes off
};

inline QuietPlan PlanQuiet(const std::wstring& current, bool hide, bool ours, const std::wstring& before)
{
    QuietPlan plan;
    if (hide) {
        // Nothing to do when the service is gone or the strict profile is
        // already selected (by Atrium or by the user).
        if (current.empty() || current == kQuietProfile)
            return plan;
        plan.action = QuietAction::Hide;
        plan.profile = kQuietProfile;
        plan.owns = true;
        return plan;
    }
    if (ours) {
        plan.action = QuietAction::Restore;
        plan.profile = before.empty() ? kQuietProfileOff : before;
    }
    return plan;
}

struct ChromeRecord {
    bool taskbarHidden = false;
    bool autoHide = false;
    bool quiet = false;
    std::wstring quietRestore;
    bool autoPlayBanner = false;

    bool Empty() const { return !taskbarHidden && !autoHide && !quiet && !autoPlayBanner; }
    bool operator==(const ChromeRecord&) const = default;
};

struct TaskbarChromeState {
    bool hidden = false;
    bool autoHideOurs = false;
};

struct TaskbarChromeOps {
    std::function<std::optional<bool>()> queryAutoHide;
    std::function<bool(bool)> setAutoHide;
    std::function<bool()> hasShellTaskbar;
    std::function<void()> hideTaskbars;
    std::function<void()> showTaskbars;
    std::function<void()> watch;
    std::function<void()> stopWatch;
    std::function<void()> scheduleRefit;
    std::function<void(bool)> setHidden;
    std::function<bool(const ChromeRecord&)> saveRecord;
};

struct BannerChromeState {
    bool bannersHidden = false;
    bool quietReady = false;
    bool quietOurs = false;
    std::wstring quietBefore;
    bool autoPlayBannerOurs = false;
    bool autoPlayBannerReady = false;
};

struct BannerChromeOps {
    std::function<std::wstring()> quietProfile;
    std::function<bool(const wchar_t*)> setQuietProfile;
    std::function<bool()> autoPlayBannerShown;
    std::function<bool(bool)> setAutoPlayBanner;
    std::function<bool(const ChromeRecord&)> saveRecord;
};

struct UndoLeftoverOps {
    std::function<void()> showTaskbars;
    std::function<bool(bool)> setAutoHide;
    std::function<std::wstring()> quietProfile;
    std::function<bool(const wchar_t*)> setQuietProfile;
    std::function<bool(bool)> setAutoPlayBanner;
    std::function<bool(const ChromeRecord&)> saveRecord;
};

[[nodiscard]] bool SaveChromeRecord(const std::wstring& path, const ChromeRecord& record);
void ApplyTaskbarHidden(bool hidden, TaskbarChromeState& state, const ChromeRecord& bannerRecord, const TaskbarChromeOps& ops);
void ApplyBannersHidden(bool hidden, BannerChromeState& state, const ChromeRecord& taskbarRecord, const BannerChromeOps& ops);
bool ApplyUndoLeftover(const std::wstring& statePath, const UndoLeftoverOps& ops, ChromeRecord* remainingOut = nullptr);

class SystemChrome;

// Rate-limits repeated taskbar re-hides (e.g. StartAllBack re-showing Shell_TrayWnd).
struct TaskbarReshowGuard {
    static constexpr ULONGLONG kWindowMs = 2000;
    static constexpr ULONGLONG kBackoffMs = 5000;
    static constexpr size_t kThreshold = 8;

    bool ShouldBackOff(ULONGLONG now) const { return backedOff_ && now < backoffUntil_; }

    bool Record(ULONGLONG now)
    {
        if (ShouldBackOff(now))
            return false;
        if (backedOff_)
            Reset();
        while (count_ > 0 && now - stamps_[head_] > kWindowMs) {
            head_ = (head_ + 1) % kThreshold;
            --count_;
        }
        stamps_[(head_ + count_) % kThreshold] = now;
        if (count_ < kThreshold)
            ++count_;
        else
            head_ = (head_ + 1) % kThreshold;
        if (count_ >= kThreshold) {
            backedOff_ = true;
            backoffUntil_ = now + kBackoffMs;
            return true;
        }
        return false;
    }

    void Reset()
    {
        stamps_.fill(0);
        head_ = 0;
        count_ = 0;
        backedOff_ = false;
        backoffUntil_ = 0;
    }

private:
    std::array<ULONGLONG, kThreshold> stamps_{};
    size_t head_ = 0;
    size_t count_ = 0;
    bool backedOff_ = false;
    ULONGLONG backoffUntil_ = 0;
};

class SystemChrome {
public:
    SystemChrome();
    ~SystemChrome(); // puts everything back
    SystemChrome(const SystemChrome&) = delete;
    SystemChrome& operator=(const SystemChrome&) = delete;

    void SetTaskbarHidden(bool hidden);
    void SetBannersHidden(bool hidden);
    // Hides the taskbars again if Explorer has shown them (a restart of
    // Explorer, a display change); follows the user turning Do Not Disturb off.
    void Enforce();
    // Irreversible for this instance: disable every hide entry point, then
    // restore Windows before joining workers or waiting for the renderer.
    void RestoreForExit();
    // Do Not Disturb is in the alarms-only state the setting asks for (whether
    // Atrium switched it or the user already had it): it must not hide Atrium's
    // own cards then.
    bool QuietIsOurs() const { return quietReady_; }
    // From a crash handler: stops hiding, restores our auto-hide change and
    // shows the taskbars (no allocation, no COM).
    static void EmergencyRestore();
    // What a run that did not end cleanly left changed (system.json), put back; false if nothing
    // was. COM must be initialised (Do Not Disturb).
    static bool UndoLeftover(ChromeRecord* remainingOut = nullptr);
    // The taskbars shown, nothing else changed (Atrium is not responding).
    static void ShowTaskbarsNow();
    // Around one real mouse click on a tray icon the taskbars must be visible;
    // the shell's own show-hook must not hide them again in between. Reveal
    // shows them (no-op flags otherwise), Conceal hides them back only when
    // Atrium had hidden them.
    static bool RevealForClick(); // false once exit restoration has started
    static void ConcealAfterClick();

private:
    static void HideTaskbars();
    void ResyncStuckWorkArea();
    void ScheduleRefit();
    void StopRefit(bool stop);
    void RefitWorker();
    void Watch();
    ChromeRecord CurrentRecord() const;
    [[nodiscard]] bool SaveRecord(const ChromeRecord& record) const;
    static void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    std::atomic_bool autoHideOurs_ = false; // Atrium switched the taskbar to auto-hide
    bool bannersHidden_ = false; // the setting asks for no system banners
    bool quietReady_ = false;    // Do Not Disturb is alarms-only (ours or already)
    bool quietOurs_ = false;     // Atrium changed the profile; restore quietBefore_
    std::wstring quietBefore_;   // the profile Atrium changed it from
    bool autoPlayBannerOurs_ = false; // Atrium turned the AutoPlay banners off; restore them
    bool autoPlayBannerReady_ = false; // AutoPlay banners are off (ours or already)
    bool exiting_ = false;
    TaskbarReshowGuard reshowGuard_;
    HWINEVENTHOOK hook_ = nullptr;
    DWORD hookedPid_ = 0;
    std::map<HWND, RECT> resyncedWork_;
    std::mutex refitMutex_;
    std::condition_variable refitCv_;
    bool refitStop_ = false;
    bool refitPending_ = false;
    std::chrono::steady_clock::time_point refitAt_;
    std::thread refitThread_;
};

// Do Not Disturb: the user's selected profile, e.g.
// "Microsoft.QuietHoursProfile.Unrestricted"; empty if the service is unavailable.
std::wstring QuietProfile();
bool SetQuietProfile(const wchar_t* profile);

} // namespace atrium
