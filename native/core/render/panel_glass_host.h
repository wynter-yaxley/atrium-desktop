// Renders the glass layers of popups and desktop widgets on the bar's render thread.
#pragma once

#include "../settings.h"
#include "capture.h"
#include "desktop_backdrop.h"
#include "glass_panel.h"
#include "glass_renderer.h"
#include "gpu.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace atrium {

// Ink over the glass (mean sRGB luminance of what it shows, 0..1): black and
// white text read equally well near 0.46, so dark ink from 0.50 up and light
// ink again below 0.42. Light grey windows (0.5..0.65) used to keep white text.
inline constexpr float kDarkInkAbove = 0.50f;
inline constexpr float kLightInkBelow = 0.42f;

inline constexpr double kBackupIdleIntervalMs = 250.0;
inline constexpr double kBackupActiveIntervalMs = 100.0;
inline constexpr double kBackupMovedWindowMs = 1000.0;

inline double BackupKeepIntervalMs(bool hasScreenPanel, double sinceMovedMs)
{
    return sinceMovedMs <= kBackupMovedWindowMs ? 0.0 : (hasScreenPanel ? kBackupActiveIntervalMs : kBackupIdleIntervalMs);
}

GlassParams ParamsFor(const Settings& s, float scale, float insetDip = 14.f);
float GlassLuma(const Settings& s, float backdrop);
void PanelParams(const Settings& s, float scale, const GlassPanel& p, std::vector<GlassParams>& out);

class PanelGlassHost {
public:
    enum class StepResult { Ok, Exit, Retry };

    struct RenderResult {
        StepResult status = StepResult::Ok;
        bool moving = false;
        bool gpuSubmitted = false;
    };

    using CreateRendererFn = std::function<std::unique_ptr<GlassRenderer>(const Gpu& gpu, HWND hwnd, int width, int height, bool desktop)>;
    using ShowOwnWindowsFn = std::function<bool(bool visible)>;

    PanelGlassHost(Gpu gpu, HMONITOR captureMonitor, HANDLE wakeEvent, HANDLE frameEvent, bool remote, bool debugCapturable, bool initialMayShowInCapture,
                   CreateRendererFn createRenderer, ShowOwnWindowsFn showOwnWindows);
    PanelGlassHost(const PanelGlassHost&) = delete;
    PanelGlassHost& operator=(const PanelGlassHost&) = delete;
    ~PanelGlassHost() = default;

    bool HasScreenPanelOnCaptureMonitor(const std::vector<GlassPanel>& panels) const;
    bool HasAnyVisibleScreenPanel(const std::vector<GlassPanel>& panels) const;
    bool KeepFrame(ID3D11Texture2D* frame, bool hasScreenPanel);
    ID3D11Texture2D* BackupFrame() const { return lastFrame_.get(); }
    void ReleaseBackupFrame() { lastFrame_ = nullptr; }
    unsigned TakeBackupCopyCount();

    bool InkDue() const { return inkDue_; }
    bool AnyOwed() const;
    bool AnyCaptureRequiresPolling() const;
    bool AnyReadbackPending() const;

    RenderResult Render(const std::vector<GlassPanel>& panels, const Settings& settings, RECT monitorRect, float barScale, ID3D11Texture2D* source,
                        uint64_t tick, bool barKeepsOwnWindowsOut);

private:
    struct PanelState {
        explicit PanelState(HWND owner) : hwnd(owner) {}
        ~PanelState() { ForgetGlass(hwnd); }
        PanelState(const PanelState&) = delete;
        PanelState& operator=(const PanelState&) = delete;
        HWND hwnd; // frame/ink records leave with the layer, including device resets
        std::unique_ptr<GlassRenderer> renderer;
        unsigned pop = 0;
        bool ready = false;   // presented once (the popup was told)
        unsigned shown = 0;   // GlassPanel::shown that `ready` was for
        unsigned presentedGen = 0;
        int ink = -1;         // ink last posted: 1 dark, 0 light, -1 none yet
        RECT window{};        // where it was last drawn: a moved panel is drawn again at once
        unsigned desktopVersion = 0; // a widget: the desktop picture its backdrop came from
        double desktopMs = -1e9;     // and when a new one came
        bool inkOwed = false;        // its ink waits for the desktop to settle
        // Presents of an animation, logged when it ends (smoothness checks).
        double burstStart = 0, burstLast = 0, burstGap = 0;
        int burstFrames = 0;
        // Why it was drawn again, during the burst: moved or resized, a new desktop
        // picture, what is under it changed (readback), its parameters changed.
        int burstMoved = 0, burstDesktop = 0, burstReadback = 0, burstParams = 0;
        bool owed = false;      // a frame to draw once its last one is on screen (Ready), or at the next tick
        bool pendingFresh = false;
        double uncoveredSinceMs = 0.0;
        double readyStalledSinceMs = 0.0;
        uint64_t drawnTick = 0; // the compositor frame it was last drawn in
        bool dark = false;
        InkGrid grid;         // ink region by region (published for the popup)
        std::vector<GlassParams> params;
    };

    struct PanelCapture {
        std::unique_ptr<BackdropSource> source;
        winrt::com_ptr<ID3D11Texture2D> texture;
        RECT bounds{};
        RECT copyBounds{};
        bool fresh = false;
        bool partialCopy = false;
    };

    StepResult SyncPanelCaptures(const std::vector<GlassPanel>& panels, bool barKeepsOwnWindowsOut);
    StepResult AcquirePanelCaptures(const std::vector<GlassPanel>& panels);
    void FlushIdleBursts(double now);
    StepResult RenderSinglePanel(const GlassPanel& p, const Settings& settings, RECT monitorRect, float barScale, ID3D11Texture2D* source, uint64_t tick,
                                 double now, bool& moving, bool& gpuSubmitted);
    void UpdatePanelBackdrop(PanelState& ps, const GlassPanel& p, ID3D11Texture2D* source, RECT monitorRect, double now, bool moved, bool changed,
                             bool& newDesktop, bool& moving, bool& gpuSubmitted);
    void UpdatePanelInk(PanelState& ps, HWND hwnd, const Settings& settings, float panelScale, int w, int h, double now, bool& changed, bool& under);
    void RecordPanelPresent(PanelState& ps, const GlassPanel& p, int w, int h, double now, double t0, double created, double t1, bool moved, bool newDesktop,
                            bool under, bool same);

    Gpu gpu_;
    HMONITOR captureMonitor_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    bool remote_ = false;
    bool debugCapturable_ = false;
    bool mayShowInCapture_ = false;
    CreateRendererFn createRenderer_;
    ShowOwnWindowsFn showOwnWindows_;
    DesktopBackdrop desktop_;
    std::map<HWND, PanelState> panels_;
    std::map<HMONITOR, PanelCapture> panelCaptures_;
    winrt::com_ptr<ID3D11Texture2D> lastFrame_;
    double panelMovedMs_ = -1e9;
    double keptMs_ = -1e9;
    double slowLogged_ = -1e9;
    bool inkDue_ = false;
    unsigned backupCopyCount_ = 0;
    std::vector<GlassParams> gp_;
};

} // namespace atrium
