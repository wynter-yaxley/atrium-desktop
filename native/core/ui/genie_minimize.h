// The genie effect, as macOS minimizes (setting genieMinimize): a window going down pours into its icon on
// the bar, its sides bending towards the icon while the picture slides down into it, the rows nearest the
// bar first; one the bar restores pours back out of its icon the same way and dissolves into the window.
// Windows' own minimize animation is off for the window in front while this is on
// (DWMWA_TRANSITIONS_FORCEDISABLED), once a picture of it is at hand, and back on as soon as it is down,
// so restoring it some other way is Windows' own. A minimized window has nothing to show: the picture is
// kept fresh while it is in front (often while the pointer is near its caption buttons, where a minimize
// comes from), as Stage Manager keeps its pictures; the bar's own click takes a fresh one first. The
// effect's layer is on screen, empty and click-through, before a minimize can come, so the first frame
// is there as the window goes. Windows Stage Manager flies itself are left to it.
#pragma once

#include "render/genie_mesh.h"
#include "render/genie_renderer.h"
#include "ui/surface.h"
#include "ui/window_snapshot.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atrium {

// Ref-counted DWM transition-disable lease so nested holds/flights preserve the prior DWM state.
class GenieTransitionLeases {
public:
    template<class QueryFn, class SetFn> void Acquire(HWND window, QueryFn&& queryDisabled, SetFn&& setDisabled)
    {
        if (!window)
            return;
        Lease& lease = leases_[window];
        if (lease.count++ == 0) {
            BOOL prior = FALSE;
            if (!std::forward<QueryFn>(queryDisabled)(window, prior))
                prior = FALSE;
            lease.priorDisabled = prior;
            std::forward<SetFn>(setDisabled)(window, true);
        }
    }

    template<class IsValidFn, class SetFn> void Release(HWND window, bool restoreAttribute, IsValidFn&& isValid, SetFn&& setDisabled)
    {
        if (!window)
            return;
        const auto it = leases_.find(window);
        if (it == leases_.end())
            return;
        if (--it->second.count > 0)
            return;
        const BOOL priorDisabled = it->second.priorDisabled;
        leases_.erase(it);
        if (restoreAttribute && std::forward<IsValidFn>(isValid)(window))
            std::forward<SetFn>(setDisabled)(window, priorDisabled != FALSE);
    }

    template<class SkipFn, class IsValidFn, class SetFn> void Clear(SkipFn&& skip, IsValidFn&& isValid, SetFn&& setDisabled)
    {
        for (const auto& [window, lease] : leases_)
            if (!skip(window) && isValid(window))
                setDisabled(window, lease.priorDisabled != FALSE);
        leases_.clear();
    }

    bool Contains(HWND window) const { return leases_.contains(window); }
    int Count(HWND window) const
    {
        const auto it = leases_.find(window);
        return it == leases_.end() ? 0 : it->second.count;
    }
    bool Empty() const { return leases_.empty(); }

private:
    struct Lease {
        int count = 0;
        BOOL priorDisabled = FALSE;
    };
    std::unordered_map<HWND, Lease> leases_;
};

// Drains pending restore callbacks and flight transition leases once before clearing flights.
template<class FlightRange, class IsValidFn, class ForgetFn, class ReleaseLeaseFn>
std::vector<std::function<void()>> DrainGenieShutdownRestores(FlightRange& flights, IsValidFn&& isValid, ForgetFn&& forgetImage, ReleaseLeaseFn&& releaseLease)
{
    std::vector<std::function<void()>> restores;
    for (auto& f : flights) {
        forgetImage(f.key);
        if (f.restore) {
            std::function<void()> restore = std::exchange(f.restore, nullptr);
            if (f.window && isValid(f.window))
                restores.push_back(std::move(restore));
        }
        if (f.restoreTransitions) {
            f.restoreTransitions = false;
            releaseLease(f.window);
        }
    }
    flights.clear();
    return restores;
}

class GenieMinimize {
public:
    // target: the icon of the window's app on a bar (screen px), else none; skip: a window another
    // animation owns.
    GenieMinimize(HINSTANCE instance, std::function<std::optional<RECT>(HWND)> target, std::function<bool(HWND)> skip);
    ~GenieMinimize();
    GenieMinimize(const GenieMinimize&) = delete;
    GenieMinimize& operator=(const GenieMinimize&) = delete;

    void SetHardwareHost(GenieHost* host);
    void SetEnabled(bool on);
    void Minimizing(HWND window); // EVENT_SYSTEM_MINIMIZESTART
    // The bar's click (TaskModel::SetWindowSteps): `minimize` runs once a fresh picture of the window is at
    // hand; `restore` runs when the window has poured back out of its icon. False: not for the genie.
    bool Minimize(HWND window, std::function<void()> minimize);
    bool Restore(HWND window, std::function<void()> restore);
    void SignalSnapshotStop() { snapshots_.SignalStop(); }
    bool WaitSnapshotStop(DWORD timeoutMs) const { return snapshots_.WaitStop(timeoutMs); }

private:
    struct PictureMeta {
        WindowSnapshotKey key;
        ULONGLONG taken = 0; // asked for or taken
        RECT frame{};        // where the window was, screen px
        uint64_t pictureId = 0;
    };
    struct CachedPictureHit {
        std::shared_ptr<const WindowPicture> picture;
        RECT frame{};
        uint64_t pictureId = 0;
    };
    struct Flight {
        std::shared_ptr<const WindowPicture> picture;
        std::wstring key;
        RECT from{}, to{}; // screen px
        double startMs = 0; // 0: from its first frame on screen
        HWND window = nullptr;
        std::function<void()> restore; // set: out of the icon, then this
        bool restoreDue = false;
        bool restoreTransitions = false;
    };
    struct HardwareFlight {
        uint64_t id = 0;
        uint64_t pictureId = 0;
        std::shared_ptr<const WindowPicture> picture;
        RECT from{}, to{};
        HWND window = nullptr;
        bool isRestore = false;
        bool restoreTransitions = false;
        bool holdPrimed = false;
        double phaseStartMs = 0.0;
    };

    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void EnsureOverlayWindow();
    void DestroyOverlayWindow();
    void Watch();                                              // kWatchTimer: the window in front, its picture, its animations
    void Release(HWND window, bool restoreTransitions = true); // its own animations back (unless another owner took it)
    void Hold(HWND window);                                    // its own animations off
    void AcquireTransitions(HWND window);
    void ReleaseTransitions(HWND window, bool restoreAttribute);
    std::optional<CachedPictureHit> LookupCachedPicture(HWND window);
    bool HasActiveHardwareMinimize(HWND window) const;
    RECT FlightBounds(const RECT& fallback) const;
    RECT HardwareFlightBounds() const;
    void Arm(const RECT& frame); // the layer on the flights' monitor(s), empty, for a while
    void Shutdown();
    void Launch(Flight f);
    void LaunchHardwareFlight(HWND window, const CachedPictureHit& hit, const RECT& to, std::function<void()> cb, bool isRestore, bool waitForCover);
    PictureMeta& EnsureProcessMeta(HWND window, const WindowSnapshotKey& key);
    std::vector<std::function<void()>> DrainHardwareFlights();
    void PublishHardwareFlights();
    void TickHardwareFlights(double nowMs);
    void OnHardwareFramePresented(uint64_t flightId, uint64_t epoch, bool pourFinished);
    void OnHardwareDeviceLost(uint64_t epoch);
    void CancelHardwareFlights();
    void RunPending();            // the bar's minimize, its picture taken (or not in time)
    void RunRestores();           // flights out of the icon that have landed
    void Paint(Canvas& c);
    bool PaintOne(Canvas& c, Flight& f); // true: done
    void PaintFlight(Canvas& c, const Flight& f, float t, float alpha);

    HINSTANCE instance_ = nullptr;
    Surface surface_; // D2D fallback layer when no hardware GenieHost is attached
    HWND hwOverlay_ = nullptr;
    GenieHost* hardwareHost_ = nullptr;
    uint64_t hardwareEpoch_ = 1;
    WindowSnapshots snapshots_;
    std::function<std::optional<RECT>(HWND)> target_;
    std::function<bool(HWND)> skip_;
    bool enabled_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    HWND front_ = nullptr, held_ = nullptr; // the window in front; the one whose animations are off
    ULONGLONG frontSince_ = 0;
    WindowPictureCache pictureCache_;
    std::unordered_map<HWND, PictureMeta> pictureMeta_;
    uint64_t nextPictureGen_ = 0;
    uint64_t nextPictureId_ = 0;
    std::vector<Flight> flights_;
    std::vector<HardwareFlight> hwFlights_;
    GenieFlightStateMachine stateMachine_;
    unsigned flightGen_ = 0;
    HWND pending_ = nullptr; // the bar's minimize, waiting for its picture
    std::function<void()> pendingMinimize_;
    GenieTransitionLeases transitionLeases_;
    double eventMs_ = 0; // when the last minimize came, for the log
};

} // namespace atrium
