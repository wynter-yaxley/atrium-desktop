// The floating glass bar (coexists with the system taskbar: it sits above the
// work area and never hides or replaces Explorer's taskbar).
//
// Two top-level windows (D08): a click-through layered visual window that
// DirectComposition renders into, and an invisible input window directly below
// it whose rounded region receives the clicks. Both are excluded from capture,
// so the monitor capture shows what lies underneath (D01).
//
// Stacking (D25): both windows are topmost and are raised again when another
// window covers them (foreground changes, a move ends, a 1 s check); they stay
// below a revealed system taskbar. While the top app on this monitor is maximized
// or full screen, the bar slides off screen and comes up again while the
// pointer is at the screen's bottom edge below it (auto-hide; setting autoHide
// can keep it up always, or hide it whatever is in front). Not over an app in
// front that fills the whole screen (a game): as Windows' taskbar, it stays down.
// The render thread also draws the glass layer of Atrium's popups.
#pragma once

#include "near_hover.h"
#include "render/bar_painter.h"
#include "render/capture_policy.h"
#include "render/genie_renderer.h"
#include "render/glass_panel.h"
#include "settings.h"
#include "shell/task_model.h"
#include "shell/displays.h"

#include <windows.h>
#include <dwrite_3.h>
#include <ole2.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atrium {

struct Gpu;
class GlassRenderer;
class Surface;

// Every Atrium window's capture exclusion again (remote-control mode changed). UI thread.
void RefreshCaptureExclusion();

class Bar : public GlassHost, public GenieHost {
public:
    // debugCapturable: development only. Leaves the bar visible to screen capture
    // and freezes the backdrop after the first frame so the glass never samples
    // itself; used to take screenshots of the real surface.
    Bar(HINSTANCE instance, bool debugCapturable, std::wstring display = {});
    ~Bar() override;
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    // UI thread only.
    void Apply(const Settings& s);
    void SetItems(std::vector<TaskItem> items);
    void Relayout();
    // Topology, resolution or work-area changed. A work area alone (Windows' taskbar shown or
    // hidden) keeps the renderer: rebuilding it leaves the glass without a backdrop for a while.
    void RefreshDisplays(bool workAreaOnly = false);
    const std::wstring& DisplayOverride() const { return displayOverride_; }
    HMONITOR TaskMonitor() const { return filterTasks_ ? hmonitor_ : nullptr; }
    HMONITOR Monitor() const { return hmonitor_; }
    // A window is about to fill the screen (a maximized one flying back out of Stage Manager's strip): the
    // bar goes now, with it, rather than show again for a moment once it lands. For `ms`, then as usual.
    void ExpectCover(UINT ms);
    const std::vector<TaskItem>& Items() const { return items_; }
    void SetVisible(bool visible);
    bool Visible() const { return visible_; }
    // On screen now: module on and not slid away for auto-hide (the bubble
    // announces in its place while it is).
    bool ShownOnScreen() const { return visible_ && !slidOut_; }
    // Unread notifications, shown as a badge on the bell.
    void SetUnread(int count);
    void SetDoNotDisturb(bool on);
    // Network, battery and volume for the status icon.
    void SetStatus(const SystemStatus& status);
    // The foreground window's input (中/a/A) in the input method entry; empty: not known.
    void SetIme(const std::wstring& glyph);
    // The app at `index` (0-based, in bar order) as a click on it would (Win + digit).
    void ActivateIndex(int index);
    // An auto-hidden bar comes up (the Windows key's start menu, startWithBar); it goes again as
    // when the pointer brought it, once nothing keeps it shown.
    void Reveal();
    // Over a game in front (only the Windows key brings the bar there): up without taking the focus from
    // the game, a moment for the pointer to reach it, then gone again when the pointer is not on it.
    // False: it was up already.
    bool RevealOverGame();
    // Screen rectangle of an entry ("#start", "#notify", an app key); empty if not shown.
    RECT EntryRect(const std::wstring& key) const;
    // Screen rectangle of the glass shape.
    RECT BarRect() const;
    RECT WindowRect() const { return window_; }
    uint64_t DropBarId() const { return dropId_; }
    RECT DropBarRect() const;
    bool ApplyRemoteFileDragOver(POINT screen, std::wstring& keyOut);
    void ApplyRemoteFileDragLeave();
    void ApplyRemoteDropFiles(const std::wstring& key, const std::vector<std::wstring>& files);
    BarEdge Edge() const { return edge_; }
    // The window that had the foreground before a click on the logo activated the bar.
    HWND PreviousForeground() const { return previousForeground_; }
    bool Seen() const { return visible_ && !tucked_; } // on screen, not tucked away
    // Glass layers of popup windows (GlassHost).
    void SetPanel(const GlassPanel& panel) override;
    // Hardware Genie overlay (UI thread).
    HWND OverlayAnchorHwnd() const override { return visual_; }
    void SetGenieOverlay(HWND hwnd, uint64_t epoch) override;
    void SubmitGenieFlights(GenieSceneSnapshot scene) override;
    void SubmitBarGenieOverlay(GenieSceneSnapshot scene);
    void ClearGenieFlights() override;
    std::function<void(const GenieSceneSnapshot&, std::vector<RECT>&)> onGenieScene;
    // Signals the render thread to stop without waiting.
    void SignalStopRendering();
    // Waits (at most `timeoutMs`) for a signaled render thread while pumping UI messages.
    bool WaitStopRendering(DWORD timeoutMs);
    // Signals and waits for the render thread, pumping this (UI) thread's messages meanwhile.
    bool StopRendering(DWORD timeoutMs);
    // Unhooks stacking WinEvent hooks, kills input timers, and tears down any AppBar registration
    // before retiring/draining a bar.
    void DetachInput();
    static constexpr double kInsetDip = 14;

    std::function<IconImage(const std::wstring& key, int px)> iconProvider;
    std::function<void(const std::wstring& key)> onActivate;  // left click on an app
    std::function<void(const std::wstring& key)> onLaunch;    // middle click on an app
    std::function<void(const std::wstring& key, POINT)> onItemMenu; // right click on an app
    std::function<void(POINT)> onContextMenu;                 // right click elsewhere on the bar
    std::function<void()> onClockClick;
    // Bar entries; `anchor` is the entry's rectangle on screen (popups open above it).
    std::function<void(RECT anchor)> onStartClick;  // the Atrium logo
    std::function<void(POINT)> onStartMenu;         // right click on the Atrium logo
    std::function<void(RECT anchor)> onNotifyClick; // the notification bell
    std::function<void(RECT anchor)> onTrayClick;   // the tray entry (Atrium's tray panel)
    std::function<void()> onDesktopClick;           // the Show Desktop slot at the bar's right end
    std::function<void(RECT anchor)> onStatusClick; // the status icon (Wi-Fi, battery: the control centre)
    std::function<void()> onImeClick;               // the input method entry: Chinese/English, or the next language
    std::function<void(int notches)> onStatusWheel; // the wheel over the status icon (volume)
    // Launchpad (#launchpad), the now-playing entry (#music) and the folder stack (#stacks).
    std::function<void(const std::wstring& key, RECT anchor)> onExtraClick;
    // The now-playing entry, right of the status entry, while `shown` (a player has a session); its
    // bars move with `feed`; `tip` its tooltip (the track). Every call wakes the render thread.
    void SetMusic(std::shared_ptr<const MusicFeed> feed, bool shown, std::wstring tip);
    void WakeRender();
    // The overflow entry: apps that did not fit, in bar order.
    std::function<void(const std::vector<std::wstring>& keys, RECT anchor)> onOverflow;
    // Drag and drop of an app to visible position `toIndex` (0-based, in the
    // list without the dragged item). The bar already shows the new order.
    std::function<void(const std::wstring& key, int toIndex)> onReorder;
    // Files dragged from Explorer: hovering an app brings it to the front,
    // dropping opens the files with it.
    std::function<void(const std::wstring& key)> onFileDragHover;
    std::function<bool(const std::wstring& key)> canOpenFiles;
    std::function<void(const std::wstring& key, const std::vector<std::wstring>& files)> onDropFiles;
    std::function<void()> onCloseRequest; // WM_CLOSE on a bar window (taskkill, logoff)
    // True while a popup opened from the bar is open: an auto-hidden bar stays up.
    std::function<bool()> keepShown;
    // The pointer rests on an app with windows: preview them above `anchor` (screen px: the icon's
    // centre, the top of the magnified icons); onPreviewLeave when it moves off, onPreviewClose on a press.
    std::function<void(const std::wstring& key, const std::vector<HWND>& windows, POINT anchor)> onPreview;
    std::function<void()> onPreviewLeave;
    std::function<void()> onPreviewClose;
    std::function<bool()> previewOpen; // a preview is up: moving to another app swaps it at once
    std::function<void()> onMonitorChanged = [] {}; // close or re-anchor the bar's popups
    std::function<void()> onInteract = [] {}; // this display owns the next popup
    std::function<void()> onItemsChanged = [] {}; // keep an open preview filtered after a window crosses displays
    std::function<void()> onTuckChanged = [] {};  // auto-hide tucked the bar away or brought it up
    std::function<void()> onDropLayoutChanged = [] {}; // drop target rect, visibility, or tuck changed

private:
    static constexpr UINT WM_BAR_LAYOUT = WM_APP + 0x71; // defer DPI relayout until SetWindowPos returns
    static constexpr UINT_PTR kTooltipTimer = 1, kFileHoverTimer = 2, kGuardTimer = 3, kSlideTimer = 4, kPointerTimer = 5, kDisplayTimer = 6, kCoverTimer = 7,
                              kNearTimer = 8;
    static constexpr UINT kGuardMs = 1000, kPointerMs = 50, kNearMs = 16, kCoverMs = 50;
    static constexpr UINT kTooltipDelayMs = 450, kPreviewSwapMs = 60, kFileHoverDelayMs = 600;
    static constexpr DWORD kLeaveMs = 500;
    static constexpr float kTooltipFont = 12.5f, kTooltipPadX = 12, kTooltipPadY = 6, kTooltipMinH = 28;
    static constexpr float kTooltipTailH = 5, kTooltipTailHalfW = 12, kTooltipTailInset = 8;

    struct Snapshot {
        Settings settings;
        BarScene scene;
        RECT window{};     // visual window, screen px
        RECT monitor{};    // monitor rect, screen px
        HMONITOR hmonitor = nullptr;
        float scale = 1;
        bool visible = true;
        unsigned generation = 0;
        unsigned displayGeneration = 0;
        std::vector<GlassPanel> panels;
    };
    struct Hit {
        std::wstring key; // app key, L"#clock", or empty for the bar itself
        RECT rect{};
    };
    struct CachedIcon {
        uint64_t version = 0;
        int px = 0;
        uint64_t id = 0;
        std::shared_ptr<const IconImage> image;
    };

    enum class SessionEnd { Exit, Switch, Retry };

    static LRESULT CALLBACK VisualProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM);
    static UINT AppBarNotice();
    static int Px(double dip, float scale);
    LRESULT OnInput(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT HandleMouseActivate(HWND h, LPARAM l);
    void HandleMouseMove(HWND h, WPARAM w, POINT pt);
    bool HandleInputTimer(HWND h, WPARAM w);
    void HandleButtonUp(UINT m, WPARAM w, POINT pt);
    void HandleCaptureChanged(HWND h, LPARAM l);
    void HandleMouseWheel(HWND h, WPARAM w, LPARAM l);
    void HandleRightButtonUp(HWND h, POINT pt);
    void ReadDisplays();
    struct RelayoutTarget;
    struct LayoutMetrics;
    HMONITOR ResolveRelayoutMonitor(MONITORINFO& mi);
    bool ResolveLayoutTarget(RelayoutTarget& out);
    void FitAppStrip(LayoutMetrics& m, float scale, int screenW, int maxW, int rightPlates, int statusExtra);
    LayoutMetrics ComputeLayoutMetrics(const RelayoutTarget& target);
    BarScene InitLayoutScene(const RelayoutTarget& target, const LayoutMetrics& m) const;
    void ResolveTaskIcon(const TaskItem& t, int iconPx, std::map<std::wstring, CachedIcon>& keep, SceneItem& it);
    void BuildLayoutScene(const RelayoutTarget& target, const LayoutMetrics& m);
    void ApplyLayout(const RelayoutTarget& target, const LayoutMetrics& m);
    void Bounce(const std::wstring& key);
    POINT LayoutPoint(POINT client) const;
    RECT EdgeZone() const;
    std::wstring HitTest(POINT pt) const;
    // Copies scene state to the render thread; `bar` false: only the popups'
    // glass changed (the bar is not drawn again for it).
    void Publish(bool bar = true);
    void UpdateTooltips();   // refreshes the text of a visible tooltip
    void HideTooltip();
    void ShowTooltip();      // above the hovered item, after a short delay
    std::wstring TooltipText(const std::wstring& key) const;
    void ClickEntry(const std::wstring& key);
    // The right-hand group (Relayout): each entry from the bar's end leftwards, and the metrics they share.
    struct RightEntry {
        const wchar_t* key;
        int width;
        const wchar_t* glyph;
        bool shown;
        int left = 0;
    };
    struct EntryMetrics {
        int plate, gap, icon, hitTop, hitBottom, barEnd;
        float plateTop, iconTop;
    };
    void PlaceRight(BarScene& scene, const std::array<RightEntry, 8>& right, const EntryMetrics& m, float timePx, float datePx);
    void BeginDrag(POINT pt);
    void UpdateDrag(POINT pt);
    void EndDrag(bool commit);
    friend struct BarDropTarget;
    friend struct BarRender;
    std::wstring FileDragOver(POINT screen); // returns the app under the point
    void FileDragLeave();
    static void ClockStrings(std::wstring& time, std::wstring& date, bool seconds);
    std::unique_ptr<GlassRenderer> TryCreatePanelGlass(const Gpu& gpu, HWND hwnd, int width, int height, bool desktop) const;
    void RenderThread();
    SessionEnd RenderSession();
    void NotifyGenieDeviceLost();
    void SetRegion(bool extended); // input region: the shape, plus the room above it while magnified
    static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD thread, DWORD time);
    void GuardStacking(const wchar_t* why);
    RECT BarOverlapZone() const;
    bool MonitorCoveredByApp() const;
    bool MonitorOverlappedByApp() const;
    bool MonitorCoveredByMeetingToolbar() const;
    void UpdateCover();    // this monitor's top app is maximized or full screen, regardless of focus
    void UpdateAutoHide(); // tucks the bar away or brings it up
    void WatchPointer();   // while covered: the screen's bottom edge brings the bar up, leaving it lets it go
    void WatchNear(bool publish = true); // the pointer above the glass; a move publishes it with ordinary hover
    void UpdateNearWatch(); // starts or stops that watch (shown bar, hoverRise on)
    void WatchDisplays();  // summon the shared Dock on another display, after the hover delay
    void WatchDisplaysIfNeeded(); // its timer, in the follow mode with several displays
    void Raise();
    void PlaceWindows(const RECT& win, bool shown);

    bool debugCapturable_ = false;
    uint64_t dropId_ = 0;
    bool ShowOwnWindowsToCapture(bool visible); // render thread; waits for the UI thread
    HWND visual_ = nullptr;
    HWND input_ = nullptr;
    void UpdateAppBar(HMONITOR monitor, RECT bounds, int reserve);
    bool appBar_ = false;          // registered as an app bar (always shown: the screen strip is the bar's)
    int appBarReserved_ = 0;       // px kept on appBarEdge_ of appBarMonitor_
    HMONITOR appBarMonitor_ = nullptr;
    BarEdge appBarEdge_ = BarEdge::Bottom;
    RECT appBarRect_{};
    RECT appBarWant_{};
    BarEdge edge_ = BarEdge::Bottom;
    std::unique_ptr<Surface> tooltip_;
    std::wstring tooltipText_;
    D2D1_SIZE_F tooltipSize_{};
    float tooltipTailX_ = 0;
    Settings settings_;
    std::vector<TaskItem> items_;
    std::vector<TaskItem> allItems_; // unfiltered task model, shared by every display
    bool filterTasks_ = false;
    bool layoutPending_ = false; // coalesce window-move events
    bool coverPending_ = false; // a look at what covers the monitor is due (kCoverTimer)
    ULONGLONG expectCoverUntil_ = 0; // ExpectCover
    std::map<std::wstring, CachedIcon> icons_;
    uint64_t nextIconId_ = 1;
    BarScene scene_;
    std::vector<Hit> hits_;
    bool previewShown_ = false; // the hovered app's windows are previewed instead of its tooltip
    bool visible_ = true;
    bool tracking_ = false;
    std::wstring pressed_;
    bool pressedLeft_ = false;
    POINT pressPt_{};
    // Drag reorder (client px). The dragged item follows the pointer; the
    // others make room at `dragInsert_`.
    bool dragging_ = false;
    std::wstring dragKey_;
    int dragInsert_ = -1;
    int dragX_ = 0, grabDx_ = 0;
    // App strip metrics of the last layout (client px).
    int appsLeft_ = 0, pitch_ = 1, plate_ = 0, shown_ = 0;
    std::vector<std::wstring> hidden_; // apps behind the overflow entry
    int unread_ = 0;
    bool dnd_ = false;
    // The now-playing entry (SetMusic): its level feed (under lock_, read through the snapshot), whether it shows, its tooltip.
    std::shared_ptr<const MusicFeed> music_;
    bool musicShown_ = false;
    std::wstring musicTip_;
    SystemStatus status_;
    bool belowTray_ = false;   // yielded to the system taskbar the user is using
    bool trayHeldUp_ = false;  // the system taskbar is up without the user (logged once)
    HWND previousForeground_ = nullptr;
    RECT applied_{};      // last window rect given to SetWindowPos
    int appliedRgn_[7]{}; // bar width, height, radius, insets, extension and edge of the last region
    // Room around the shape inside the windows (px): sides (shadow plus the
    // magnified bar's stretch), top (shadow plus magnified icons).
    int insetX_ = 0, insetTop_ = 0;
    int rise_ = 0;        // how far magnified or hopping icons reach above the shape
    int radius_ = 0;
    bool regionExtended_ = false;
    std::map<std::wstring, double> bounces_; // key -> start (ClockMs)
    std::map<HWND, GlassPanel> panels_;
    // Stacking guard.
    HWINEVENTHOOK hooks_[3]{};
    // Auto-hide: `covered_` by a maximized or full-screen app, `revealed_` by
    // the pointer; `tucked_` slides the bar off the screen, after which its
    // windows are hidden (`slidOut_`).
    bool covered_ = false, revealed_ = false, tucked_ = false, slidOut_ = false;
    // The pointer just above the glass: its place along the bar, and what answers it —
    // the glass rising (hoverRise and riseNear) and the icons magnifying (magnifyNear).
    float nearX_ = -1;
    bool nearRise_ = false, nearMagnify_ = false;
    NearHover near_;
    ULONGLONG leftAt_ = 0; // when the pointer left a revealed bar
    std::map<HWND, std::pair<int, ULONGLONG>> raises_; // windows we were raised over: count, first time
    HWND lastCover_ = nullptr;
    bool appliedVisible_ = false;
    std::wstring fileHoverKey_;
    winrt::com_ptr<IDropTarget> dropTarget_;
    bool oleReady_ = false;
    RECT window_{};
    RECT monitor_{};
    HMONITOR hmonitor_ = nullptr;
    std::vector<DisplayInfo> displays_;
    std::vector<UINT> displayDpis_;
    std::wstring activeDisplay_;
    std::wstring displayOverride_; // an extra taskbar in all-displays mode
    HMONITOR summonMonitor_ = nullptr;
    ULONGLONG summonAt_ = 0;
    ULONGLONG summonGraceUntil_ = 0;
    unsigned displayGeneration_ = 0;
    float scale_ = 1;
    winrt::com_ptr<IDWriteFactory3> dwrite_; // UI-thread text measurement

    std::mutex lock_;
    Snapshot snapshot_;
    std::atomic<uint64_t> publishSeq_{0};
    std::mutex genieLock_;
    GenieSceneSnapshot genieScene_;
    std::atomic<uint64_t> genieSeq_{0};
    HWND genieOverlay_ = nullptr;
    uint64_t genieEpoch_ = 0;
    HANDLE wake_ = nullptr;  // snapshot changed
    HANDLE quit_ = nullptr;
    HANDLE frame_ = nullptr; // a capture frame arrived
    HANDLE captureAck_ = nullptr; // the UI thread changed the windows' capture exclusion
    std::atomic<bool> captureVisible_{false}; // Atrium's windows show in screenshots (render thread)
    CapturePolicy capturePolicy_;
    std::thread render_;
};

} // namespace atrium
