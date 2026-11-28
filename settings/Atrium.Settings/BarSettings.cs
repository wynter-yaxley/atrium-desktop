using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Atrium.Settings;

// One pinned app of the bar. Either Path (.lnk / .exe) or Aumid identifies it.
internal sealed record PinnedApp(string Name, string Path, string Args, string Aumid, bool? MultipleWindows = null)
{
    public bool AllowsMultipleWindows(string? resolvedPath = null)
    {
        if (MultipleWindows is bool multiple) return multiple;
        string exe = System.IO.Path.GetFileNameWithoutExtension(resolvedPath ?? Path).ToLowerInvariant();
        return exe is "explorer" or "chrome" or "msedge" or "firefox" or "brave" or "opera" or "vivaldi" or "chromium" ||
               Aumid.Equals("Microsoft.Windows.Explorer", StringComparison.OrdinalIgnoreCase);
    }
}

// Mirrors native/core/settings.h (same keys, ranges and presets). Lengths in DIP.
internal sealed class BarSettings
{
    public const int CurrentVersion = 2;

    public string Preset { get; set; } = "liquid";
    public double Transparency { get; set; } = 0.95;
    public double CornerRadius { get; set; } = 30;
    public double Blur { get; set; } = 0.50;
    public double Refraction { get; set; } = 30;
    public double Edge { get; set; } = 24;
    public double Dispersion { get; set; } = 0.30;
    public double Highlight { get; set; } = 0.80;
    public double Shadow { get; set; } = 0.30;
    public uint Tint { get; set; } = 0xFFFFFF;
    public double Width { get; set; } = 1200;
    public double Height { get; set; } = 60;
    public double Margin { get; set; } = 10;
    public double IconSize { get; set; } = 30;
    public double IconSpacing { get; set; } = 5;       // DIP between neighbouring icon plates (apps and right-hand entries)
    public double Magnify { get; set; } = 1.7;         // icon scale under the pointer, 1 = off
    public bool HoverRise { get; set; }               // glass follows the magnification wave (off unless chosen)
    public bool RiseNear { get; set; } = true;         // and for a pointer resting just above the bar
    public bool MagnifyNear { get; set; }              // the icons magnify for that pointer too, on its own switch (off unless chosen)
    public double NearEnter { get; set; } = 40;        // DIP above the glass where that starts
    public double NearLeave { get; set; } = 48;        // and where it lets go again
    public bool AutoWidth { get; set; } = true;
    public bool ShowClock { get; set; } = true;
    public bool ShowClockSeconds { get; set; }        // seconds after the minutes (off unless chosen)
    public bool ShowTray { get; set; } = true;
    public bool TrayHost { get; set; } = true;         // Atrium receives the apps' tray icons itself
    public bool StartTray { get; set; } = true;        // the tray's button in the start menu
    public bool StartStatus { get; set; } = true;      // its network, volume and battery buttons
    public bool StartCentered { get; set; }            // it opens in the middle of the screen, not at the left
    public bool StartWithBar { get; set; }             // the Windows key brings an auto-hidden bar up with it
    // The start menu's size and its icons' size: small | medium | large | xlarge;
    // empty: not known yet (the core fills it in from start.json).
    public string StartSize { get; set; } = "";
    public string StartIcon { get; set; } = "";
    public bool ShowStatus { get; set; } = true;
    public bool ShowNotifications { get; set; } = true;
    public bool ShowDesktop { get; set; }              // the Show Desktop slot at the bar's right end
    public bool ShowRunning { get; set; } = true;
    public bool ShowLaunchpad { get; set; } = true;
    public string LaunchpadMaterial { get; set; } = "bar"; // bar | tint
    public string LaunchpadLayout { get; set; } = "window"; // window | fullscreen
    public bool ShowNowPlaying { get; set; } = true;
    public bool NowPlayingShowPaused { get; set; } = true;
    public string NowPlayingMotion { get; set; } = "normal"; // off | subtle | normal | vivid
    public string NowPlayingRhythm { get; set; } = "live"; // live | loop
    public string NowPlayingAppMode { get; set; } = "all";   // all | allow | block
    public List<string> NowPlayingApps { get; set; } = [];
    public bool ShowFolderStacks { get; set; }
    public string FolderStackLayout { get; set; } = "fan";   // fan | column
    public int FolderStackMax { get; set; } = 8;
    public List<string> FolderStacks { get; set; } = [];
    public List<string> FolderStackFiles { get; set; } = [];
    public string TextColor { get; set; } = "auto"; // auto | light | dark
    public string Language { get; set; } = "auto"; // auto (follow Windows) | a locale tag: en, zh-Hans, ...
    public string AutoHide { get; set; } = "fullscreen"; // never | fullscreen | always
    public bool BarDodge { get; set; }                  // Visible / Full screen: also hide whenever a window overlaps the bar
    public bool BarReserve { get; set; } = true;        // always shown: the bar keeps its strip, maximized windows end before it
    public bool BarFullscreenHide { get; set; } = true; // always shown: a full-screen app in front (a video, F11) has the screen
    public string BarEdge { get; set; } = "bottom"; // bottom | top | left | right
    public string StatusStyle { get; set; } = "ring"; // ring | icons (network, volume, battery as on the Windows taskbar)
    // The control centre's optional modules, in order; kControlModules in native/core/settings.h.
    public static readonly string[] ControlModuleIds = ["stage", "energysaver", "darkmode", "hotspot", "dnd", "airplane"];
    public List<string> ControlModules { get; set; } = ["stage"];
    public string DockDisplayMode { get; set; } = "follow"; // primary | follow | fixed | all
    public double DockDisplayDelay { get; set; } = 450; // milliseconds at another display's bottom edge
    public string DockDisplay { get; set; } = "";
    public string DesktopDisplay { get; set; } = ""; // empty follows the system primary
    public string TaskbarApps { get; set; } = "all"; // all | primaryAndWindow | window
    public bool TaskbarRightPrimaryOnly { get; set; } // hide the right-side group on non-primary displays
    // An icon's right-click menu lists the app's own jump list entries and the
    // system's ("run as administrator", "properties").
    public bool WindowsContextMenu { get; set; } = true;
    public bool ClickWindowPicker { get; set; }
    public double NotifyTimeout { get; set; } = 6;    // seconds a popup stays
    public bool NotifyAlways { get; set; }            // popups stay until dismissed
    public string NotifyPlacement { get; set; } = "dock"; // dock (default) | topLeft | topRight | bottomLeft | bottomRight
    public static readonly string[] NotifyCorners = ["topLeft", "topRight", "bottomLeft", "bottomRight"];
    public bool NotifyHideBody { get; set; }           // privacy: no message text, excluded from capture
    public bool NotifyFullscreen { get; set; }         // popups also over full-screen apps (never games)
    public bool ImeStatus { get; set; } = true;        // the status icon shows the foreground window's input language
    public bool ImeBubble { get; set; } = true;        // with ImeStatus: a bubble from the bottom announces a change while the bar is hidden
    public bool NotifyListener { get; set; } = true;   // Windows notifications as Atrium cards
    public bool NotifyFeishu { get; set; } = true;     // Feishu messages as Atrium cards (its own popup, read only)
    public bool NotifyWeChat { get; set; } = true;     // WeChat messages as Atrium cards (its chat list, read only)
    public bool WeChatBlockOfficial { get; set; }       // ... but none for its official accounts (公众号, 订阅号, 服务号)
    public bool NotifyQQ { get; set; } = true;         // QQ's messages (its Windows notifications)
    public bool NotifyWeCom { get; set; } = true;      // WeCom's messages (a card when it flashes for one)
    public bool NotifyDingTalk { get; set; } = true;   // DingTalk's messages (likewise)
    public bool AttentionLight { get; set; } = true;   // an app asking for attention: its icon lit white on the bar
    public bool AttentionBounce { get; set; }          // its icon hops a few times
    public bool BarBadges { get; set; } = true;        // unread count badges on taskbar icons
    public bool Bar { get; set; } = true;              // the glass bar module (off: Windows' taskbar and banners)
    public bool Widgets { get; set; } = true;          // desktop widgets (their layout is the core's widgets.json)
    public bool WidgetsOnRight { get; set; }
    public string WidgetBackdrop { get; set; } = "desktop"; // desktop (captured live) | wallpaper (the files, least work)
    public bool StageManager { get; set; }             // the app in front on the stage, the others in a strip at the side
    public bool StageOnRight { get; set; }             // the strip's side; left by default (the widgets take the other side)
    public bool StageAutoHide { get; set; }            // the strip only comes out while the pointer rests on its edge
    public bool StageMinimize { get; set; }            // a window minimized on the stage goes to the strip (off unless chosen)
    public bool StageDragMerge { get; set; } = true;   // a window released over a group in the strip joins it
    public bool StageAvoid { get; set; } = true;       // always shown, a window taking the stage over the strip moves beside it
    public bool StageCloseButton { get; set; } = true; // a card in the strip shows a close button under the pointer
    public bool StageScrollWrap { get; set; } = true;  // scrolling the strip wraps past the last card back to the first
    // Apps Stage Manager leaves alone; the default is the Windows Snipping Tool, whose overlay
    // and control bar must stay on screen while recording. Other tools can be added.
    // Keep in step with DefaultStageExcluded() in native/core/stage_excluded.h.
    public static readonly string[] StageExcludedDefaults = ["snippingtool.exe", "screenclippinghost.exe", "microsoft.screensketch_"];
    public List<string> StageExcluded { get; set; } = [.. StageExcludedDefaults];
    public string WidgetMaterial { get; set; } = "glass"; // glass (the bar's) | dusk (dimmed glass, white ink) | color (their own faces)
    public bool HideTaskbar { get; set; } = true;      // the system taskbar is hidden while Atrium runs
    public bool GenieMinimize { get; set; }            // windows minimize into their bar icon with the genie effect
    public bool WindowPeek { get; set; } = true;       // resting on a window's picture in the bar's preview shows that window, the others veiled
    public bool HideBanners { get; set; }              // Do Not Disturb: no system banners, notifications still become cards
    public bool RemoteVisible { get; set; }            // remote-control apps see Atrium; the glass shows the wallpaper
    public bool WinKeyStart { get; set; }              // the Windows key alone opens Atrium's start menu
    public bool WinNumbers { get; set; }               // Win + 1..9, 0 open the bar's apps
    public bool StartOnLogin { get; set; } = true;
    public string StartupMode { get; set; } = "run";   // run | task
    public bool AutoUpdate { get; set; } = true;
    // Null = the key was absent (never saved); the core then imports the Windows pins.
    public List<PinnedApp>? Pins { get; set; }

    public static BarSettings Liquid() => new();

    public static BarSettings Frosted() => new()
    {
        Preset = "frosted",
        Transparency = 0.80,
        Blur = 1.0,
        Refraction = 6,
        Edge = 10,
        Dispersion = 0.04,
        Highlight = 0.45,
        Shadow = 0.30,
        Tint = 0xF5F5FA,
    };

    // Copies the material of `preset`, keeping layout and content options.
    public void ApplyMaterial(BarSettings preset)
    {
        Preset = preset.Preset;
        Transparency = preset.Transparency;
        Blur = preset.Blur;
        Refraction = preset.Refraction;
        Edge = preset.Edge;
        Dispersion = preset.Dispersion;
        Highlight = preset.Highlight;
        Shadow = preset.Shadow;
        Tint = preset.Tint;
    }

    public BarSettings Clone()
    {
        var copy = (BarSettings)MemberwiseClone();
        copy.Pins = Pins == null ? null : [.. Pins];
        copy.StageExcluded = [.. StageExcluded];
        copy.NowPlayingApps = [.. NowPlayingApps];
        copy.FolderStacks = [.. FolderStacks];
        copy.FolderStackFiles = [.. FolderStackFiles];
        copy.ControlModules = [.. ControlModules];
        return copy;
    }

    public JsonObject ToJson()
    {
        var o = new JsonObject
        {
            ["version"] = CurrentVersion,
            ["preset"] = Preset,
            ["transparency"] = Transparency,
            ["cornerRadius"] = CornerRadius,
            ["blur"] = Blur,
            ["refraction"] = Refraction,
            ["edge"] = Edge,
            ["dispersion"] = Dispersion,
            ["highlight"] = Highlight,
            ["shadow"] = Shadow,
            ["tint"] = "#" + Tint.ToString("X6", CultureInfo.InvariantCulture),
            ["width"] = Width,
            ["height"] = Height,
            ["margin"] = Margin,
            ["iconSize"] = IconSize,
            ["iconSpacing"] = IconSpacing,
            ["magnify"] = Magnify,
            ["hoverRise"] = HoverRise,
            ["riseNear"] = RiseNear,
            ["magnifyNear"] = MagnifyNear,
            ["nearEnter"] = NearEnter,
            ["nearLeave"] = NearLeave,
            ["showClock"] = ShowClock,
            ["showClockSeconds"] = ShowClockSeconds,
            ["showTray"] = ShowTray,
            ["trayHost"] = TrayHost,
            ["startTray"] = StartTray,
            ["startStatus"] = StartStatus,
            ["startCentered"] = StartCentered,
            ["startWithBar"] = StartWithBar,
            ["showStatus"] = ShowStatus,
            ["showNotifications"] = ShowNotifications,
            ["showDesktop"] = ShowDesktop,
            ["autoWidth"] = AutoWidth,
            ["showRunning"] = ShowRunning,
            ["showLaunchpad"] = ShowLaunchpad,
            ["launchpadMaterial"] = LaunchpadMaterial,
            ["launchpadLayout"] = LaunchpadLayout,
            ["showNowPlaying"] = ShowNowPlaying,
            ["nowPlayingShowPaused"] = NowPlayingShowPaused,
            ["nowPlayingMotion"] = NowPlayingMotion,
            ["nowPlayingRhythm"] = NowPlayingRhythm,
            ["nowPlayingAppMode"] = NowPlayingAppMode,
            ["showFolderStacks"] = ShowFolderStacks,
            ["folderStackLayout"] = FolderStackLayout,
            ["folderStackMax"] = (double)FolderStackMax, // as the other numbers: read back as a double in memory too
            ["textColor"] = TextColor,
            ["language"] = Language,
            ["autoHide"] = AutoHide,
            ["barDodge"] = BarDodge,
            ["barReserve"] = BarReserve,
            ["barFullscreenHide"] = BarFullscreenHide,
            ["barEdge"] = BarEdge,
            ["statusStyle"] = StatusStyle,
            ["dockDisplayMode"] = DockDisplayMode,
            ["dockDisplayDelay"] = DockDisplayDelay,
            ["dockDisplay"] = DockDisplay,
            ["desktopDisplay"] = DesktopDisplay,
            ["taskbarApps"] = TaskbarApps,
            ["taskbarRightPrimaryOnly"] = TaskbarRightPrimaryOnly,
            ["windowsContextMenu"] = WindowsContextMenu,
            ["clickWindowPicker"] = ClickWindowPicker,
            ["notifyTimeout"] = NotifyTimeout,
            ["notifyAlways"] = NotifyAlways,
            ["notifyPlacement"] = NotifyPlacement,
            ["notifyHideBody"] = NotifyHideBody,
            ["notifyFullscreen"] = NotifyFullscreen,
            ["imeStatus"] = ImeStatus,
            ["imeBubble"] = ImeBubble,
            ["notifyListener"] = NotifyListener,
            ["notifyWeChat"] = NotifyWeChat,
            ["wechatBlockOfficial"] = WeChatBlockOfficial,
            ["notifyFeishu"] = NotifyFeishu,
            ["notifyQQ"] = NotifyQQ,
            ["notifyWeCom"] = NotifyWeCom,
            ["notifyDingTalk"] = NotifyDingTalk,
            ["attentionLight"] = AttentionLight,
            ["attentionBounce"] = AttentionBounce,
            ["barBadges"] = BarBadges,
            ["bar"] = Bar,
            ["widgets"] = Widgets,
            ["widgetsOnRight"] = WidgetsOnRight,
            ["widgetBackdrop"] = WidgetBackdrop,
            ["stageManager"] = StageManager,
            ["stageOnRight"] = StageOnRight,
            ["stageAutoHide"] = StageAutoHide,
            ["stageMinimize"] = StageMinimize,
            ["stageDragMerge"] = StageDragMerge,
            ["stageAvoid"] = StageAvoid,
            ["stageCloseButton"] = StageCloseButton,
            ["stageScrollWrap"] = StageScrollWrap,
            ["widgetMaterial"] = WidgetMaterial,
            ["hideTaskbar"] = HideTaskbar,
            ["genieMinimize"] = GenieMinimize,
            ["windowPeek"] = WindowPeek,
            ["hideBanners"] = HideBanners,
            ["remoteVisible"] = RemoteVisible,
            ["winKeyStart"] = WinKeyStart,
            ["winNumbers"] = WinNumbers,
            ["startOnLogin"] = StartOnLogin,
            ["startupMode"] = StartupMode,
            ["autoUpdate"] = AutoUpdate,
        };
        static JsonArray ToStringArray(List<string> list)
        {
            var array = new JsonArray();
            foreach (string item in list)
                array.Add(item);
            return array;
        }
        o["stageExcluded"] = ToStringArray(StageExcluded);
        o["nowPlayingApps"] = ToStringArray(NowPlayingApps);
        o["folderStacks"] = ToStringArray(FolderStacks);
        o["folderStackFiles"] = ToStringArray(FolderStackFiles);
        o["controlModules"] = ToStringArray(ControlModules);
        if (StartSize.Length > 0)
            o["startSize"] = StartSize;
        if (StartIcon.Length > 0)
            o["startIcon"] = StartIcon;
        if (Pins != null)
        {
            var a = new JsonArray();
            foreach (var p in Pins)
            {
                var pin = new JsonObject { ["name"] = p.Name, ["path"] = p.Path, ["args"] = p.Args, ["aumid"] = p.Aumid };
                if (p.MultipleWindows is bool multiple) pin["multipleWindows"] = multiple;
                a.Add(pin);
            }
            o["pins"] = a;
        }
        return o;
    }

    // Missing or malformed keys keep the values of `fallback`.
    public static BarSettings FromJson(JsonObject? o, BarSettings fallback)
    {
        var s = new BarSettings();
        var f = fallback;
        double Num(string k, double d) => o?[k] is JsonValue v && v.TryGetValue(out double x) && double.IsFinite(x) ? x : d;
        string Str(string key, string value) => o?[key] is JsonValue v && v.TryGetValue(out string? text) && text != null ? text : value;
        string preset = Str("preset", f.Preset);
        s.Preset = preset is "liquid" or "frosted" ? preset : "custom";
        s.Transparency = Math.Clamp(Num("transparency", f.Transparency), 0, 1);
        s.CornerRadius = Math.Clamp(Num("cornerRadius", f.CornerRadius), 0, 200);
        s.Blur = Math.Clamp(Num("blur", f.Blur), 0, 1);
        s.Refraction = Math.Clamp(Num("refraction", f.Refraction), 0, 60);
        s.Edge = Math.Clamp(Num("edge", f.Edge), 1, 80);
        s.Dispersion = Math.Clamp(Num("dispersion", f.Dispersion), 0, 0.5);
        s.Highlight = Math.Clamp(Num("highlight", f.Highlight), 0, 1);
        s.Shadow = Math.Clamp(Num("shadow", f.Shadow), 0, 1);
        s.Width = Math.Clamp(Num("width", f.Width), 200, 4000);
        s.Height = Math.Clamp(Num("height", f.Height), 32, 160);
        s.Margin = Math.Clamp(Num("margin", f.Margin), 0, 400);
        s.IconSize = Math.Clamp(Num("iconSize", f.IconSize), 16, 64);
        s.IconSpacing = Math.Clamp(Num("iconSpacing", f.IconSpacing), 2, 40);
        s.Magnify = Math.Clamp(Num("magnify", f.Magnify), 1, 2);
        s.Tint = f.Tint & 0xFFFFFFu;
        if (o?["tint"] is JsonValue t && t.TryGetValue(out string? ts) && ts is { Length: 7 } && ts[0] == '#' &&
            ts.Skip(1).All(char.IsAsciiHexDigit) &&
            uint.TryParse(ts.AsSpan(1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out uint tint))
        {
            s.Tint = tint & 0xFFFFFFu;
        }

        bool Flag(string k, bool d) => o?[k] is JsonValue v && v.TryGetValue(out bool b) ? b : d;
        s.HoverRise = Flag("hoverRise", f.HoverRise);
        s.RiseNear = Flag("riseNear", f.RiseNear);
        s.MagnifyNear = Flag("magnifyNear", f.MagnifyNear);
        s.NearEnter = Math.Clamp(Num("nearEnter", f.NearEnter), 2, 160);
        s.NearLeave = Math.Clamp(Num("nearLeave", f.NearLeave), 2, 200);
        s.ShowClock = Flag("showClock", f.ShowClock);
        s.ShowClockSeconds = Flag("showClockSeconds", f.ShowClockSeconds);
        s.ShowTray = Flag("showTray", f.ShowTray);
        s.TrayHost = Flag("trayHost", f.TrayHost);
        s.StartTray = Flag("startTray", f.StartTray);
        s.StartStatus = Flag("startStatus", f.StartStatus);
        s.StartCentered = Flag("startCentered", f.StartCentered);
        s.StartWithBar = Flag("startWithBar", f.StartWithBar);
        string startSize = Str("startSize", f.StartSize);
        s.StartSize = startSize is "small" or "medium" or "large" or "xlarge" ? startSize : "";
        string startIcon = Str("startIcon", f.StartIcon);
        s.StartIcon = startIcon is "small" or "medium" or "large" or "xlarge" ? startIcon : "";
        s.ShowStatus = Flag("showStatus", f.ShowStatus);
        s.ShowNotifications = Flag("showNotifications", f.ShowNotifications);
        s.ShowDesktop = Flag("showDesktop", f.ShowDesktop);
        s.AutoWidth = Flag("autoWidth", f.AutoWidth);
        s.ShowRunning = Flag("showRunning", f.ShowRunning);
        s.NotifyTimeout = Math.Clamp(Num("notifyTimeout", f.NotifyTimeout), 3, 30);
        s.NotifyAlways = Flag("notifyAlways", f.NotifyAlways);
        s.NotifyHideBody = Flag("notifyHideBody", f.NotifyHideBody);
        s.NotifyFullscreen = Flag("notifyFullscreen", f.NotifyFullscreen);
        s.ImeStatus = Flag("imeStatus", f.ImeStatus);
        s.ImeBubble = Flag("imeBubble", f.ImeBubble);
        // Version 1 files saved the old default (off): from version 2 it is on (as in the core).
        bool v1 = o?["version"] is JsonValue ver && ver.TryGetValue(out double vn) && vn < 2;
        s.NotifyListener = v1 ? f.NotifyListener : Flag("notifyListener", f.NotifyListener);
        s.NotifyWeChat = Flag("notifyWeChat", f.NotifyWeChat);
        s.WeChatBlockOfficial = Flag("wechatBlockOfficial", f.WeChatBlockOfficial);
        s.NotifyFeishu = Flag("notifyFeishu", f.NotifyFeishu);
        s.NotifyQQ = Flag("notifyQQ", f.NotifyQQ);
        s.NotifyWeCom = Flag("notifyWeCom", f.NotifyWeCom);
        s.NotifyDingTalk = Flag("notifyDingTalk", f.NotifyDingTalk);
        s.AttentionLight = Flag("attentionLight", f.AttentionLight);
        s.AttentionBounce = Flag("attentionBounce", f.AttentionBounce);
        s.BarBadges = Flag("barBadges", f.BarBadges);
        s.Bar = Flag("bar", f.Bar);
        s.Widgets = Flag("widgets", f.Widgets);
        s.WidgetsOnRight = Flag("widgetsOnRight", f.WidgetsOnRight);
        string widgetBackdrop = Str("widgetBackdrop", f.WidgetBackdrop);
        s.WidgetBackdrop = widgetBackdrop == "wallpaper" ? "wallpaper" : "desktop";
        s.StageManager = Flag("stageManager", f.StageManager);
        s.StageOnRight = Flag("stageOnRight", f.StageOnRight);
        s.StageAutoHide = Flag("stageAutoHide", f.StageAutoHide);
        s.StageMinimize = Flag("stageMinimize", f.StageMinimize);
        s.StageDragMerge = Flag("stageDragMerge", f.StageDragMerge);
        s.StageAvoid = Flag("stageAvoid", f.StageAvoid);
        s.StageCloseButton = Flag("stageCloseButton", f.StageCloseButton);
        s.StageScrollWrap = Flag("stageScrollWrap", f.StageScrollWrap);
        s.StageExcluded = ReadStringArray(o, "stageExcluded", f.StageExcluded);
        ReadExtrasJson(o, s, f);
        string widgetMaterial = Str("widgetMaterial", f.WidgetMaterial);
        s.WidgetMaterial = widgetMaterial is "dusk" or "color" ? widgetMaterial : "glass";
        s.HideTaskbar = Flag("hideTaskbar", f.HideTaskbar);
        s.GenieMinimize = Flag("genieMinimize", f.GenieMinimize);
        s.WindowPeek = Flag("windowPeek", f.WindowPeek);
        s.HideBanners = Flag("hideBanners", f.HideBanners);
        s.RemoteVisible = Flag("remoteVisible", f.RemoteVisible);
        s.WinKeyStart = Flag("winKeyStart", f.WinKeyStart);
        s.WinNumbers = Flag("winNumbers", f.WinNumbers);
        s.StartOnLogin = Flag("startOnLogin", f.StartOnLogin);
        s.StartupMode = Str("startupMode", f.StartupMode) == "task" ? "task" : "run";
        s.AutoUpdate = Flag("autoUpdate", f.AutoUpdate);
        string mode = Str("dockDisplayMode", f.DockDisplayMode);
        s.DockDisplayMode = mode is "primary" or "fixed" or "all" ? mode : "follow";
        s.DockDisplayDelay = Math.Clamp(Num("dockDisplayDelay", f.DockDisplayDelay), 100, 2000);
        s.DockDisplay = Str("dockDisplay", f.DockDisplay);
        s.DesktopDisplay = Str("desktopDisplay", f.DesktopDisplay);
        string apps = Str("taskbarApps", f.TaskbarApps);
        s.TaskbarApps = apps is "primaryAndWindow" or "window" ? apps : "all";
        s.TaskbarRightPrimaryOnly = Flag("taskbarRightPrimaryOnly", f.TaskbarRightPrimaryOnly);
        s.WindowsContextMenu = Flag("windowsContextMenu", f.WindowsContextMenu);
        s.ClickWindowPicker = Flag("clickWindowPicker", f.ClickWindowPicker);
        string placement = Str("notifyPlacement", f.NotifyPlacement);
        s.NotifyPlacement = NotifyCorners.Contains(placement) ? placement : "dock";
        string textColor = Str("textColor", f.TextColor);
        s.TextColor = textColor is "light" or "dark" ? textColor : "auto";
        string autoHide = Str("autoHide", f.AutoHide);
        s.AutoHide = autoHide is "never" or "always" ? autoHide : "fullscreen";
        s.BarDodge = Flag("barDodge", autoHide == "dodge" && o?["barDodge"] == null || f.BarDodge);
        s.BarReserve = Flag("barReserve", f.BarReserve);
        s.BarFullscreenHide = Flag("barFullscreenHide", f.BarFullscreenHide);
        string barEdge = Str("barEdge", f.BarEdge);
        s.BarEdge = barEdge is "top" or "left" or "right" ? barEdge : "bottom";
        string language = Str("language", f.Language);
        s.Language = !LocalizationValidation.ValidTag(language) ? "auto" : language == "zh" ? "zh-Hans" : language;
        string statusStyle = Str("statusStyle", f.StatusStyle);
        s.StatusStyle = statusStyle == "icons" ? "icons" : "ring";
        // Known modules only, each once, in the order given; the night light module that was is dark mode now.
        s.ControlModules = [.. ReadStringArray(o, "controlModules", f.ControlModules).Select(id => id == "nightlight" ? "darkmode" : id)
            .Where(ControlModuleIds.Contains).Distinct()];
        s.Pins = f.Pins == null ? null : [.. f.Pins];
        if (o?["pins"] is JsonArray arr)
        {
            var pins = new List<PinnedApp>();
            foreach (var node in arr)
            {
                if (node is not JsonObject e) continue;
                string PinStr(string k) => e[k] is JsonValue v && v.TryGetValue(out string? x) && x != null ? x : "";
                bool? multiple = e["multipleWindows"] is JsonValue flag && flag.TryGetValue(out bool value) ? value : null;
                var pin = new PinnedApp(PinStr("name"), PinStr("path"), PinStr("args"), PinStr("aumid"), multiple);
                if (pin.Path.Length > 0 || pin.Aumid.Length > 0) pins.Add(pin);
            }
            s.Pins = pins;
        }
        if (s.Pins is { Count: > 64 }) s.Pins.RemoveRange(64, s.Pins.Count - 64);
        return s;
    }

    private static List<string> ReadStringArray(JsonObject? o, string key, List<string>? fallback, int maxCount = int.MaxValue)
    {
        const string AsciiWs = " \t\r\n\v\f";
        var result = new List<string>();
        void AddTrimmed(string? raw)
        {
            if (raw == null || result.Count >= maxCount) return;
            string trimmed = raw.AsSpan().Trim(AsciiWs).ToString();
            if (trimmed.Length > 0) result.Add(trimmed);
        }
        if (o?[key] is JsonArray array)
        {
            foreach (var node in array)
            {
                if (node is JsonValue value && value.TryGetValue(out string? item))
                    AddTrimmed(item);
            }
        }
        else if (fallback != null)
        {
            foreach (string item in fallback)
                AddTrimmed(item);
        }
        return result;
    }

    private static bool IsFolderRootSlash(ReadOnlySpan<char> path)
    {
        if (path.Length == 1) return path[0] is '\\' or '/';
        if (path.Length == 2) return path[0] == '\\' && path[1] == '\\';
        if (path.Length == 3) return char.IsAsciiLetter(path[0]) && path[1] == ':' && path[2] is '\\' or '/';
        if (path.Length == 7 && (path.StartsWith(@"\\?\") || path.StartsWith(@"\\.\")))
            return char.IsAsciiLetter(path[4]) && path[5] == ':' && path[6] is '\\' or '/';
        return false;
    }

    private static List<string> ReadUniquePaths(JsonObject? o, string key, List<string>? fallback, bool trimTrailingSlashes, int maxCount = 32)
    {
        var raw = ReadStringArray(o, key, fallback);
        var result = new List<string>(Math.Min(raw.Count, maxCount));
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string item in raw)
        {
            var span = item.AsSpan();
            if (trimTrailingSlashes)
            {
                while (span.Length > 0 && span[^1] is '\\' or '/' && !IsFolderRootSlash(span))
                    span = span[..^1];
                if (span.Length == 0) continue;
            }
            string norm = span.ToString();
            string dedupeKey = norm.Replace('/', '\\');
            if (seen.Add(dedupeKey))
            {
                result.Add(norm);
                if (result.Count >= maxCount) break;
            }
        }
        return result;
    }

    private static void ReadExtrasJson(JsonObject? o, BarSettings s, BarSettings f)
    {
        bool Flag(string k, bool d) => o?[k] is JsonValue v && v.TryGetValue(out bool b) ? b : d;
        string Str(string k, string d) => o?[k] is JsonValue v && v.TryGetValue(out string? t) && t != null ? t : d;
        s.ShowLaunchpad = Flag("showLaunchpad", f.ShowLaunchpad);
        s.LaunchpadMaterial = Str("launchpadMaterial", f.LaunchpadMaterial) == "tint" ? "tint" : "bar";
        s.LaunchpadLayout = Str("launchpadLayout", f.LaunchpadLayout) == "fullscreen" ? "fullscreen" : "window";
        s.ShowNowPlaying = Flag("showNowPlaying", f.ShowNowPlaying);
        s.NowPlayingShowPaused = Flag("nowPlayingShowPaused", f.NowPlayingShowPaused);
        string motion = Str("nowPlayingMotion", f.NowPlayingMotion);
        s.NowPlayingMotion = motion is "off" or "subtle" or "vivid" ? motion : "normal";
        s.NowPlayingRhythm = Str("nowPlayingRhythm", f.NowPlayingRhythm) == "loop" ? "loop" : "live";
        string appMode = Str("nowPlayingAppMode", f.NowPlayingAppMode);
        s.NowPlayingAppMode = appMode is "allow" or "block" ? appMode : "all";
        s.NowPlayingApps = ReadStringArray(o, "nowPlayingApps", f.NowPlayingApps, 64);
        s.ShowFolderStacks = Flag("showFolderStacks", f.ShowFolderStacks);
        string stackLayout = Str("folderStackLayout", f.FolderStackLayout);
        s.FolderStackLayout = stackLayout == "column" ? "column" : "fan";
        double rawMax = o?["folderStackMax"] is JsonValue mv && mv.TryGetValue(out double mx) && double.IsFinite(mx) ? mx : f.FolderStackMax;
        s.FolderStackMax = (int)Math.Round(Math.Clamp(rawMax, 4, 24), MidpointRounding.AwayFromZero);
        s.FolderStacks = ReadUniquePaths(o, "folderStacks", f.FolderStacks, trimTrailingSlashes: true, 32);
        s.FolderStackFiles = ReadUniquePaths(o, "folderStackFiles", f.FolderStackFiles, trimTrailingSlashes: false, 32);
    }

    public bool SameAs(BarSettings o) =>
        SameMaterial(o) && Preset == o.Preset && CornerRadius == o.CornerRadius && Width == o.Width && Height == o.Height &&
        Margin == o.Margin && IconSize == o.IconSize && IconSpacing == o.IconSpacing && Magnify == o.Magnify &&
        HoverRise == o.HoverRise && RiseNear == o.RiseNear && MagnifyNear == o.MagnifyNear && NearEnter == o.NearEnter &&
        NearLeave == o.NearLeave && AutoWidth == o.AutoWidth && ShowClock == o.ShowClock &&
        ShowClockSeconds == o.ShowClockSeconds &&
        ShowTray == o.ShowTray && TrayHost == o.TrayHost && StartTray == o.StartTray && StartStatus == o.StartStatus &&
        StartCentered == o.StartCentered && StartWithBar == o.StartWithBar && StartSize == o.StartSize && StartIcon == o.StartIcon &&
        ShowStatus == o.ShowStatus && ShowNotifications == o.ShowNotifications && ShowDesktop == o.ShowDesktop &&
        ShowRunning == o.ShowRunning && ShowLaunchpad == o.ShowLaunchpad &&
        LaunchpadMaterial == o.LaunchpadMaterial && LaunchpadLayout == o.LaunchpadLayout &&
        ShowNowPlaying == o.ShowNowPlaying &&
        NowPlayingShowPaused == o.NowPlayingShowPaused && NowPlayingMotion == o.NowPlayingMotion && NowPlayingRhythm == o.NowPlayingRhythm &&
        NowPlayingAppMode == o.NowPlayingAppMode && NowPlayingApps.SequenceEqual(o.NowPlayingApps, StringComparer.OrdinalIgnoreCase) &&
        ShowFolderStacks == o.ShowFolderStacks &&
        FolderStackLayout == o.FolderStackLayout && FolderStackMax == o.FolderStackMax &&
        FolderStacks.SequenceEqual(o.FolderStacks, StringComparer.OrdinalIgnoreCase) &&
        FolderStackFiles.SequenceEqual(o.FolderStackFiles, StringComparer.OrdinalIgnoreCase) &&
        TextColor == o.TextColor && Language == o.Language && AutoHide == o.AutoHide &&
        BarDodge == o.BarDodge && BarReserve == o.BarReserve && BarFullscreenHide == o.BarFullscreenHide &&
        BarEdge == o.BarEdge && StatusStyle == o.StatusStyle && ControlModules.SequenceEqual(o.ControlModules) && NotifyTimeout == o.NotifyTimeout &&
        NotifyAlways == o.NotifyAlways &&
        NotifyHideBody == o.NotifyHideBody && NotifyFullscreen == o.NotifyFullscreen &&
        ImeStatus == o.ImeStatus && ImeBubble == o.ImeBubble &&
        NotifyListener == o.NotifyListener && NotifyWeChat == o.NotifyWeChat &&
        WeChatBlockOfficial == o.WeChatBlockOfficial && NotifyFeishu == o.NotifyFeishu &&
        NotifyQQ == o.NotifyQQ && NotifyWeCom == o.NotifyWeCom && NotifyDingTalk == o.NotifyDingTalk &&
        AttentionLight == o.AttentionLight && AttentionBounce == o.AttentionBounce && BarBadges == o.BarBadges && Bar == o.Bar && Widgets == o.Widgets &&
        WidgetsOnRight == o.WidgetsOnRight && WidgetBackdrop == o.WidgetBackdrop && StageManager == o.StageManager && StageOnRight == o.StageOnRight &&
        StageAutoHide == o.StageAutoHide && StageMinimize == o.StageMinimize &&
        StageDragMerge == o.StageDragMerge && StageCloseButton == o.StageCloseButton &&
        StageScrollWrap == o.StageScrollWrap &&
        StageAvoid == o.StageAvoid &&
        StageExcluded.SequenceEqual(o.StageExcluded, StringComparer.OrdinalIgnoreCase) && WidgetMaterial == o.WidgetMaterial &&
        HideTaskbar == o.HideTaskbar && GenieMinimize == o.GenieMinimize &&
        WindowPeek == o.WindowPeek && HideBanners == o.HideBanners &&
        RemoteVisible == o.RemoteVisible &&
        WinKeyStart == o.WinKeyStart && WinNumbers == o.WinNumbers &&
        StartOnLogin == o.StartOnLogin && StartupMode == o.StartupMode && AutoUpdate == o.AutoUpdate &&
        DockDisplayMode == o.DockDisplayMode && DockDisplayDelay == o.DockDisplayDelay && DockDisplay == o.DockDisplay && DesktopDisplay == o.DesktopDisplay &&
        TaskbarApps == o.TaskbarApps && TaskbarRightPrimaryOnly == o.TaskbarRightPrimaryOnly &&
        WindowsContextMenu == o.WindowsContextMenu && ClickWindowPicker == o.ClickWindowPicker &&
        NotifyPlacement == o.NotifyPlacement &&
        (Pins == o.Pins || (Pins != null && o.Pins != null && Pins.SequenceEqual(o.Pins)));

    public bool SameMaterial(BarSettings o) =>
        Transparency == o.Transparency && Blur == o.Blur && Refraction == o.Refraction && Edge == o.Edge && Dispersion == o.Dispersion &&
        Highlight == o.Highlight && Shadow == o.Shadow && Tint == o.Tint;
}

internal static class SettingsPersistence
{
    private static readonly SemaphoreSlim WriteGate = new(1, 1);

    public static void WriteAtomicText(string path, string text)
    {
        string full = Path.GetFullPath(path);
        string tmp = $"{full}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            byte[] bytes = new UTF8Encoding(false).GetBytes(text);
            using (var stream = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush(flushToDisk: true);
            }
            File.Move(tmp, full, overwrite: true);
        }
        catch
        {
            try { File.Delete(tmp); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
    }

    public static bool TryBackupCorrupt(string path, int maxBackups = 3)
    {
        if (!File.Exists(path)) return false;
        string full = Path.GetFullPath(path);
        string dir = Path.GetDirectoryName(full) ?? ".";
        string stem = Path.GetFileNameWithoutExtension(full);
        if (string.IsNullOrEmpty(stem)) stem = "settings";
        string ext = Path.GetExtension(full);
        if (string.IsNullOrEmpty(ext)) ext = ".json";
        var existing = FindCorruptBackups(dir, stem, ext);
        if (existing.Count > 0 && FilesEqual(full, existing[^1].Path)) return true;
        string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture);
        for (int i = 0; i < 100; i++)
        {
            string suffix = i == 0 ? "" : "." + i.ToString(CultureInfo.InvariantCulture);
            string candidate = Path.Combine(dir, $"{stem}.corrupt.{stamp}{suffix}{ext}");
            try
            {
                File.Copy(full, candidate, overwrite: false);
                PruneCorruptBackups(dir, stem, ext, maxBackups);
                return true;
            }
            catch (IOException) when (File.Exists(candidate)) { continue; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
        }
        return false;
    }

    public static BarSettings ReadSettings(string path)
    {
        try
        {
            if (!File.Exists(path)) return new BarSettings();
            string text = File.ReadAllText(path, Encoding.UTF8);
            if (text.Length == 0) return new BarSettings();
            if (JsonNode.Parse(text) is JsonObject obj)
                return BarSettings.FromJson(obj, new());
        }
        catch (JsonException) { }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new BarSettings(); }
        TryBackupCorrupt(path);
        return new BarSettings();
    }

    private static bool StartupDiffers(BarSettings prev, BarSettings next) =>
        prev.StartOnLogin != next.StartOnLogin || (next.StartOnLogin && prev.StartupMode != next.StartupMode);

    public static bool WriteSettings(
        string path, BarSettings s, Func<string, object?[], string>? formatError = null, Func<string, bool>? backupCorrupt = null)
    {
        WriteGate.Wait();
        try
        {
            bool startupChanged = StartupDiffers(new BarSettings(), s);
            if (File.Exists(path))
            {
                string text = File.ReadAllText(path, Encoding.UTF8);
                if (text.Length > 0)
                {
                    JsonObject? obj = null;
                    try { obj = JsonNode.Parse(text) as JsonObject; }
                    catch (JsonException) { }
                    if (obj == null)
                    {
                        bool backedUp = backupCorrupt != null ? backupCorrupt(path) : TryBackupCorrupt(path);
                        if (!backedUp)
                        {
                            throw new IOException(Format(
                                formatError,
                                "Could not back up corrupt settings file {0} before saving; original file was kept.",
                                path));
                        }
                    }
                    else
                    {
                        if (obj["version"] is JsonValue verNode && verNode.TryGetValue(out double ver) &&
                            double.IsFinite(ver) && ver > BarSettings.CurrentVersion)
                        {
                            throw new IOException(Format(
                                formatError,
                                "Could not save {0}: settings file uses a newer format version (v{1} > v{2}). Update Atrium before saving.",
                                path, ver, BarSettings.CurrentVersion));
                        }
                        startupChanged = StartupDiffers(BarSettings.FromJson(obj, new()), s);
                    }
                }
            }
            string? dir = Path.GetDirectoryName(Path.GetFullPath(path));
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            WriteAtomicText(path, s.ToJson().ToJsonString());
            return startupChanged;
        }
        finally
        {
            WriteGate.Release();
        }
    }

    private static string Format(Func<string, object?[], string>? formatError, string key, params object?[] args) =>
        formatError != null ? formatError(key, args) : string.Format(CultureInfo.InvariantCulture, key, args);

    private static bool FilesEqual(string a, string b)
    {
        try
        {
            if (new FileInfo(a).Length != new FileInfo(b).Length) return false;
            return File.ReadAllBytes(a).AsSpan().SequenceEqual(File.ReadAllBytes(b));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static List<(string Stamp, int Seq, string Path)> FindCorruptBackups(string dir, string stem, string ext)
    {
        var entries = new List<(string Stamp, int Seq, string Path)>();
        if (!Directory.Exists(dir)) return entries;
        string prefix = stem + ".corrupt.";
        foreach (string file in Directory.EnumerateFiles(dir, $"{prefix}*{ext}"))
        {
            string name = Path.GetFileName(file);
            if (name.Length <= prefix.Length + ext.Length ||
                !name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ||
                !name.EndsWith(ext, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }
            var middle = name.AsSpan(prefix.Length, name.Length - prefix.Length - ext.Length);
            if (TryParseBackupSuffix(middle, out string stamp, out int seq))
                entries.Add((stamp, seq, file));
        }
        entries.Sort((a, b) =>
        {
            int cmp = StringComparer.Ordinal.Compare(a.Stamp, b.Stamp);
            return cmp != 0 ? cmp : a.Seq.CompareTo(b.Seq);
        });
        return entries;
    }

    private static void PruneCorruptBackups(string dir, string stem, string ext, int maxBackups)
    {
        if (maxBackups < 1) return;
        var entries = FindCorruptBackups(dir, stem, ext);
        if (entries.Count <= maxBackups) return;
        for (int i = 0; i < entries.Count - maxBackups; i++)
        {
            try { File.Delete(entries[i].Path); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }

    private static bool TryParseBackupSuffix(ReadOnlySpan<char> middle, out string stamp, out int seq)
    {
        stamp = "";
        seq = 0;
        if (middle.Length < 15 || middle[8] != '-') return false;
        for (int i = 0; i < 15; i++)
        {
            if (i != 8 && !char.IsAsciiDigit(middle[i])) return false;
        }
        stamp = middle[..15].ToString();
        if (middle.Length == 15) return true;
        return middle[15] == '.' && middle.Length > 16 &&
            int.TryParse(middle[16..], NumberStyles.None, CultureInfo.InvariantCulture, out seq);
    }
}
