// User-adjustable appearance and layout. One flat JSON object, shared with
// Atrium.Settings (settings/Atrium.Settings/BarSettings.cs) and the pipe
// protocol (docs/SETTINGS-PROTOCOL.md). Lengths are in DIP.
#pragma once

#include "pinned_app.h"
#include "stage_excluded.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace atrium {

// The control centre's optional modules (controlModules), in the settings app's order.
inline constexpr const wchar_t* kControlModules[] = {L"stage", L"energysaver", L"darkmode", L"hotspot", L"dnd", L"airplane"};

struct Settings {
    static constexpr int kVersion = 2; // 2: Windows notifications on by default (v1 files are migrated)

    // Material (presets change only these). Liquid follows macOS Liquid Glass:
    // nearly clear centre, strong lensing over a wide rim, visible dispersion.
    std::wstring preset = L"liquid"; // liquid | frosted | custom (informational)
    double transparency = 0.95;      // 0 = fully tinted, 1 = clear glass
    double blur = 0.50;              // 0 = sharp backdrop, 1 = fully frosted
    double refraction = 39;          // max sample displacement at the rim
    double edge = 10;                // width of the refracting rim
    double dispersion = 0.30;        // relative channel spread of the refraction
    double highlight = 0.60;
    double shadow = 0.10;
    // The rim's coverage transition in px, and how much wider it grows over light content (where the
    // shadow is the only rim signal). Tunable from the material page.
    double edgeSoftness = 2.0;
    double edgeLightBoost = 0.8;
    uint32_t tint = 0xFFFFFF;        // 0xRRGGBB

    // Layout and content.
    double cornerRadius = 30;        // clamped to half the bar height when drawn
    double width = 1200;             // maximum width when autoWidth
    double height = 60;
    double margin = 10;              // gap above the bottom of the work area
    double iconSize = 30;
    double magnify = 1.7;            // dock magnification under the pointer: icon scale, 1 = off
    bool startTray = true;           // the tray's button in the start menu's capsule
    bool startStatus = true;         // the network, volume and battery buttons there
    // The start menu's: small | medium | large | xlarge; empty: as start.json has it
    // (older files). `startIcon`: how large its icons and folders are drawn, four
    // steps of its own.
    std::wstring startSize;
    std::wstring startIcon;
    bool startCentered = false;       // the start menu opens in the middle of the screen, not at the left
    bool startWithBar = false;        // the Windows key brings an auto-hidden bar up with the start menu
    bool hoverRise = false;           // glass rises with the pointer's magnification wave
    // A pointer resting just above the bar is answered ahead of time: the glass rises
    // to it (with hoverRise) and the icons magnify — each on its own switch.
    bool riseNear = true;
    bool magnifyNear = false; // off unless chosen: the icons would grow under a pointer working just above the bar
    double nearEnter = 40, nearLeave = 48; // DIP above the glass where that starts, and where it lets go
    bool autoWidth = true;           // shrink to the content
    bool showClock = true;
    bool showClockSeconds = false;    // seconds after the minutes, in the system's long time format
    bool showTray = true;
    bool showStatus = true;
    bool showNotifications = true;
    bool showDesktop = false;        // the Show Desktop slot at the bar's right end
    bool showRunning = true;
    bool showLaunchpad = true;       // the Launchpad entry next to the start logo
    std::wstring launchpadMaterial = L"bar"; // bar: the bar's glass | tint: the glass under a black veil
    std::wstring launchpadLayout = L"window"; // window: a window in the middle | fullscreen: the whole screen, as macOS
    bool showNowPlaying = true;      // the now-playing entry (visible while a player has a media session)
    bool nowPlayingShowPaused = true;
    std::wstring nowPlayingMotion = L"normal"; // off | subtle | normal | vivid
    std::wstring nowPlayingRhythm = L"live";   // live: the bars follow the sound | loop: a steady animation
    std::wstring nowPlayingAppMode = L"all";   // all | allow | block
    std::vector<std::wstring> nowPlayingApps;  // AUMIDs or exe names (max 64)
    bool showFolderStacks = false;   // the folder stack entry left of the tray
    std::wstring folderStackLayout = L"fan";   // fan | column
    // The stack entry's own icon: mono (one outline in the bar's ink, as the other entries) |
    // color (a folder in macOS blue, as large as the apps).
    std::wstring folderStackIcon = L"mono";
    // The expanded stack stays open when the user clicks elsewhere: only a second click on its entry
    // closes it (false: losing the focus closes it, as the other popups).
    bool folderStackPinned = false;
    int folderStackMax = 8;          // 4..24
    std::vector<std::wstring> folderStacks;    // folder paths (max 32)
    std::vector<std::wstring> folderStackFiles; // file paths (max 32)
    // The interface language of the bar, its popups, the widgets and the settings app.
    // auto follows Windows' UI language (Chinese -> zh, anything else -> en).
    std::wstring language = L"auto"; // auto | zh | en
    std::wstring textColor = L"auto"; // auto | light | dark
    // When the bar steps out of the way: never | fullscreen (a maximized or full-screen app is in front) | always (waits at the screen edge).
    // The pointer at the edge brings it up. With barDodge (not in always), any overlapping non-transient window also hides it.
    std::wstring autoHide = L"fullscreen";
    bool barDodge = false;           // always or fullscreen: also hide whenever any window overlaps the bar
    bool barReserve = true;          // always shown: the bar keeps its strip of the screen, maximized windows end before it (#40, #129)
    bool barFullscreenHide = true;   // always shown: a full-screen app in front (a video, F11) has the screen, the bar goes (#134)
    std::wstring barEdge = L"bottom"; // bottom | top | left | right
    std::wstring statusStyle = L"ring"; // the status entry: ring (a ring round the Wi-Fi fan) | icons (network, volume, battery)
    std::vector<std::wstring> controlModules{L"stage"}; // the control centre's optional modules, in order (kControlModules)
    // One Dock, summoned at another display's bottom edge, as on macOS.
    std::wstring dockDisplayMode = L"follow"; // follow (at the active display) | primary | fixed | all
    double dockDisplayDelay = 450;   // milliseconds at another display's bottom edge (100..2000)
    std::wstring dockDisplay;        // fixed display's device-interface ID
    std::wstring desktopDisplay;     // widgets and Stage Manager; empty follows the system primary
    std::wstring taskbarApps = L"all"; // all | primaryAndWindow | window
    bool taskbarRightPrimaryOnly = false; // hide the right-side group on non-primary displays
    // An icon's right-click menu lists the app's own jump list entries (recent
    // places, tasks) and the system's ("run as administrator", "properties").
    bool windowsContextMenu = true;
    // Notifications (Atrium's own cards above the bell).
    double notifyTimeout = 6;        // seconds a popup stays (3..30)
    bool notifyAlways = false;       // popups stay until dismissed (ignores notifyTimeout)
    std::wstring notifyPlacement = L"dock"; // dock (default) | topLeft | topRight | bottomLeft | bottomRight (the main screen's)
    bool notifyHideBody = false;     // privacy: no message text; cards excluded from screen capture
    bool notifyFullscreen = false;   // popups also over full-screen apps (never games)
    bool imeStatus = true;           // the status icon shows the foreground window's input language (issue #56)
    bool imeBubble = true;           // with imeStatus: a change is announced by a bubble rising from the bottom while the bar is hidden
    bool notifyListener = true;      // Windows notifications as Atrium cards (UserNotificationListener)
    bool notifyFeishu = true;        // Feishu messages as Atrium cards (its own popup, read through UI Automation)
    bool notifyWeChat = true;        // WeChat messages as Atrium cards (its chat list, read through UI Automation)
    bool wechatBlockOfficial = false; // ... but none for its official accounts (公众号, 订阅号, 服务号)
    bool notifyQQ = true;            // QQ's messages (its Windows notifications; a flash without one: a plain card)
    bool notifyWeCom = true;         // WeCom's messages (a flash of its window or tray icon: a plain card)
    bool notifyDingTalk = true;      // DingTalk's messages (likewise)
    // An app asking for attention (a new message: its window or tray icon flashing) on the bar.
    bool attentionLight = true;      // its icon's plate lit white until it is opened
    bool attentionBounce = false;    // its icon hops a few times
    bool barBadges = true;           // unread count badges on taskbar icons
    // Three independent modules: the bar (with its popups and Atrium's notification cards), the
    // desktop widgets and Stage Manager. Any of them may run alone.
    bool bar = true;                 // the glass bar; off: Windows' own taskbar and banners stay in charge
    bool widgets = true;             // desktop widgets (their layout is widgets.json)
    bool widgetsOnRight = false;     // mirror the logical grid without rewriting widgets.json
    std::wstring widgetBackdrop = L"desktop"; // the widgets' glass: desktop (captured live, D38) | wallpaper (the files, least work)
    // Stage Manager (ui/stage_manager.h): the app in front on the stage, the others in a strip at the side.
    bool stageManager = false;
    bool stageOnRight = false;       // the strip's side; left by default (the widgets take the other side)
    bool stageAutoHide = false;      // the strip only comes out while the pointer rests on its edge of the screen
    bool stageMinimize = false;      // a window minimized on the stage goes to the strip
    bool stageDragMerge = true;      // a window released over a group in the strip joins it
    bool stageAvoid = true;          // always shown, a window taking the stage over the strip moves beside it
    bool stageCloseButton = true;    // a card in the strip shows a close button (its top right) under the pointer
    bool stageScrollWrap = true;     // scrolling the strip wraps past the last card back to the first
    // Apps Stage Manager leaves alone: their windows are never put away. Defaults cover the
    // screenshot tools whose own overlay/control bar must stay on screen while recording.
    std::vector<std::wstring> stageExcluded = DefaultStageExcluded();
    // The widgets' material: glass (the bar's, its ink adapting) | dusk (the glass dimmed, white ink;
    // macOS's dimmed desktop widgets) | color (their own opaque faces, as macOS's full-colour widgets).
    std::wstring widgetMaterial = L"glass";
    // Windows' own shell (shell/system_chrome.h): put back when turned off or on exit.
    bool hideTaskbar = true;         // the system taskbar is hidden while Atrium runs (a new installation's default)
    bool genieMinimize = false;      // windows minimize into their bar icon with the genie effect
    bool windowPeek = true;          // resting on a window's picture in the bar's preview shows that window, the others veiled
    bool hideBanners = false;        // Do Not Disturb: no system banners, the notifications still become cards
    bool winKeyStart = false;        // the Windows key alone opens Atrium's start menu instead of Windows'
    bool winNumbers = false;         // Win + 1..9, 0 open the bar's apps
    bool trayHost = true;            // Atrium receives the apps' tray icons itself (shell/tray_host.h)
    bool remoteVisible = false;      // remote-control apps see Atrium; the glass shows the wallpaper
    bool startOnLogin = true;        // independent of starting/stopping the current session
    std::wstring startupMode = L"run"; // run (HKCU Run entry) | task (Task Scheduler logon task)
    bool autoUpdate = true;          // check GitHub releases on startup and periodically
    // Pinned apps; nullopt = never saved yet (first run imports the Windows pins).
    std::optional<std::vector<PinnedApp>> pins;

    // Clamps every value into its valid range (input from disk or the pipe).
    void Clamp();

    std::wstring ToJson() const;
    // Missing keys keep the values of `base`; malformed JSON returns false.
    static bool FromJson(const std::wstring& text, const Settings& base, Settings& out);
};

enum class SettingsFileState { Missing, Valid, Corrupt, FutureVersion };

// %LOCALAPPDATA%\Atrium (or --data-dir in development builds)
bool ParseDataDirArgument(const std::vector<std::wstring>& args, std::wstring& outDir, bool& specified);
std::wstring DataDirectory();
std::wstring DataDirectoryArgs();
void WriteDebugMessage(const wchar_t* message);
// %LOCALAPPDATA%\Atrium\settings.json
std::wstring SettingsPath();
// UTF-8 text files (a BOM is skipped); the write replaces the file at once.
std::wstring ReadTextFile(const std::wstring& path);
[[nodiscard]] bool WriteTextFile(const std::wstring& path, const std::wstring& text);
// Copies a corrupt file to `<stem>.corrupt.<YYYYMMDD-HHMMSS>[.<n>].json` (keeping at most `maxBackups`).
[[nodiscard]] bool BackupCorruptSettings(const std::wstring& path, int maxBackups = 3);
Settings LoadSettingsFrom(const std::wstring& path, SettingsFileState* stateOut = nullptr, bool* backupOkOut = nullptr);
Settings LoadSettings();
// True means the file was saved. A failed save says why in `error` (the path,
// the system's reason). `startupChangedOut`, when non-null, reports whether
// `startOnLogin` or `startupMode` differs from the prior valid file on disk.
[[nodiscard]] bool SaveSettingsTo(const std::wstring& path, const Settings& s, std::wstring* error = nullptr,
                                  const std::function<bool(const std::wstring&)>& backupCorrupt = {}, bool* startupChangedOut = nullptr);
[[nodiscard]] bool SaveSettings(const Settings& s, std::wstring* error = nullptr, bool* startupChangedOut = nullptr);
// The process runs below medium integrity (an executable with a Low label):
// the profile and HKCU are closed to it, and its keys cannot reach other windows.
bool LowIntegrity();
bool IsDevelopmentBuild();

} // namespace atrium
