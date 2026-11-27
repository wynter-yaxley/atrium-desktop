using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using Atrium.Settings;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

var settings = new BarSettings
{
    DockDisplayMode = "all",
    DockDisplayDelay = 750,
    DockDisplay = @"\\?\DISPLAY#external",
    DesktopDisplay = "disconnected-screen",
    TaskbarApps = "primaryAndWindow",
    TaskbarRightPrimaryOnly = true,
    NotifyPlacement = "topRight",
    HoverRise = false,
    NearEnter = 40,
    NearLeave = 4,
};
var parsed = BarSettings.FromJson(settings.ToJson(), new());
Require(settings.SameAs(parsed), "Display preferences must survive a settings round trip.");
Require(parsed.NearEnter == 40 && parsed.NearLeave == 4,
    "A four-DIP leave setting must survive a round trip independently of enter.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"nearLeave":4}""")!.AsObject(), new()).NearLeave == 4,
    "A partial four-DIP leave update must not be replaced with the default forty-eight.");
Require(BarSettings.FromJson([], new()).SameAs(new()), "Old files must receive display defaults.");
Require(new BarSettings().ImeStatus, "The input method status is on by default.");
var imeOff = BarSettings.FromJson(JsonNode.Parse("""{"imeStatus":false}""")!.AsObject(), new());
Require(!imeOff.ImeStatus && !new BarSettings().SameAs(imeOff) && !BarSettings.FromJson(imeOff.ToJson(), new()).ImeStatus,
    "Turning the input method status off must be saved, compared and survive a round trip.");
var bubbleOff = BarSettings.FromJson(JsonNode.Parse("""{"imeBubble":false}""")!.AsObject(), new());
Require(new BarSettings().ImeBubble && !bubbleOff.ImeBubble &&
        !BarSettings.FromJson(bubbleOff.ToJson(), new()).ImeBubble && !new BarSettings().SameAs(bubbleOff),
    "The input method bubble is on by default, and off survives a round trip and marks the settings dirty.");
Require(!BarSettings.FromJson([], new()).HoverRise, "Old files must keep the glass flat.");
Require(BarSettings.FromJson([], new() { HoverRise = true }).HoverRise, "Partial updates must retain enabled hover rise.");
Require(!BarSettings.FromJson([], settings).HoverRise, "Partial updates must retain disabled hover rise.");
Require(!BarSettings.FromJson(JsonNode.Parse("""{"hoverRise":"bad"}""")!.AsObject(), settings).HoverRise,
    "Malformed hover rise must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"hoverRise":true}""")!.AsObject(), settings);
Require(parsed.HoverRise && parsed.Magnify == settings.Magnify, "Hover rise must toggle without changing icon magnification.");
Require(BarSettings.FromJson([], new()).StartStatus && BarSettings.FromJson([], new()).StartTray,
    "Old files must keep the start menu's status and tray buttons.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"startStatus":false}""")!.AsObject(), new());
Require(!parsed.StartStatus && parsed.StartTray && BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed),
    "The start menu's status buttons must turn off on their own and survive a round trip.");
Require(!new BarSettings().SameAs(parsed), "The start menu's status buttons must participate in the Save/Revert dirty state.");
Require(!BarSettings.FromJson([], new()).StageMinimize, "Old files must keep minimized windows out of Stage Manager.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"genieMinimize":true}""")!.AsObject(), new());
Require(!new BarSettings().GenieMinimize && parsed.GenieMinimize &&
        BarSettings.FromJson(parsed.ToJson(), new()).GenieMinimize && !new BarSettings().SameAs(parsed),
    "The genie effect must be off by default, survive a round trip and mark the settings dirty.");
var visibleOff = BarSettings.FromJson(JsonNode.Parse("""{"barReserve":false,"barFullscreenHide":false}""")!.AsObject(), new());
Require(new BarSettings().BarReserve && new BarSettings().BarFullscreenHide && !visibleOff.BarReserve && !visibleOff.BarFullscreenHide &&
        BarSettings.FromJson(visibleOff.ToJson(), new()) is { BarReserve: false, BarFullscreenHide: false } && !new BarSettings().SameAs(visibleOff),
    "The always-shown bar keeps room and hides for full-screen apps by default; both off survive a round trip and mark the settings dirty.");
var dodgeOn = BarSettings.FromJson(JsonNode.Parse("""{"barDodge":true}""")!.AsObject(), new());
Require(!new BarSettings().BarDodge && dodgeOn.BarDodge &&
        BarSettings.FromJson(dodgeOn.ToJson(), new()).BarDodge && !new BarSettings().SameAs(dodgeOn) &&
        BarSettings.FromJson(JsonNode.Parse("""{"autoHide":"dodge"}""")!.AsObject(), new()) is { AutoHide: "fullscreen", BarDodge: true } &&
        BarSettings.FromJson(JsonNode.Parse("""{"autoHide":"__invalid__"}""")!.AsObject(), new()) is { AutoHide: "fullscreen", BarDodge: false },
    "BarDodge defaults off, survives a round trip, marks settings dirty, and migrates legacy autoHide dodge.");
var peekOff = BarSettings.FromJson(JsonNode.Parse("""{"windowPeek":false}""")!.AsObject(), new());
Require(new BarSettings().WindowPeek && !peekOff.WindowPeek && !BarSettings.FromJson(peekOff.ToJson(), new()).WindowPeek && !new BarSettings().SameAs(peekOff),
    "Peeking at windows is on by default, and off survives a round trip and marks the settings dirty.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"stageMinimize":true}""")!.AsObject(), new());
Require(parsed.StageMinimize && BarSettings.FromJson(parsed.ToJson(), new()).StageMinimize && !new BarSettings().SameAs(parsed),
    "Minimizing to Stage Manager must survive a round trip and mark the settings dirty.");
Require(new BarSettings().StageDragMerge && BarSettings.FromJson([], new()).StageDragMerge,
    "Dragging onto a Stage Manager group is on by default, also for old files.");
var avoidOff = BarSettings.FromJson(JsonNode.Parse("""{"stageAvoid":false}""")!.AsObject(), new());
Require(new BarSettings().StageAvoid && !avoidOff.StageAvoid &&
        !BarSettings.FromJson(avoidOff.ToJson(), new()).StageAvoid && !new BarSettings().SameAs(avoidOff),
    "Windows avoiding the Stage Manager strip is on by default, and off survives a round trip.");
var closeOff = BarSettings.FromJson(JsonNode.Parse("""{"stageCloseButton":false}""")!.AsObject(), new());
Require(new BarSettings().StageCloseButton && !closeOff.StageCloseButton && !BarSettings.FromJson(closeOff.ToJson(), new()).StageCloseButton &&
        !new BarSettings().SameAs(closeOff),
    "The Stage Manager cards' close button is on by default, and off survives a round trip.");
var scrollWrapOff = BarSettings.FromJson(JsonNode.Parse("""{"stageScrollWrap":false}""")!.AsObject(), new());
Require(new BarSettings().StageScrollWrap && BarSettings.FromJson([], new()).StageScrollWrap &&
        !scrollWrapOff.StageScrollWrap && !BarSettings.FromJson(scrollWrapOff.ToJson(), new()).StageScrollWrap &&
        !BarSettings.FromJson([], scrollWrapOff).StageScrollWrap &&
        !BarSettings.FromJson(JsonNode.Parse("""{"stageScrollWrap":"bad"}""")!.AsObject(), scrollWrapOff).StageScrollWrap &&
        !new BarSettings().SameAs(scrollWrapOff),
    "Stage Manager strip scroll wrap is on by default, and off survives round trips, partial updates, and malformed values.");
var dragMergeOff = BarSettings.FromJson(JsonNode.Parse("""{"stageDragMerge":false}""")!.AsObject(), new());
Require(!dragMergeOff.StageDragMerge && !BarSettings.FromJson(dragMergeOff.ToJson(), new()).StageDragMerge &&
        !BarSettings.FromJson([], dragMergeOff).StageDragMerge &&
        !BarSettings.FromJson(JsonNode.Parse("""{"stageDragMerge":"bad"}""")!.AsObject(), dragMergeOff).StageDragMerge &&
        !new BarSettings().SameAs(dragMergeOff),
    "Turning Stage Manager drag merge off must survive round trips, partial updates, malformed values and mark settings dirty.");
var excludedDefault = new BarSettings();
Require(excludedDefault.StageExcluded.SequenceEqual(BarSettings.StageExcludedDefaults) &&
        excludedDefault.StageExcluded.Contains("snippingtool.exe") && excludedDefault.StageExcluded.Contains("microsoft.screensketch_") &&
        !excludedDefault.StageExcluded.Contains("wechat.exe") && !excludedDefault.StageExcluded.Contains("notepad.exe"),
    "The Stage Manager blacklist must start with the Windows screenshot tools.");
var excluded = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":["wechat.exe","tencent.qq_"]}""")!.AsObject(), new());
Require(excluded.StageExcluded.SequenceEqual(["wechat.exe", "tencent.qq_"]) &&
        BarSettings.FromJson(excluded.ToJson(), new()).SameAs(excluded) && !new BarSettings().SameAs(excluded),
    "The Stage Manager blacklist must survive a round trip and mark the settings dirty.");
var excludedEmpty = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":[]}""")!.AsObject(), new());
Require(excludedEmpty.StageExcluded.Count == 0 && BarSettings.FromJson(excludedEmpty.ToJson(), new()).SameAs(excludedEmpty),
    "An empty Stage Manager blacklist must stay empty and survive a round trip.");
var excludedMalformed = BarSettings.FromJson(JsonNode.Parse("""{"stageExcluded":false}""")!.AsObject(), excluded);
Require(excludedMalformed.StageExcluded.SequenceEqual(excluded.StageExcluded),
    "A malformed Stage Manager blacklist must retain the current list.");
Require(!BarSettings.FromJson([], new()).ShowClockSeconds, "Old files must keep the clock at minutes.");
Require(!new BarSettings().MagnifyNear && !BarSettings.FromJson([], new()).MagnifyNear,
    "Icons must not magnify above the bar unless chosen.");
Require(!new BarSettings().ShowDesktop && BarSettings.FromJson(JsonNode.Parse("""{"showDesktop":true}""")!.AsObject(), new()).ShowDesktop &&
    !new BarSettings().SameAs(BarSettings.FromJson(JsonNode.Parse("""{"showDesktop":true}""")!.AsObject(), new())),
    "The Show Desktop slot is off by default and turning it on is saved.");
Require(new BarSettings().TrayHost && BarSettings.FromJson([], new()).TrayHost, "Atrium hosts the tray icons unless turned off.");
{
    var old = BarSettings.FromJson([], new());
    var chosen = BarSettings.FromJson(
        JsonNode.Parse("""{"attentionLight":false,"attentionBounce":true,"barBadges":false,"startCentered":true,"startWithBar":true}""")!.AsObject(), new());
    Require(old.AttentionLight && !old.AttentionBounce && old.BarBadges && !old.StartCentered && !old.StartWithBar,
        "Old files light apps asking for attention, without hops, with unread badges, the start menu at the left, alone.");
    Require(!chosen.AttentionLight && chosen.AttentionBounce && !chosen.BarBadges && chosen.StartCentered && chosen.StartWithBar && !chosen.SameAs(old) &&
            BarSettings.FromJson(JsonNode.Parse(chosen.ToJson().ToJsonString())!.AsObject(), new()).SameAs(chosen),
        "The attention, badge and start menu choices are read, compared and saved.");
}
parsed = BarSettings.FromJson(JsonNode.Parse("""{"trayHost":false}""")!.AsObject(), new());
Require(!parsed.TrayHost && !BarSettings.FromJson(parsed.ToJson(), new()).TrayHost && !new BarSettings().SameAs(parsed),
    "Turning the tray host off must survive a round trip and mark the settings dirty.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"remoteVisible":true}""")!.AsObject(), new());
Require(parsed.RemoteVisible && BarSettings.FromJson(parsed.ToJson(), new()).RemoteVisible && !new BarSettings().SameAs(parsed),
    "Remote-control mode must survive a round trip and mark the settings dirty.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"showClockSeconds":true}""")!.AsObject(), new());
Require(parsed.ShowClockSeconds && parsed.ShowClock &&
        BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed) && !new BarSettings().SameAs(parsed),
    "Clock seconds must survive a round trip, mark the settings dirty and leave the clock on.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"showClockSeconds":"bad"}""")!.AsObject(),
        new() { ShowClockSeconds = true }).ShowClockSeconds,
    "Malformed clock seconds must retain the current value.");
Require(!BarSettings.FromJson([], new()).TaskbarRightPrimaryOnly, "Old files must keep right-side elements on every display.");
Require(BarSettings.FromJson([], settings).TaskbarRightPrimaryOnly, "Partial updates must retain primary-only visibility.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"taskbarRightPrimaryOnly":"bad"}""")!.AsObject(), settings).TaskbarRightPrimaryOnly,
    "Malformed primary-only visibility must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"taskbarRightPrimaryOnly":false}""")!.AsObject(), settings);
Require(!parsed.TaskbarRightPrimaryOnly && parsed.ShowTray && parsed.ShowStatus && parsed.ShowClock && parsed.ShowNotifications,
    "Disabling primary-only visibility must retain the individual content switches.");
Require(BarSettings.FromJson([], new()).NotifyPlacement == "dock", "Existing users must retain the default notification placement.");
Require(BarSettings.FromJson([], new()).Language == "auto", "Old files must follow the system language.");
settings.Language = "en";
Require(BarSettings.FromJson(settings.ToJson(), new()).Language == "en", "The interface language must survive a round trip.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"language":"bad tag"}""")!.AsObject(), settings).Language == "auto",
    "Invalid languages must follow the system.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"language":false}""")!.AsObject(), settings).Language == "en",
    "Malformed languages must retain the current value.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"notifyPlacement":"bad"}""")!.AsObject(), new()).NotifyPlacement == "dock",
    "Invalid notification placement must normalize.");
Require(BarSettings.FromJson(JsonNode.Parse("""{"notifyPlacement":false}""")!.AsObject(), settings).NotifyPlacement == "topRight",
    "Malformed notification placement must retain the current value.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayMode":"bad","taskbarApps":"bad","dockDisplayDelay":99999}""")!.AsObject(), new());
Require(parsed.DockDisplayMode == "follow" && parsed.TaskbarApps == "all" && parsed.DockDisplayDelay == 2000,
    "Invalid values must normalize like the native core.");
Require(new BarSettings().DockDisplayMode == "follow" && BarSettings.FromJson([], new()).DockDisplayMode == "follow",
    "DockDisplayMode must default to follow.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayDelay":-1}""")!.AsObject(), new());
Require(parsed.DockDisplayDelay == 100, "Delay must have a lower bound.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"dockDisplayDelay":"bad","desktopDisplay":false}""")!.AsObject(), settings);
Require(parsed.DockDisplayDelay == settings.DockDisplayDelay && parsed.DesktopDisplay == settings.DesktopDisplay,
    "Malformed values must retain the current settings.");
var original = settings.Clone();
settings.ApplyMaterial(BarSettings.Frosted());
Require(settings.DockDisplayMode == original.DockDisplayMode && settings.DockDisplay == original.DockDisplay &&
    settings.DesktopDisplay == original.DesktopDisplay && settings.DockDisplayDelay == original.DockDisplayDelay &&
    settings.TaskbarApps == original.TaskbarApps && settings.TaskbarRightPrimaryOnly == original.TaskbarRightPrimaryOnly &&
    settings.NotifyPlacement == original.NotifyPlacement && settings.NotifyFullscreen == original.NotifyFullscreen &&
    settings.HoverRise == original.HoverRise &&
    settings.WindowsContextMenu == original.WindowsContextMenu,
    "Material presets must preserve monitor, notification and hover-rise choices.");
foreach (var edit in new Action<BarSettings>[]
{
    s => s.DockDisplayMode = "fixed", s => s.DockDisplayDelay += 50, s => s.DockDisplay += "other",
    s => s.DesktopDisplay += "other", s => s.TaskbarApps = "all", s => s.NotifyPlacement = "dock",
    s => s.NotifyFullscreen = true, s => s.NotifyAlways = true, s => s.NotifyTimeout = 15, s => s.TaskbarRightPrimaryOnly = false,
    s => s.HoverRise = true, s => s.NearEnter += 2, s => s.NearLeave += 2,
})
{
    var changed = settings.Clone();
    edit(changed);
    Require(!settings.SameAs(changed), "Every display preference must participate in the Save/Revert dirty state.");
}
var autoUpdateOff = BarSettings.FromJson(JsonNode.Parse("""{"autoUpdate":false}""")!.AsObject(), new());
Require(new BarSettings().AutoUpdate && BarSettings.FromJson([], new()).AutoUpdate &&
        !autoUpdateOff.AutoUpdate && !BarSettings.FromJson(autoUpdateOff.ToJson(), new()).AutoUpdate &&
        !BarSettings.FromJson([], autoUpdateOff).AutoUpdate &&
        !BarSettings.FromJson(JsonNode.Parse("""{"autoUpdate":"bad"}""")!.AsObject(), autoUpdateOff).AutoUpdate &&
        !new BarSettings().SameAs(autoUpdateOff),
    "AutoUpdate must default true and survive round trips, partial updates, malformed values, and dirty tracking.");
var startupTask = BarSettings.FromJson(JsonNode.Parse("""{"startupMode":"task"}""")!.AsObject(), new());
Require(new BarSettings().StartupMode == "run" && BarSettings.FromJson([], new()).StartupMode == "run" &&
        startupTask.StartupMode == "task" && BarSettings.FromJson(startupTask.ToJson(), new()).StartupMode == "task" &&
        BarSettings.FromJson([], startupTask).StartupMode == "task" &&
        BarSettings.FromJson(JsonNode.Parse("""{"startupMode":123}""")!.AsObject(), startupTask).StartupMode == "task" &&
        BarSettings.FromJson(JsonNode.Parse("""{"startupMode":"bad"}""")!.AsObject(), startupTask).StartupMode == "run" &&
        !new BarSettings().SameAs(startupTask),
    "StartupMode must default to run, survive round trips and malformed values, clamp unknown strings to run, and mark settings dirty.");
var menuOff = BarSettings.FromJson(JsonNode.Parse("""{"windowsContextMenu":false}""")!.AsObject(), new());
Require(!menuOff.WindowsContextMenu && BarSettings.FromJson(menuOff.ToJson(), new()).SameAs(menuOff),
    "The Windows-style context menu must turn off and survive a round trip.");
Require(new BarSettings().WindowsContextMenu && BarSettings.FromJson([], new()).WindowsContextMenu,
    "The Windows-style context menu is on by default, also for files written before it existed.");
Require(!new BarSettings().SameAs(menuOff), "The Windows-style context menu must participate in the Save/Revert dirty state.");
Require(!new BarSettings().NotifyFullscreen && !BarSettings.FromJson([], new()).NotifyFullscreen,
    "Fullscreen notification popups are off by default.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"notifyFullscreen":true}""")!.AsObject(), new());
Require(parsed.NotifyFullscreen && BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed) &&
        BarSettings.FromJson([], parsed).NotifyFullscreen &&
        BarSettings.FromJson(JsonNode.Parse("""{"notifyFullscreen":"bad"}""")!.AsObject(), parsed).NotifyFullscreen,
    "Fullscreen notification popups survive round-trip, partial update and malformed input.");
Require(!new BarSettings().NotifyAlways && !BarSettings.FromJson([], new()).NotifyAlways,
    "Always-visible notification popups are off by default.");
parsed = BarSettings.FromJson(JsonNode.Parse("""{"notifyAlways":true,"notifyTimeout":15}""")!.AsObject(), new());
Require(parsed.NotifyAlways && parsed.NotifyTimeout == 15 && BarSettings.FromJson(parsed.ToJson(), new()).SameAs(parsed) &&
        BarSettings.FromJson([], parsed).NotifyAlways &&
        BarSettings.FromJson(JsonNode.Parse("""{"notifyAlways":"bad"}""")!.AsObject(), parsed).NotifyAlways,
    "NotifyAlways and NotifyTimeout survive round-trip, partial update and malformed input.");
var displays = DesktopDisplay.Read();
Require(displays.Count > 0 && displays.Any(d => d.Primary), "Connected displays and system primary must be discoverable.");
Require(displays.All(d => d.Id.Length > 0 && d.Scale > 0 && d.Width > 0 && d.WorkWidth > 0), "Display IDs, geometry and scale must be valid.");
var shortcutPin = new PinnedApp("Browser", @"C:\Links\Browser.lnk", "", "");
Require(!shortcutPin.AllowsMultipleWindows() && shortcutPin.AllowsMultipleWindows(@"C:\Program Files\chrome.exe"),
    "PinnedApp multiple-window default must use the resolved target path without shell I/O.");
Require(!(shortcutPin with { MultipleWindows = false }).AllowsMultipleWindows(@"C:\Program Files\chrome.exe"),
    "An explicit launch-mode override must take precedence over the resolved target path.");
Require(new PinnedApp("Edge", @"C:\Program Files\msedge.exe", "", "").AllowsMultipleWindows() &&
        !new PinnedApp("Notepad", @"C:\Windows\notepad.exe", "", "").AllowsMultipleWindows(),
    "An .exe pin with no resolved path must fall back to its own executable name.");
Require(new PinnedApp("Explorer", "", "", "Microsoft.Windows.Explorer").AllowsMultipleWindows(),
    "The Explorer AUMID must default to allowing multiple windows.");
{
    var modulesDefault = new BarSettings();
    Require(modulesDefault.ControlModules.SequenceEqual(["stage"]), "Control centre modules must default to Stage Manager.");
    var modules = BarSettings.FromJson(
        JsonNode.Parse("""{"controlModules":[" darkmode ","wifi","stage","dnd","darkmode","","airplane","hotspot"]}""")!.AsObject(),
        new());
    Require(modules.ControlModules.SequenceEqual(["darkmode", "stage", "dnd", "airplane", "hotspot"]) &&
            BarSettings.FromJson(modules.ToJson(), new()).SameAs(modules) && !modules.SameAs(modulesDefault),
        "Control centre modules must keep known ids once, in order, and survive a round trip.");
    var nightLight = BarSettings.FromJson(JsonNode.Parse("""{"controlModules":["nightlight","stage","darkmode"]}""")!.AsObject(), new());
    Require(nightLight.ControlModules.SequenceEqual(["darkmode", "stage"]),
        "The night light module must become dark mode.");
    var stackFiles = BarSettings.FromJson(
        JsonNode.Parse("""{"folderStackFiles":[" C:\\Docs\\Plan.pdf ","c:/docs/plan.pdf","","D:\\Notes.txt"]}""")!.AsObject(), new());
    Require(stackFiles.FolderStackFiles.SequenceEqual([@"C:\Docs\Plan.pdf", @"D:\Notes.txt"]),
        "Folder stack files must deduplicate paths case-insensitively.");
    Require(BarSettings.FromJson(JsonNode.Parse("""{"folderStackIcon":"bad"}""")!.AsObject(), new()).FolderStackIcon == "mono",
        "An unknown stack icon must fall back to the bar's ink outline.");
    var rim = BarSettings.FromJson(JsonNode.Parse("""{"edgeSoftness":9,"edgeLightBoost":-1}""")!.AsObject(), new());
    Require(rim.EdgeSoftness == 3.0 && rim.EdgeLightBoost == 0.0 &&
            new BarSettings().EdgeSoftness == 2.0 && new BarSettings().EdgeLightBoost == 0.8,
        "The rim's softness must clamp to 0.8..3 and the light boost to 0..1.5.");
    var extrasDefault = new BarSettings();
    Require(extrasDefault.ShowLaunchpad && extrasDefault.LaunchpadMaterial == "bar" &&
            extrasDefault.LaunchpadLayout == "window" && extrasDefault.ShowNowPlaying && extrasDefault.NowPlayingShowPaused &&
            extrasDefault.NowPlayingMotion == "normal" && extrasDefault.NowPlayingRhythm == "live" && extrasDefault.NowPlayingAppMode == "all" &&
            extrasDefault.NowPlayingApps.Count == 0 &&
            !extrasDefault.ShowFolderStacks && extrasDefault.FolderStackLayout == "fan" && extrasDefault.FolderStackIcon == "mono" &&
            !extrasDefault.FolderStackPinned &&
            extrasDefault.FolderStackMax == 8 && extrasDefault.FolderStacks.Count == 0 && extrasDefault.FolderStackFiles.Count == 0,
        "Extras settings must have expected defaults.");
    var extrasCustom = new BarSettings
    {
        ShowLaunchpad = false,
        LaunchpadMaterial = "tint",
        LaunchpadLayout = "fullscreen",
        ShowNowPlaying = false,
        NowPlayingShowPaused = false,
        NowPlayingMotion = "vivid",
        NowPlayingRhythm = "loop",
        NowPlayingAppMode = "block",
        NowPlayingApps = ["spotify.exe"],
        ShowFolderStacks = true,
        FolderStackLayout = "column",
        FolderStackIcon = "color",
        FolderStackPinned = true,
        FolderStackMax = 24,
        FolderStacks = [@"C:\Users\Demo\Desktop"],
        FolderStackFiles = [@"C:\Users\Demo\Notes.pdf"],
    };
    Require(BarSettings.FromJson(extrasCustom.ToJson(), new()).SameAs(extrasCustom),
        "Extras settings must survive a round trip.");
    foreach (var edit in new Action<BarSettings>[]
    {
        s => s.ShowLaunchpad = false, s => s.LaunchpadMaterial = "tint", s => s.LaunchpadLayout = "fullscreen",
        s => s.ShowNowPlaying = false, s => s.NowPlayingShowPaused = false,
        s => s.NowPlayingMotion = "subtle", s => s.NowPlayingRhythm = "loop", s => s.NowPlayingAppMode = "allow",
        s => s.NowPlayingApps = ["a.exe"],
        s => s.ShowFolderStacks = true, s => s.FolderStackLayout = "column",
        s => s.FolderStackIcon = "color",
        s => s.FolderStackPinned = true,
        s => s.FolderStackMax = 10, s => s.FolderStacks = [@"D:\Work"], s => s.FolderStackFiles = [@"D:\Notes.pdf"],
    })
    {
        var changed = extrasDefault.Clone();
        edit(changed);
        Require(!extrasDefault.SameAs(changed), "Every extras setting must participate in the Save/Revert dirty state.");
    }
}
Console.WriteLine($"Settings round trips, validation, dirty tracking and display discovery passed ({displays.Count} connected display(s)).");

string? Arg(string flag) => Array.IndexOf(args, flag) is int idx && idx >= 0 && idx + 1 < args.Length ? args[idx + 1] : null;
string? coreNormalizer = Arg("--core-normalizer"), repoRoot = Arg("--repo-root");
for (string? d = AppContext.BaseDirectory; repoRoot == null && d != null; d = Directory.GetParent(d)?.FullName)
{
    if (File.Exists(Path.Combine(d, "docs", "SETTINGS-PROTOCOL.md")) &&
        File.Exists(Path.Combine(d, "tests", "fixtures", "settings-schema.json")))
    { repoRoot = d; }
}
Require(repoRoot != null, "Could not locate repository root.");
string buildRoot = Path.Combine(repoRoot!, "native", "build");
string normalizerPath = coreNormalizer != null
    ? (File.Exists(coreNormalizer) ? coreNormalizer : throw new FileNotFoundException($"Core normalizer not found: {coreNormalizer}"))
    : BuildConfigs.SelectMany(c => new[]
        { Path.Combine(buildRoot, c, "tests", "display_settings_tests.exe"), Path.Combine(buildRoot, c, "tests", "Release", "display_settings_tests.exe") })
        .Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault()
        ?? throw new FileNotFoundException($"display_settings_tests.exe not found under {buildRoot}.");

var schemaKeys = JsonNode.Parse(File.ReadAllText(Path.Combine(repoRoot!, "tests", "fixtures", "settings-schema.json")))!["keys"]!.AsObject();
string protocolText = File.ReadAllText(Path.Combine(repoRoot!, "docs", "SETTINGS-PROTOCOL.md"));
string protocolTable = protocolText[..protocolText.IndexOf("## 管道", StringComparison.Ordinal)];
var docKeys = DocKeyRegex().Matches(protocolTable).Select(m => m.Groups[1].Value).ToHashSet(StringComparer.Ordinal);
var schemaKeySet = schemaKeys.Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);
var csKeys = new BarSettings { StartSize = "medium", StartIcon = "medium", Pins = [] }.ToJson().Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);

var altBase = new JsonObject();
foreach (var (k, node) in schemaKeys)
{
    var s = node!.AsObject();
    altBase[k] = s["type"]!.GetValue<string>() switch
    {
        "number" => Math.Abs(s["default"]!.GetValue<double>() - s["min"]!.GetValue<double>()) > 1e-6
            ? s["min"]!.GetValue<double>() : s["max"]!.GetValue<double>(),
        "bool" => !s["default"]!.GetValue<bool>(),
        "enum" => k == "startSize" ? "large" : k == "startIcon" ? "small"
            : s["values"]!.AsArray().Select(v => v!.GetValue<string>()).First(v => v != s["default"]!.GetValue<string>()),
        "string" => @"\\?\DISPLAY#alt",
        "tint" => "#123456",
        "language" => "en",
        "stringArray" => new JsonArray("a.exe"),
        "pins" => new JsonArray(new JsonObject { ["name"] = "App", ["path"] = @"C:\app.exe", ["args"] = "--x", ["aumid"] = "", ["multipleWindows"] = true }),
        _ => throw new InvalidOperationException($"Unknown schema type for {k}")
    };
}
var altBaseSettings = BarSettings.FromJson(altBase, new());

var cases = new List<(string Label, JsonObject? BaseObj, BarSettings BaseSettings, JsonObject? PatchObj, string? RawPatch)>
{
    ("keys:populated", null, new(), JsonNode.Parse("""{"startSize":"medium","startIcon":"medium","pins":[]}""")!.AsObject(), null),
    ("empty:default", null, new(), new(), null), ("empty:alt", altBase, altBaseSettings, new(), null),
    ("v1:migration", null, new(), JsonNode.Parse("""{"version":1,"notifyListener":false}""")!.AsObject(), null),
    ("v2:no-migration", null, new(), JsonNode.Parse("""{"version":2,"notifyListener":false}""")!.AsObject(), null),
    ("malformed:json", null, new(), null, "{broken"),
};
foreach (var (k, node) in schemaKeys)
{
    var s = node!.AsObject();
    void Add(string sub, JsonNode? val) => cases.Add(($"{k}:{sub}", altBase, altBaseSettings, new JsonObject { [k] = val }, null));
    switch (s["type"]!.GetValue<string>())
    {
        case "number":
            double lo = s["min"]!.GetValue<double>(), hi = s["max"]!.GetValue<double>();
            Add("min", lo); Add("max", hi); Add("under", lo - 100); Add("over", hi + 100);
            Add("str", "bad"); Add("bool", false); Add("null", null); Add("arr", new JsonArray()); break;
        case "bool": Add("true", true); Add("false", false); Add("str", "true"); Add("num", 1); Add("null", null); break;
        case "enum":
            foreach (var v in s["values"]!.AsArray()) Add($"val:{v!.GetValue<string>()}", v.GetValue<string>());
            Add("unknown", "__unknown__"); Add("empty", ""); Add("bool", false); Add("num", 123); Add("null", null); break;
        case "string": Add("empty", ""); Add("display", @"\\?\DISPLAY#1"); Add("bool", false); break;
        case "tint": foreach (string t in TintSamples) Add($"val:{t}", t); Add("bool", false); break;
        case "language": foreach (string l in LangSamples) Add($"val:{l}", l); Add("bool", false); break;
        case "stringArray":
            Add("empty", new JsonArray()); Add("ws", new JsonArray(" wechat.exe ", "  ", "foo_"));
            Add("bool", false); Add("mixed", new JsonArray(123, "ok.exe"));
            if (s["maxCount"] is JsonValue mc && mc.TryGetValue(out int maxItems))
            {
                var over = new JsonArray("   ");
                for (int idx = 0; idx < maxItems + 3; idx++) over.Add($" item{idx} ");
                Add("overMax", over);
            }
            break;
        case "pins":
            Add("empty", new JsonArray());
            var over65 = new JsonArray();
            for (int i = 0; i < 65; i++) { over65.Add(new JsonObject { ["name"] = $"P{i}", ["path"] = $@"C:\p{i}.exe" }); }
            Add("65", over65);
            Add("drop-empty", new JsonArray(new JsonObject { ["name"] = "D", ["path"] = "" }, new JsonObject { ["name"] = "K", ["path"] = @"C:\k.exe" }));
            Add("multi", new JsonArray(new JsonObject { ["name"] = "A", ["path"] = @"C:\a.exe", ["multipleWindows"] = true },
                new JsonObject { ["name"] = "B", ["path"] = @"C:\b.exe", ["multipleWindows"] = false },
                new JsonObject { ["name"] = "C", ["path"] = @"C:\c.exe" }));
            Add("bad", "bad"); break;
    }
}

var psi = new ProcessStartInfo(normalizerPath, "--normalize");
psi.RedirectStandardInput = psi.RedirectStandardOutput = true;
psi.StandardInputEncoding = psi.StandardOutputEncoding = new UTF8Encoding(false);
using var proc = Process.Start(psi)!;
var readTask = Task.Run(() =>
{
    var l = new List<string>();
    while (proc.StandardOutput.ReadLine() is string s) { if (!string.IsNullOrWhiteSpace(s)) l.Add(s); }
    return l;
});
foreach (var (_, baseObj, _, patchObj, rawPatch) in cases)
{
    var req = new JsonObject();
    if (baseObj != null) req["base"] = baseObj.DeepClone();
    if (rawPatch != null) req["patchText"] = rawPatch; else req["patch"] = patchObj!.DeepClone();
    proc.StandardInput.WriteLine(req.ToJsonString());
}
proc.StandardInput.Close();
var timeout = TimeSpan.FromSeconds(15);
List<string> respLines;
try
{
    respLines = await readTask.WaitAsync(timeout);
    await proc.WaitForExitAsync().WaitAsync(timeout);
}
catch (TimeoutException ex)
{
    try { proc.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
    throw new TimeoutException($"display_settings_tests --normalize timed out after {timeout.TotalSeconds:0}s.", ex);
}
Require(proc.ExitCode == 0, $"Core normalizer exited with code {proc.ExitCode}.");
Require(respLines.Count == cases.Count, $"Expected {cases.Count} normalizer responses, got {respLines.Count}.");

static bool JsonEqual(JsonNode? a, JsonNode? b)
{
    if (a is null || b is null) return a is null && b is null;
    if (a is JsonObject ao && b is JsonObject bo)
        return ao.Count == bo.Count && ao.All(kv => bo.TryGetPropertyValue(kv.Key, out var bv) && JsonEqual(kv.Value, bv));
    if (a is JsonArray aa && b is JsonArray ba)
        return aa.Count == ba.Count && aa.Zip(ba, JsonEqual).All(x => x);
    if (a is JsonValue av && b is JsonValue bv)
    {
        if (av.TryGetValue(out bool ab) && bv.TryGetValue(out bool bb)) return ab == bb;
        if (av.TryGetValue(out string? astr) && bv.TryGetValue(out string? bstr)) return astr == bstr;
        if (av.GetValueKind() == JsonValueKind.Number && bv.GetValueKind() == JsonValueKind.Number)
        {
            double ad = double.Parse(av.ToJsonString(), CultureInfo.InvariantCulture);
            double bd = double.Parse(bv.ToJsonString(), CultureInfo.InvariantCulture);
            return Math.Abs(ad - bd) <= 1e-9;
        }
    }
    return false;
}

static void AssertSchemaCompliance(string label, JsonObject cs, JsonObject schemaKeys)
{
    foreach (var (k, node) in schemaKeys)
    {
        var s = node!.AsObject();
        string type = s["type"]!.GetValue<string>();
        bool hasKey = cs.TryGetPropertyValue(k, out var val);
        if (label == "empty:default")
        {
            bool canOmit = s["omitWhenEmpty"]?.GetValue<bool>() == true || s["optional"]?.GetValue<bool>() == true;
            Require((hasKey || canOmit) && (!hasKey || JsonEqual(val, s["default"])),
                $"Default mismatch [{label}] for {k}: got {val?.ToJsonString() ?? "<omitted>"}");
        }
        bool ok = type switch
        {
            "number" => hasKey && val is JsonValue nv && nv.GetValueKind() == JsonValueKind.Number &&
                double.Parse(nv.ToJsonString(), CultureInfo.InvariantCulture) is double d &&
                d >= s["min"]!.GetValue<double>() && d <= s["max"]!.GetValue<double>(),
            "enum" => (!hasKey || val is null)
                ? s["omitWhenEmpty"]?.GetValue<bool>() == true
                : val is JsonValue ev && ev.TryGetValue(out string? es) &&
                    ((s["omitWhenEmpty"]?.GetValue<bool>() == true && es == "") ||
                     s["values"]!.AsArray().Any(v => v!.GetValue<string>() == es)),
            "tint" => hasKey && val is JsonValue tv && tv.TryGetValue(out string? ts) && TintRegex().IsMatch(ts),
            "bool" => hasKey && val is JsonValue bv && bv.TryGetValue(out bool _),
            "string" or "language" => hasKey && val is JsonValue sv && sv.TryGetValue(out string? _),
            "stringArray" => hasKey && val is JsonArray sa && sa.All(e => e is JsonValue jv && jv.TryGetValue(out string? _)) &&
                (s["maxCount"] is null || sa.Count <= s["maxCount"]!.GetValue<int>()),
            "pins" => (!hasKey || val is null)
                ? s["optional"]?.GetValue<bool>() == true
                : val is JsonArray pa && pa.Count <= (s["maxCount"]?.GetValue<int>() ?? 64),
            _ => throw new InvalidOperationException($"Unknown schema type {type} for {k}")
        };
        Require(ok, $"Schema violation [{label}] for {k} ({type}): {val?.ToJsonString() ?? "<omitted>"}");
    }
}

for (int i = 0; i < cases.Count; i++)
{
    var (label, _, baseSettings, patchObj, rawPatch) = cases[i];
    var resp = JsonNode.Parse(respLines[i])!.AsObject();
    if (rawPatch != null) { Require(!resp["ok"]!.GetValue<bool>(), $"Malformed JSON [{label}] must return ok=false."); continue; }
    Require(resp["ok"]!.GetValue<bool>(), $"Case [{label}] failed in C++ normalizer.");
    var cpp = resp["result"]!.AsObject();
    if (label == "keys:populated")
    {
        var cppKeys = cpp.Select(kv => kv.Key).ToHashSet(StringComparer.Ordinal);
        void CheckSet(string name, HashSet<string> actual)
        {
            if (actual.SetEquals(schemaKeySet)) return;
            throw new InvalidOperationException($"4-way key mismatch ({name} vs schema): " +
                $"missing=[{string.Join(",", schemaKeySet.Except(actual))}], extra=[{string.Join(",", actual.Except(schemaKeySet))}]");
        }
        CheckSet("SETTINGS-PROTOCOL.md", docKeys); CheckSet("BarSettings.ToJson", csKeys); CheckSet("Settings.ToJson", cppKeys);
    }
    var cs = BarSettings.FromJson(patchObj, baseSettings).ToJson();
    if (!JsonEqual(cs, cpp))
        throw new InvalidOperationException($"Differential mismatch [{label}]\nC#:  {cs.ToJsonString()}\nC++: {cpp.ToJsonString()}");
    AssertSchemaCompliance(label, cs, schemaKeys);
    if (label == "pins:65") Require(cs["pins"]!.AsArray().Count == 64, "Pins must cap at 64.");
    if (label == "v1:migration") Require(cs["notifyListener"]!.GetValue<bool>(), "v1 migration must keep notifyListener=true.");
    if (label == "v2:no-migration") Require(!cs["notifyListener"]!.GetValue<bool>(), "v2 must apply notifyListener=false.");
}

string xamlCs = File.ReadAllText(Path.Combine(repoRoot!, "settings", "Atrium.Settings", "MainWindow.xaml.cs"));
var sliderMatches = SliderRegex().Matches(xamlCs);
var schemaSliders = schemaKeys.Where(kv => kv.Value!["sliderProp"] != null)
    .ToDictionary(kv => kv.Value!["sliderProp"]!.GetValue<string>(), kv => (kv.Key, Spec: kv.Value!.AsObject()));
Require(sliderMatches.Count == schemaSliders.Count, $"Expected {schemaSliders.Count} sliders in MainWindow.xaml.cs, found {sliderMatches.Count}.");
foreach (Match m in sliderMatches)
{
    double uiMin = double.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
    double uiMax = double.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
    string prop = m.Groups[3].Value;
    Require(schemaSliders.TryGetValue(prop, out var entry), $"Slider property {prop} missing from schema.");
    var sp = entry.Spec;
    double sMin = sp["uiMin"]!.GetValue<double>(), sMax = sp["uiMax"]!.GetValue<double>();
    double scale = sp["uiScale"]?.GetValue<double>() ?? 1, mMin = sp["min"]!.GetValue<double>(), mMax = sp["max"]!.GetValue<double>();
    Require(uiMin == sMin && uiMax == sMax, $"Slider {prop} ({entry.Key}) UI [{uiMin},{uiMax}] != schema [{sMin},{sMax}].");
    Require(uiMin / scale >= mMin && uiMax / scale <= mMax, $"Slider {prop} ({entry.Key}) scaled exceeds model [{mMin},{mMax}].");
}
Console.WriteLine($"Settings contract passed: {schemaKeySet.Count} keys (4-way), " +
    $"{cases.Count} C++<->C# differential cases, {sliderMatches.Count} sliders aligned.");

static void ExpectThrows<TException>(Action action, string message) where TException : Exception
{
    try { action(); }
    catch (TException) { return; }
    throw new InvalidOperationException(message);
}

var libPaths = GameFinder.ParseLibraryPaths(""" "path" "D:\\SteamLibrary" "path" "E:\\Games\\Steam" """);
Require(libPaths.SequenceEqual([@"D:\SteamLibrary", @"E:\Games\Steam"]), "VDF library paths must unescape.");
string acf = """ "name" "Portal 2" "installdir" "Portal 2" """;
Require(GameFinder.ParseVdfField(acf, "name") == "Portal 2" && GameFinder.ParseVdfField(acf, "installdir") == "Portal 2",
    "ACF fields must parse.");
Require(GameFinder.IsExcludedRuntime("Steamworks Common Redistributables") &&
        GameFinder.IsExcludedRuntime("Foo Redistributable") && !GameFinder.IsExcludedRuntime("Portal 2"),
    "Steam runtime exclusion must filter runtimes only.");
var epicValid = GameFinder.ParseEpicManifest(
    """{"AppCategories":["games"],"DisplayName":"Hades","InstallLocation":"C:\\Games\\Hades","LaunchExecutable":"Hades.exe"}""");
Require(epicValid is { Name: "Hades", Folder: @"C:\Games\Hades", LaunchExe: "Hades.exe" }, "Valid Epic manifest must parse.");
Require(GameFinder.ParseEpicManifest("""{"AppCategories":["engines"],"DisplayName":"UE","InstallLocation":"C:\\UE"}""") is null,
    "Non-game Epic manifest must be ignored.");
Require(GameFinder.ParseEpicManifest("{bad") is null, "Malformed Epic manifest must return null.");
var merged = GameFinder.MergeGames([
    new("Portal 2", @"d:\steam\common\portal 2\", "a.exe"),
    new("Portal 2", @"D:\Steam\common\Portal 2\portal2.exe", "b.exe"),
    new("Notepad", @"c:\windows\notepad.exe", "n.exe"),
]);
Require(merged.Count == 2 && merged[0].Title == "Notepad" && merged[1].Title == "Portal 2",
    "MergeGames must deduplicate exe entries inside game folders.");

static JsonElement ReleaseJson(string tag, string? assetName = null, string? digest = null)
{
    var assets = new JsonArray();
    if (assetName != null)
    {
        var asset = new JsonObject { ["name"] = assetName, ["browser_download_url"] = "https://example.com/a.zip" };
        if (digest != null) asset["digest"] = digest;
        assets.Add(asset);
    }
    var obj = new JsonObject { ["tag_name"] = tag, ["html_url"] = "https://example.com/rel", ["assets"] = assets };
    return JsonDocument.Parse(obj.ToJsonString()).RootElement;
}
var relOk = ReleaseInfo.FromJson(ReleaseJson("v2.7.0", "Atrium-2.7.0-win-x64.zip", "sha256:abc"));
Require(relOk.Version == new Version(2, 7, 0, 0) && relOk.Download != null && relOk.Digest == "sha256:abc",
    "ReleaseInfo.FromJson must parse v2.7.0, matching zip asset, and digest.");
Require(ReleaseInfo.FromJson(ReleaseJson("v2.7.0+build")).Version == new Version(2, 7, 0, 0) &&
        ReleaseInfo.FromJson(ReleaseJson("v2.7.0", "other.zip")).Download == null,
    "ReleaseInfo.FromJson must handle +build tags and missing matching asset.");
ExpectThrows<JsonException>(() => ReleaseInfo.FromJson(ReleaseJson("bad")), "Invalid version must throw JsonException.");
ExpectThrows<KeyNotFoundException>(() => ReleaseInfo.FromJson(JsonDocument.Parse("{}").RootElement), "Missing tag_name must throw.");

string temp = Path.Combine(Path.GetTempPath(), "atrium-checks-" + Guid.NewGuid().ToString("N"));
try
{
    Directory.CreateDirectory(temp);
    string goodZip = Path.Combine(temp, "good.zip");
    using (var z = ZipFile.Open(goodZip, ZipArchiveMode.Create))
    {
        var e = z.CreateEntry("Atrium/ok.txt");
        using var w = new StreamWriter(e.Open());
        w.Write("ok");
    }
    string goodDigest = "sha256:" + Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(goodZip)));
    ReleaseValidation.VerifyZipDigest(goodZip, goodDigest);
    ExpectThrows<IOException>(() => ReleaseValidation.VerifyZipDigest(goodZip, "sha256:0000"), "Digest mismatch must throw.");
    string extractDir = Path.Combine(temp, "out");
    ReleaseValidation.ExtractZipSafely(goodZip, extractDir);
    Require(File.ReadAllText(Path.Combine(extractDir, "Atrium", "ok.txt")) == "ok", "ExtractZipSafely must extract valid zip.");
    string slipZip = Path.Combine(temp, "slip.zip");
    using (var z = ZipFile.Open(slipZip, ZipArchiveMode.Create))
        z.CreateEntry("../escape.txt");
    ExpectThrows<InvalidDataException>(() => ReleaseValidation.ExtractZipSafely(slipZip, extractDir), "ZipSlip must be rejected.");

    string pkg = Path.Combine(temp, "pkg");
    string coreExe = Path.Combine(pkg, "Atrium.exe");
    string settingsExe = Path.Combine(pkg, "Settings", "Atrium.Settings.exe");
    string settingsDll = Path.Combine(pkg, "Settings", "Atrium.Settings.dll");
    Directory.CreateDirectory(Path.GetDirectoryName(settingsExe)!);
    File.WriteAllText(coreExe, ""); File.WriteAllText(settingsExe, ""); File.WriteAllText(settingsDll, "");
    Version v27 = new(2, 7, 0, 0), v26 = new(2, 6, 0, 0);
    ReleaseValidation.ValidatePackage(pkg, v27, _ => v27);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v26), "Version mismatch must throw.");
    File.Delete(settingsDll);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v27), "Missing Atrium.Settings.dll must throw.");
    File.Delete(coreExe);
    ExpectThrows<IOException>(() => ReleaseValidation.ValidatePackage(pkg, v27, _ => v27), "Missing Atrium.exe must throw.");

    string cacheDir = Path.Combine(temp, "cache");
    string stateFile = Path.Combine(cacheDir, "ready.json");
    string workDir = Path.Combine(cacheDir, "package-background");
    Directory.CreateDirectory(workDir);
    ReleaseValidation.SaveJsonAtomic(stateFile, new { Tag = "v2.8.0", Version = "2.8.0" });
    Require(File.Exists(stateFile) && Directory.GetFiles(cacheDir, "*.tmp").Length == 0,
        "SaveJsonAtomic must write target without leaving temporary files.");
    ReleaseValidation.SaveJsonAtomic(stateFile, new { Tag = "v2.9.0", Version = "2.9.0" });
    Require(JsonNode.Parse(File.ReadAllText(stateFile))!["Version"]!.GetValue<string>() == "2.9.0",
        "SaveJsonAtomic must overwrite existing JSON atomically.");
    Require(ReleaseValidation.TryRecoverPrepared(() => "ok", stateFile, workDir) == "ok" && File.Exists(stateFile),
        "TryRecoverPrepared must reuse valid prepared package.");
    foreach (var ex in new Exception[]
    {
        new JsonException("corrupt"),
        new IOException("missing"),
        new FormatException("bad version"),
        new InvalidDataException("bad zip"),
        new ArgumentException("bad path"),
    })
    {
        Directory.CreateDirectory(workDir);
        File.WriteAllText(Path.Combine(workDir, "stale.txt"), "x");
        File.WriteAllText(stateFile, "{corrupt");
        Exception? logged = null;
        string? recovered = ReleaseValidation.TryRecoverPrepared<string>(
            () => throw ex, stateFile, workDir, e => logged = e);
        Require(recovered == null && ReferenceEquals(logged, ex) && !File.Exists(stateFile) && !Directory.Exists(workDir),
            $"TryRecoverPrepared must log and clean up on {ex.GetType().Name}.");
    }
    Directory.CreateDirectory(workDir);
    File.WriteAllText(stateFile, "{}");
    ExpectThrows<UnauthorizedAccessException>(
        () => ReleaseValidation.TryRecoverPrepared<string>(() => throw new UnauthorizedAccessException("denied"), stateFile, workDir),
        "TryRecoverPrepared must not swallow UnauthorizedAccessException.");

    string mutexName = @"Local\Atrium.Update.Test." + Guid.NewGuid().ToString("N");
    string resultFile = Path.Combine(cacheDir, "result.json");
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == null,
        "ReadFailureWithLock must return null when result.json is absent.");
    ReleaseValidation.SaveJsonAtomic(resultFile, new { error = "boom" });
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == "boom" && !File.Exists(resultFile),
        "ReadFailureWithLock must return error and delete result.json.");
    ReleaseValidation.SaveJsonAtomic(resultFile, new { error = "" });
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName) == null && !File.Exists(resultFile),
        "ReadFailureWithLock must return null on empty error and delete result.json.");
    File.WriteAllText(resultFile, "{broken");
    Exception? corruptLogged = null;
    Require(ReleaseValidation.ReadFailureWithLock(resultFile, mutexName, e => corruptLogged = e) == null &&
            corruptLogged is JsonException && !File.Exists(resultFile),
        "ReadFailureWithLock must log, clean up corrupt result.json, and not throw.");

    Require(ShellIcons.ExecutableForIcon("foo.exe") == "foo.exe", "Bare exe must return itself.");
    Require(ShellIcons.ExecutableForIcon(@"C:\app\foo.lnk") == @"C:\app\foo.lnk", "Non-exe must return itself.");
    string iconExe = Path.Combine(temp, "MyApp.exe");
    string appIconExe = Path.Combine(temp, "app", "MyApp.exe");
    Require(ShellIcons.ExecutableForIcon(iconExe) == iconExe, "Missing app/<exe> must return original path.");
    Directory.CreateDirectory(Path.GetDirectoryName(appIconExe)!);
    File.WriteAllText(appIconExe, "");
    Require(ShellIcons.ExecutableForIcon(iconExe) == appIconExe, "Existing app/<exe> must be returned.");
}
finally
{
    if (Directory.Exists(temp)) Directory.Delete(temp, true);
}

{
    string cppFacesH = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "widget_faces.h"));
    string cppFaces = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "widget_faces.cpp"));
    string csFaces = File.ReadAllText(Path.Combine(repoRoot!, "settings", "Atrium.Settings", "WidgetFaces.cs"));
    string cppWidgets = File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "desktop_widgets.cpp")) + "\n" +
                        File.ReadAllText(Path.Combine(repoRoot!, "native", "core", "ui", "widget_storage.cpp"));
    var zhHans = JsonNode.Parse(File.ReadAllText(Path.Combine(repoRoot!, "locales", "zh-Hans.json")))!["strings"]!.AsObject();
    static string Slice(string src, string marker)
    {
        int start = src.IndexOf(marker, StringComparison.Ordinal);
        int end = src.IndexOf("};", start, StringComparison.Ordinal);
        int csEnd = src.IndexOf("];", start, StringComparison.Ordinal);
        if (end < 0 || (csEnd >= 0 && csEnd < end)) end = csEnd;
        return src[start..end];
    }
    var cppCities = CppCityRegex().Matches(Slice(cppFaces, "kCities ="))
        .Select(m => (Name: m.Groups[1].Value, Zone: m.Groups[2].Value)).ToList();
    var cppLegacy = CppLegacyRegex().Matches(Slice(cppFacesH, "kLegacy[] =")).ToDictionary(m => m.Groups[2].Value, m => m.Groups[1].Value);
    var csCities = CsCityRegex().Matches(Slice(csFaces, "WorldCities ="))
        .Select(m => (Name: m.Groups[1].Value, Zone: m.Groups[2].Value, Legacy: m.Groups[3].Value)).ToList();
    Require(cppCities.Count == 29 && csCities.Count == 29 &&
            cppCities.Zip(csCities).All(p => p.First.Name == p.Second.Name && p.First.Zone == p.Second.Zone &&
                                             cppLegacy.TryGetValue(p.Second.Name, out string? leg) && leg == p.Second.Legacy),
        "WorldCities in widget_faces.{h,cpp} and WidgetFaces.cs must have 29 identical entries.");
    Require(zhHans["Local"]?.GetValue<string>() == "本地" &&
            csCities.All(c => zhHans[c.Name]?.GetValue<string>() == c.Legacy),
        "Every canonical WorldCities key and Local must match locales/zh-Hans.json.");
    Require(cppFaces.Contains("""{L"Tokyo", L"London", L"New York"}""") &&
            cppFaces.Contains("""{L"Beijing", L"Tokyo", L"London", L"New York"}""") &&
            csFaces.Contains("""["Tokyo", "London", "New York"]""") &&
            csFaces.Contains("""["Beijing", "Tokyo", "London", "New York"]"""),
        "Default world clock cities must match between C++ and C#.");
    string Canonical(string s) => csCities.FirstOrDefault(c => c.Name == s || c.Legacy == s).Name ?? s;
    Require(Canonical("北京") == "Beijing" && Canonical("圣保罗") == "São Paulo" && Canonical("奥克兰") == "Auckland" &&
            Canonical("Beijing") == "Beijing" && Canonical("Unknown") == "Unknown",
        "Legacy Chinese city keys must normalize to canonical English keys.");
    Require(cppWidgets.Contains("CanonicalCity(") && csFaces.Contains("CanonicalCity("),
        "Both C++ and C# must normalize legacy city keys on load.");
}
{
    string persistTemp = Path.Combine(Path.GetTempPath(), "atrium-persist-cs-" + Guid.NewGuid().ToString("N"));
    try
    {
        Directory.CreateDirectory(persistTemp);
        string atomicPath = Path.Combine(persistTemp, "atomic.json");
        SettingsPersistence.WriteAtomicText(atomicPath, "{\"ok\":true}");
        byte[] atomicBytes = File.ReadAllBytes(atomicPath);
        Require(atomicBytes.Length > 0 && !(atomicBytes.Length >= 3 && atomicBytes[0] == 0xEF && atomicBytes[1] == 0xBB && atomicBytes[2] == 0xBF),
            "WriteAtomicText must write UTF-8 without BOM.");
        Require(!Directory.EnumerateFiles(persistTemp, "*.tmp").Any(), "WriteAtomicText must not leave .tmp files on success.");

        string dirTarget = Path.Combine(persistTemp, "dir-target");
        Directory.CreateDirectory(dirTarget);
        bool moveFailed = false;
        try { SettingsPersistence.WriteAtomicText(dirTarget, "fail"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { moveFailed = true; }
        Require(moveFailed && !Directory.EnumerateFiles(persistTemp, "*.tmp").Any(),
            "WriteAtomicText must clean up .tmp when File.Move fails.");

        string settingsPath = Path.Combine(persistTemp, "settings.json");
        var tasks = Enumerable.Range(0, 8).Select(i => Task.Run(() =>
        {
            var s = new BarSettings { Height = 40 + i, StartOnLogin = i % 2 == 0 };
            SettingsPersistence.WriteSettings(settingsPath, s);
        })).ToArray();
        await Task.WhenAll(tasks);
        var concurrentLoaded = SettingsPersistence.ReadSettings(settingsPath);
        Require(concurrentLoaded.Height is >= 40 and <= 47 && !Directory.EnumerateFiles(persistTemp, "*.tmp").Any(),
            "Concurrent WriteSettings must serialize cleanly without .tmp leftovers.");

        string startupPath = Path.Combine(persistTemp, "startup-settings.json");
        Require(!SettingsPersistence.WriteSettings(startupPath, new BarSettings { StartOnLogin = true, StartupMode = "run" }),
            "Writing default startup settings must report startupChanged=false.");
        Require(SettingsPersistence.WriteSettings(startupPath, new BarSettings { StartOnLogin = true, StartupMode = "task" }),
            "Switching StartupMode while StartOnLogin is true must report startupChanged=true.");
        Require(!SettingsPersistence.WriteSettings(startupPath, new BarSettings { StartOnLogin = true, StartupMode = "task" }),
            "Unchanged StartupMode=task must report startupChanged=false.");
        Require(SettingsPersistence.WriteSettings(startupPath, new BarSettings { StartOnLogin = false, StartupMode = "task" }),
            "Disabling StartOnLogin must report startupChanged=true.");
        Require(!SettingsPersistence.WriteSettings(startupPath, new BarSettings { StartOnLogin = false, StartupMode = "run" }),
            "Changing StartupMode while StartOnLogin is false must report startupChanged=false.");

        byte[] corruptBytes = Encoding.UTF8.GetBytes("{broken-cs");
        File.WriteAllBytes(settingsPath, corruptBytes);
        var fallback = SettingsPersistence.ReadSettings(settingsPath);
        Require(fallback.Height == new BarSettings().Height && File.ReadAllBytes(settingsPath).SequenceEqual(corruptBytes),
            "ReadSettings must return defaults and keep corrupt original bytes until overwrite.");
        string[] backupsAfterRead = Directory.GetFiles(persistTemp, "settings.corrupt.*.json");
        Require(backupsAfterRead.Length == 1 && File.ReadAllBytes(backupsAfterRead[0]).SequenceEqual(corruptBytes),
            "ReadSettings must back up corrupt settings with exact original bytes.");
        _ = SettingsPersistence.ReadSettings(settingsPath);
        Require(Directory.GetFiles(persistTemp, "settings.corrupt.*.json").Length == 1,
            "Repeated ReadSettings on unchanged corrupt file must deduplicate backups.");

        for (int i = 0; i < 4; i++)
        {
            File.WriteAllText(settingsPath, $"{{broken-round-{i}", new UTF8Encoding(false));
            SettingsPersistence.WriteSettings(settingsPath, new BarSettings { Height = 64 });
        }
        Require(Directory.GetFiles(persistTemp, "settings.corrupt.*.json").Length == 3 &&
                SettingsPersistence.ReadSettings(settingsPath).Height == 64,
            "WriteSettings must cap corrupt backups at 3 and persist valid settings.");

        byte[] lockedCorrupt = Encoding.UTF8.GetBytes("{broken-no-backup");
        File.WriteAllBytes(settingsPath, lockedCorrupt);
        bool localizedCalled = false;
        ExpectThrows<IOException>(
            () => SettingsPersistence.WriteSettings(settingsPath, new BarSettings { Height = 50 },
                (key, fmtArgs) => { localizedCalled = true; return "LOC:" + string.Format(CultureInfo.InvariantCulture, key, fmtArgs); }, _ => false),
            "WriteSettings must refuse overwrite when corrupt backup fails.");
        Require(localizedCalled && File.ReadAllBytes(settingsPath).SequenceEqual(lockedCorrupt),
            "Failed corrupt backup must preserve original bytes and invoke localize callback.");

        byte[] futureBytes = Encoding.UTF8.GetBytes("""{"version":99,"height":80,"futureNested":{"k":"v"}}""");
        File.WriteAllBytes(settingsPath, futureBytes);
        var futureRead = SettingsPersistence.ReadSettings(settingsPath);
        Require(futureRead.Height == 80, "ReadSettings must parse compatible fields from future version read-only.");
        ExpectThrows<IOException>(
            () => SettingsPersistence.WriteSettings(settingsPath, new BarSettings { Height = 44 }),
            "WriteSettings must refuse to overwrite future-version settings.");
        Require(File.ReadAllBytes(settingsPath).SequenceEqual(futureBytes),
            "Future-version refusal must leave original bytes untouched.");
    }
    finally
    {
        if (Directory.Exists(persistTemp)) Directory.Delete(persistTemp, true);
    }
}
{
    // 1. SettingsSession: edit during reconnect GetAsync() await must survive
    var importSession = new SettingsSession();
    importSession.ApplyInitialState(new BarSettings { Height = 60 }, new BarSettings { Height = 60 }, "inst-9", 1);
    importSession.Import(new BarSettings { Height = 90, FolderStackPinned = true });
    Require(importSession.Draft.Height == 90 && importSession.Draft.FolderStackPinned &&
            importSession.Saved.Height == 60 && importSession.HasUnsavedChanges,
        "An imported file must replace the draft and read as unsaved.");
    var session = new SettingsSession();
    session.ApplyInitialState(new BarSettings { Height = 60, Margin = 10 }, new BarSettings { Height = 60, Margin = 10 }, "inst-1", 1);
    long draftBeforeReconnect = session.DraftVersion;
    session.Draft.Height = 76;
    session.MarkEdited();
    bool needPreview = session.ApplyReconnectSnapshot(
        new BarSettings { Height = 60, Margin = 22 },
        new BarSettings { Height = 60, Margin = 22 },
        "inst-2",
        4,
        draftBeforeReconnect);
    Require(needPreview && session.HasUnsavedChanges && session.Draft.Height == 76 && session.Draft.Margin == 22 &&
            session.Saved.Margin == 22 && session.InstanceId == "inst-2" && session.Revision == 4,
        "ApplyReconnectSnapshot must preserve edits made while GetAsync() was awaiting and merge baseline updates.");

    // 2. SettingsSession: edits made while CommitAsync is in flight stay unsaved after ApplyCommitSuccess
    var sent = session.Draft.Clone();
    session.Draft.Blur = 0.85;
    session.MarkEdited();
    Require(session.ApplyCommitSuccess(sent, "inst-2", 5) && session.Revision == 5 &&
            session.Saved.Height == 76 && session.Draft.Blur == 0.85 && session.HasUnsavedChanges,
        "ApplyCommitSuccess must update Saved/Revision while keeping edits made during commit.");

    // 3. SettingsSession: external changes merge non-conflicting fields while keeping draft edits
    bool extApplied = session.ApplyExternalChange(new BarSettings { Height = 76, Margin = 22, ShowClock = false, Blur = 0.25 }, 6);
    Require(extApplied && session.Revision == 6 && !session.Draft.ShowClock && session.Draft.Blur == 0.85 && !session.Saved.ShowClock,
        "ApplyExternalChange must adopt external field changes while keeping conflicting draft edits.");

    // 4. SettingsSession: 3-way conflict merge preserves unsubmitted draft diff over server baseline
    var conflictSession = new SettingsSession();
    conflictSession.ApplyInitialState(
        new BarSettings { Height = 60, Margin = 10, ShowClock = true },
        new BarSettings { Height = 60, Margin = 10, ShowClock = true },
        "inst-a",
        1);
    conflictSession.Draft.Height = 84;
    conflictSession.MarkEdited();
    conflictSession.ApplyConflict(new BarSettings { Height = 68, Margin = 18, ShowClock = false }, 3, "inst-b");
    Require(conflictSession.Saved.Height == 68 && conflictSession.Saved.Margin == 18 && !conflictSession.Saved.ShowClock &&
            conflictSession.Draft.Height == 84 && conflictSession.Draft.Margin == 18 && !conflictSession.Draft.ShowClock &&
            conflictSession.Revision == 3 && conflictSession.InstanceId == "inst-b" && conflictSession.HasUnsavedChanges,
        "ApplyConflict must rebase on serverSaved and preserve unsubmitted draft differences via 3-way merge.");

    // 5. SettingsSession: Close() rejects late async responses
    conflictSession.Close();
    long closedDraftVer = conflictSession.DraftVersion;
    Require(!conflictSession.ApplyInitialState(new(), new(), "inst-z", 99) &&
            !conflictSession.ApplyReconnectSnapshot(new(), new(), "inst-z", 99, closedDraftVer) &&
            !conflictSession.ApplyCommitSuccess(new(), "inst-z", 99) &&
            !conflictSession.ApplyExternalChange(new BarSettings { Height = 99 }, 99) &&
            !conflictSession.ApplyConflict(new BarSettings { Height = 99 }, 99, "inst-z") &&
            conflictSession.Revision == 3 && conflictSession.InstanceId == "inst-b" && conflictSession.Draft.Height == 84,
        "Closed SettingsSession must ignore late async state/commit/conflict responses.");

    // 6. CoreClient: concurrent offline CommitAsync writes are serialized via _io (early slow write cannot overwrite later write)
    string sessionTemp = Path.Combine(Path.GetTempPath(), "atrium-session-cs-" + Guid.NewGuid().ToString("N"));
    try
    {
        Directory.CreateDirectory(sessionTemp);
        string offlinePath = Path.Combine(sessionTemp, "settings.json");
        var firstStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var allowFirstToFinish = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var writeOrder = new List<double>();
        using var offlineClient = new CoreClient(
            pipeName: "Atrium.Test.Missing." + Guid.NewGuid().ToString("N"),
            settingsPath: offlinePath,
            writeFileOverride: s =>
            {
                if (s.Height == 44)
                {
                    firstStarted.TrySetResult();
                    allowFirstToFinish.Task.GetAwaiter().GetResult();
                }
                SettingsPersistence.WriteSettings(offlinePath, s);
                lock (writeOrder) writeOrder.Add(s.Height);
                return null;
            });
        var slowFirst = offlineClient.CommitAsync(new BarSettings { Height = 44 });
        await firstStarted.Task;
        var fastSecond = offlineClient.CommitAsync(new BarSettings { Height = 88 });
        var readThird = offlineClient.GetAsync();
        await Task.Delay(60);
        Require(!fastSecond.IsCompleted && !readThird.IsCompleted,
            "Offline CommitAsync and GetAsync must serialize behind an in-flight offline write.");
        allowFirstToFinish.SetResult();
        var r1 = await slowFirst;
        var r2 = await fastSecond;
        var s3 = await readThird;
        Require(r1.Succeeded && r2.Succeeded && writeOrder.SequenceEqual([44, 88]) &&
                s3.Saved.Height == 88 && SettingsPersistence.ReadSettings(offlinePath).Height == 88,
            "Concurrent offline saves must serialize in order so the newer write wins.");

        // 6b. Disposed CoreClient must not read offline file on GetAsync()
        offlineClient.Dispose();
        var afterDispose = await offlineClient.GetAsync();
        Require(!afterDispose.FromCore && afterDispose.Saved.Height == new BarSettings().Height,
            "Disposed CoreClient must skip offline disk/pin reads and return default state.");

        // 7. CoreClient + SettingsSession over a real NamedPipeServerStream: conflict -> 3-way merge -> retry commit succeeds
        string pipeName = "Atrium.Test.Session." + Guid.NewGuid().ToString("N");
        var pipeSession = new SettingsSession();
        var serverTask = Task.Run(async () =>
        {
            var mode = OperatingSystem.IsWindows() ? PipeTransmissionMode.Message : PipeTransmissionMode.Byte;
            using var pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1, mode, PipeOptions.Asynchronous);
            await pipe.WaitForConnectionAsync();
            static async Task<JsonObject> ReadMsgAsync(NamedPipeServerStream p)
            {
                byte[] buf = new byte[4096];
                using var ms = new MemoryStream();
                do
                {
                    int n = await p.ReadAsync(buf);
                    if (n == 0) throw new EndOfStreamException();
                    ms.Write(buf, 0, n);
                } while (!p.IsMessageComplete);
                return JsonNode.Parse(ms.ToArray())!.AsObject();
            }
            static async Task WriteMsgAsync(NamedPipeServerStream p, JsonObject o)
            {
                byte[] bytes = Encoding.UTF8.GetBytes(o.ToJsonString());
                await p.WriteAsync(bytes);
                await p.FlushAsync();
            }
            var m1 = await ReadMsgAsync(pipe);
            Require(m1["op"]?.GetValue<string>() == "get", "Expected initial get op.");
            await WriteMsgAsync(pipe, new JsonObject
            {
                ["op"] = "state",
                ["protocol"] = 1,
                ["instanceId"] = "srv-1",
                ["revision"] = 1,
                ["saved"] = new BarSettings { Height = 60, ShowClock = true }.ToJson(),
                ["current"] = new BarSettings { Height = 60, ShowClock = true }.ToJson(),
                ["path"] = offlinePath,
            });
            var m2 = await ReadMsgAsync(pipe);
            Require(m2["op"]?.GetValue<string>() == "commit" && m2["instanceId"]?.GetValue<string>() == "srv-1" &&
                    m2["expectedRevision"]?.GetValue<ulong>() == 1,
                "First online commit must send instanceId and expectedRevision=1.");
            await WriteMsgAsync(pipe, new JsonObject
            {
                ["op"] = "conflict",
                ["instanceId"] = "srv-1",
                ["revision"] = 2,
                ["saved"] = new BarSettings { Height = 60, ShowClock = false }.ToJson(),
                ["current"] = new BarSettings { Height = 60, ShowClock = false }.ToJson(),
                ["message"] = "settings changed since last read",
            });
            var m3 = await ReadMsgAsync(pipe);
            Require(m3["op"]?.GetValue<string>() == "commit" && m3["expectedRevision"]?.GetValue<ulong>() == 2 &&
                    Math.Abs(m3["settings"]!["height"]!.GetValue<double>() - 92) < 1e-6 &&
                    !m3["settings"]!["showClock"]!.GetValue<bool>(),
                "Retry commit after 3-way conflict merge must send expectedRevision=2 with preserved draft and server change.");
            await WriteMsgAsync(pipe, new JsonObject
            {
                ["op"] = "ok",
                ["instanceId"] = "srv-1",
                ["revision"] = 3,
            });
        });

        using var onlineClient = new CoreClient(pipeName, offlinePath, Environment.ProcessPath);
        Require(await onlineClient.ConnectAsync(3000), "CoreClient must connect to in-process test pipe.");
        var initialState = await onlineClient.GetAsync();
        pipeSession.ApplyInitialState(initialState.Saved, initialState.Current, initialState.InstanceId, initialState.Revision);
        pipeSession.Draft.Height = 92;
        pipeSession.MarkEdited();
        var conflictRes = await onlineClient.CommitAsync(pipeSession.Draft.Clone(), pipeSession.InstanceId, pipeSession.Revision);
        Require(conflictRes.IsConflict && conflictRes.Revision == 2 && conflictRes.ServerSaved != null,
            "CoreClient.CommitAsync must surface structured conflict result.");
        pipeSession.ApplyConflict(conflictRes.ServerSaved!, conflictRes.Revision, conflictRes.InstanceId);
        Require(pipeSession.Draft.Height == 92 && !pipeSession.Draft.ShowClock,
            "SettingsSession must keep draft height=92 while merging server showClock=false.");
        var retrySent = pipeSession.Draft.Clone();
        var okRes = await onlineClient.CommitAsync(retrySent, pipeSession.InstanceId, pipeSession.Revision);
        pipeSession.ApplyCommitSuccess(retrySent, okRes.InstanceId, okRes.Revision);
        await serverTask;
        Require(okRes.Succeeded && okRes.Revision == 3 && pipeSession.Revision == 3 && !pipeSession.HasUnsavedChanges,
            "Retry commit after conflict must succeed and advance revision to 3.");

        // 7b. SettingsSession: 3-way list merge for Pins and StageExcluded
        static PinnedApp Pin(string name, string path, string args = "", string aumid = "", bool? multiple = null) =>
            new(name, path, args, aumid, multiple);

        var pinA = Pin("A", @"C:\Apps\a.exe");
        var pinB = Pin("B", @"C:\Apps\b.exe");
        var pinC = Pin("C", @"C:\Apps\c.exe");
        var pinH = Pin("H", @"C:\Apps\h.exe");
        var pinM = Pin("M", @"C:\Apps\m.exe");
        var pinT = Pin("T", @"C:\Apps\t.exe");

        // Reorder + tail pin in both ApplyConflict and ApplyExternalChange: [A,B] + draft=[B,A] + target=[A,B,C] -> [B,A,C]
        foreach (bool viaExternal in new[] { false, true })
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB] }, new BarSettings { Pins = [pinA, pinB] }, "i1", 1);
            s.Draft.Pins = [pinB, pinA];
            s.MarkEdited();
            if (viaExternal) s.ApplyExternalChange(new BarSettings { Pins = [pinA, pinB, pinC] }, 2);
            else s.ApplyConflict(new BarSettings { Pins = [pinA, pinB, pinC] }, 2, "i1");
            Require(s.Draft.Pins != null && s.Draft.Pins.SequenceEqual([pinB, pinA, pinC]),
                $"3-way Pins merge ({(viaExternal ? "ApplyExternalChange" : "ApplyConflict")}) must keep reorder and tail-append C.");
            Require(s.HasUnsavedChanges, "Merged Pins must stay dirty when local reorder differs from server.");
        }

        // Local unpin + remote add
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB] }, new BarSettings { Pins = [pinA, pinB] }, "i1", 1);
            s.Draft.Pins = [pinA];
            s.ApplyConflict(new BarSettings { Pins = [pinA, pinB, pinC] }, 2, "i1");
            Require(s.Draft.Pins!.SequenceEqual([pinA, pinC]), "Local unpin must survive remote tail add.");
        }

        // Remote unpin + local reorder
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB, pinC] }, new BarSettings { Pins = [pinA, pinB, pinC] }, "i1", 1);
            s.Draft.Pins = [pinC, pinB, pinA];
            s.ApplyConflict(new BarSettings { Pins = [pinA, pinC] }, 2, "i1");
            Require(s.Draft.Pins!.SequenceEqual([pinC, pinA]), "Remote unpin must remove B while keeping local reorder.");
        }

        // Local add + remote reorder
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB] }, new BarSettings { Pins = [pinA, pinB] }, "i1", 1);
            s.Draft.Pins = [pinA, pinB, pinC];
            s.ApplyExternalChange(new BarSettings { Pins = [pinB, pinA] }, 2);
            Require(s.Draft.Pins!.SequenceEqual([pinB, pinA, pinC]), "Local add must append to tail after remote reorder.");
        }

        // Concurrent same-key add deduped (draft wins; Path/Aumid OrdinalIgnoreCase, Args Ordinal) + input dedup
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA] }, new BarSettings { Pins = [pinA] }, "i1", 1);
            var draftC = Pin("C-Draft", @"c:\apps\c.exe", "--x", "App.C", true);
            var targetC = Pin("C-Target", @"C:\Apps\C.EXE", "--x", "app.c", false);
            var diffArgsC = Pin("C-Args", @"C:\Apps\C.EXE", "--X", "app.c", false);
            s.Draft.Pins = [pinA, draftC, Pin("C-Dup", @"C:\Apps\C.EXE", "--x", "APP.C")];
            s.ApplyConflict(new BarSettings { Pins = [pinA, targetC, diffArgsC] }, 2, "i1");
            Require(s.Draft.Pins!.Count == 3 && s.Draft.Pins[1] == draftC && s.Draft.Pins[2] == diffArgsC,
                "Concurrent same-key pin add must dedupe by (Path/Aumid CI, Args CS) preferring draft.");
        }

        // Head and middle anchor insertions
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB, pinC] }, new BarSettings { Pins = [pinA, pinB, pinC] }, "i1", 1);
            s.Draft.Pins = [pinC, pinB, pinA];
            s.ApplyConflict(new BarSettings { Pins = [pinH, pinA, pinM, pinB, pinC, pinT] }, 2, "i1");
            Require(s.Draft.Pins!.SequenceEqual([pinH, pinC, pinB, pinA, pinM, pinT]),
                "Head, middle, and tail anchor splicing must place secondary additions accurately.");
        }

        // Concurrent non-key field updates on surviving pin (MultipleWindows + Name)
        {
            var s = new SettingsSession();
            var basePin = Pin("OldName", @"C:\Apps\app.exe", "", "", null);
            s.ApplyInitialState(new BarSettings { Pins = [basePin] }, new BarSettings { Pins = [basePin] }, "i1", 1);
            s.Draft.Pins = [Pin("OldName", @"C:\Apps\app.exe", "", "", true)];
            s.ApplyConflict(new BarSettings { Pins = [Pin("NewName", @"C:\Apps\app.exe", "", "", null)] }, 2, "i1");
            Require(s.Draft.Pins!.Count == 1 && s.Draft.Pins[0].Name == "NewName" && s.Draft.Pins[0].MultipleWindows == true,
                "Surviving pin must merge concurrent Name and MultipleWindows edits.");
        }

        // Pins null vs [] semantics
        {
            var sn1 = new SettingsSession();
            sn1.ApplyInitialState(new BarSettings { Pins = null }, new BarSettings { Pins = null }, "i1", 1);
            sn1.ApplyExternalChange(new BarSettings { Pins = [pinA] }, 2);
            Require(sn1.Draft.Pins != null && sn1.Draft.Pins.SequenceEqual([pinA]), "Unedited null Pins must adopt server pins.");

            var sn2 = new SettingsSession();
            sn2.ApplyInitialState(new BarSettings { Pins = null }, new BarSettings { Pins = null }, "i1", 1);
            sn2.Draft.Pins = [];
            sn2.ApplyConflict(new BarSettings { Pins = [pinA] }, 2, "i1");
            Require(sn2.Draft.Pins != null && sn2.Draft.Pins.Count == 0, "Explicit empty Pins must stay empty against server import.");

            var sn3 = new SettingsSession();
            sn3.ApplyInitialState(new BarSettings { Pins = [pinA] }, new BarSettings { Pins = [pinA] }, "i1", 1);
            sn3.Draft.Pins = [];
            sn3.ApplyConflict(new BarSettings { Pins = [pinA] }, 2, "i1");
            Require(sn3.Draft.Pins != null && sn3.Draft.Pins.Count == 0, "Explicit empty Pins must stay empty when server Pins unchanged.");

            var sn4 = new SettingsSession();
            sn4.ApplyInitialState(new BarSettings { Pins = null }, new BarSettings { Pins = null }, "i1", 1);
            sn4.Draft.Pins = [pinB];
            sn4.ApplyConflict(new BarSettings { Pins = [pinA] }, 2, "i1");
            Require(sn4.Draft.Pins != null && sn4.Draft.Pins.SequenceEqual([pinA, pinB]),
                "Null base with non-empty edits on both sides must merge additions.");
        }

        // 64-pin overflow on merge: never silently truncate; preserve full merge and report unsaved overflow
        {
            var base63 = Enumerable.Range(1, 63).Select(i => Pin($"P{i}", $@"C:\p{i}.exe")).ToList();
            var pinX = Pin("AppX", @"C:\x.exe");
            var pinY = Pin("AppY", @"C:\y.exe");
            var reorderedBase = new List<PinnedApp>(base63);
            (reorderedBase[0], reorderedBase[1]) = (reorderedBase[1], reorderedBase[0]);

            // 63 + draft X (reordered + scalar Height=84) + server Y across all 4 merge paths
            foreach (string mode in new[] { "conflict", "external", "reconnect", "initial-pending" })
            {
                var s = new SettingsSession();
                var baseSettings = new BarSettings { Height = 60, Pins = [.. base63] };
                var serverSettings = new BarSettings { Height = 60, ShowClock = false, Pins = [.. base63, pinY] };
                var draftPins = (List<PinnedApp>)[.. reorderedBase, pinX];
                if (mode == "initial-pending")
                {
                    var pendingCurrent = new BarSettings { Height = 84, Pins = draftPins };
                    s.ApplyInitialState(serverSettings, serverSettings, "i1", 2, baseSettings, pendingCurrent);
                }
                else
                {
                    s.ApplyInitialState(baseSettings, baseSettings, "i1", 1);
                    s.Draft.Height = 84;
                    s.Draft.Pins = draftPins;
                    s.MarkEdited();
                    if (mode == "conflict") s.ApplyConflict(serverSettings, 2, "i1");
                    else if (mode == "external") s.ApplyExternalChange(serverSettings, 2);
                    else s.ApplyReconnectSnapshot(serverSettings, serverSettings, "i1", 2, s.DraftVersion - 1);
                }

                Require(s.Draft.Pins!.Count == 65 && s.HasPinOverflow && s.HasUnsavedChanges && s.Draft.Height == 84 && !s.Draft.ShowClock,
                    $"63+1+1 merge ({mode}) must keep 65 pins, set HasPinOverflow, and preserve scalar edits.");
                Require(s.OverflowPins.SequenceEqual([pinX]),
                    $"63+1+1 merge ({mode}) OverflowPins must identify unsaved X even when draft reordered, not server-saved Y.");
                Require(s.Saved.Pins!.Count == 64 && s.Saved.Pins.Contains(pinY),
                    $"63+1+1 merge ({mode}) must keep 64 server-saved pins including Y.");

                // Reordering while already in overflow must keep HasPinOverflow and OverflowPins accurate
                (s.Draft.Pins[0], s.Draft.Pins[1]) = (s.Draft.Pins[1], s.Draft.Pins[0]);
                s.MarkEdited();
                Require(s.HasPinOverflow && s.OverflowPins.SequenceEqual([pinX]),
                    $"Reordering while overflowing ({mode}) must keep HasPinOverflow and OverflowPins=[X].");
            }

            // Resolution path (a): Revert() discards X and restores server's 64 pins along with any scalar edits
            {
                var s = new SettingsSession();
                s.ApplyInitialState(new BarSettings { Height = 60, Pins = [.. base63] }, new BarSettings { Height = 60, Pins = [.. base63] }, "i1", 1);
                s.Draft.Height = 84;
                s.Draft.Pins = [.. reorderedBase, pinX];
                s.ApplyConflict(new BarSettings { Height = 60, Pins = [.. base63, pinY] }, 2, "i1");
                Require(s.HasPinOverflow && s.Draft.Height == 84, "Precondition: 65-pin conflict must overflow and preserve draft height.");
                s.Revert();
                Require(!s.HasPinOverflow && s.OverflowPins.Count == 0 && !s.HasUnsavedChanges &&
                        s.Draft.Height == 60 && s.Draft.Pins!.SequenceEqual([.. base63, pinY]),
                    "Revert() must clear overflow, revert scalar edits, and restore server's 64 pins.");
            }

            // Resolution path (b): removing 1 pin drops to 64, round-trips JSON and commits cleanly
            {
                var s = new SettingsSession();
                s.ApplyInitialState(new BarSettings { Pins = [.. base63] }, new BarSettings { Pins = [.. base63] }, "i1", 1);
                s.Draft.Pins = [.. base63, pinX];
                s.ApplyConflict(new BarSettings { Pins = [.. base63, pinY] }, 2, "i1");
                Require(s.Draft.Pins!.Count == 65 && s.OverflowPins.SequenceEqual([pinX]),
                    "Precondition: non-reordered 63+1+1 merge must hold 65 pins with OverflowPins=[X].");
                s.Draft.Pins.RemoveAt(0); // remove P1 -> P2..P63 + Y + X (64 items)
                s.MarkEdited();
                var expected64 = base63.Skip(1).Concat([pinY, pinX]).ToList();
                Require(!s.HasPinOverflow && s.OverflowPins.Count == 0 && s.HasUnsavedChanges &&
                        s.Draft.Pins.SequenceEqual(expected64),
                    "Removing 1 pin from 65-pin merge must clear overflow and leave exact 64-pin order.");
                var roundTrip = BarSettings.FromJson(s.Draft.ToJson(), new());
                Require(roundTrip.Pins != null && roundTrip.Pins.SequenceEqual(expected64),
                    "Resolved 64-pin draft must round-trip ToJson/FromJson with zero item or order loss.");
                var committed = s.Draft.Clone();
                s.ApplyCommitSuccess(committed, "i1", 3);
                var savedRoundTrip = BarSettings.FromJson(s.Saved.ToJson(), new());
                Require(!s.HasUnsavedChanges && !s.HasPinOverflow && savedRoundTrip.Pins!.SequenceEqual(expected64),
                    "ApplyCommitSuccess after resolving overflow must clear HasUnsavedChanges and preserve all 64 pins.");
            }

            // Both sides add the same 64th pin -> 64 pins, no overflow
            {
                var s = new SettingsSession();
                s.ApplyInitialState(new BarSettings { Pins = [.. base63] }, new BarSettings { Pins = [.. base63] }, "i1", 1);
                s.Draft.Pins = [.. base63, pinX];
                s.ApplyConflict(new BarSettings { Pins = [.. base63, pinX] }, 2, "i1");
                Require(s.Draft.Pins!.Count == 64 && !s.HasPinOverflow && s.OverflowPins.Count == 0 && !s.HasUnsavedChanges,
                    "Adding the identical 64th pin on both sides must converge at 64 without overflow.");
            }

            // Exact 64 pins -> no overflow
            {
                var s = new SettingsSession();
                var full64 = Enumerable.Range(1, 64).Select(i => Pin($"F{i}", $@"C:\f{i}.exe")).ToList();
                s.ApplyInitialState(new BarSettings { Pins = full64 }, new BarSettings { Pins = full64 }, "i1", 1);
                Require(s.Draft.Pins!.Count == 64 && !s.HasPinOverflow && s.OverflowPins.Count == 0,
                    "Exact 64 pins must not trigger HasPinOverflow.");
            }

            // target40 + draft40 -> full 80 kept, OverflowPins == [D24..D39]
            {
                var s = new SettingsSession();
                s.ApplyInitialState(new BarSettings { Pins = [] }, new BarSettings { Pins = [] }, "i1", 1);
                var target40 = Enumerable.Range(0, 40).Select(i => Pin($"T{i}", $@"C:\t{i}.exe")).ToList();
                var draft40 = Enumerable.Range(0, 40).Select(i => Pin($"D{i}", $@"C:\d{i}.exe")).ToList();
                s.Draft.Pins = draft40;
                s.ApplyConflict(new BarSettings { Pins = target40 }, 2, "i1");
                Require(s.Draft.Pins!.Count == 80 && s.HasPinOverflow && s.Draft.Pins[0] == target40[0] &&
                        s.Draft.Pins[40] == draft40[0] && s.Draft.Pins[79] == draft40[39],
                    "Merged Pins exceeding 64 items must keep all 80 items without silent truncation.");
                Require(s.OverflowPins.SequenceEqual(draft40.Skip(24)),
                    "target40 + draft40 OverflowPins must report exact excess [D24..D39].");
            }
        }

        // Identical changes on both sides leave HasUnsavedChanges == false
        {
            var s = new SettingsSession();
            s.ApplyInitialState(new BarSettings { Pins = [pinA, pinB] }, new BarSettings { Pins = [pinA, pinB] }, "i1", 1);
            s.Draft.Pins = [pinA];
            s.ApplyConflict(new BarSettings { Pins = [pinA] }, 2, "i1");
            Require(!s.HasUnsavedChanges && s.Draft.Pins!.SequenceEqual([pinA]) && s.Saved.Pins!.SequenceEqual([pinA]),
                "Identical changes on both sides must converge with HasUnsavedChanges == false.");
        }

        // StageExcluded 3-way merge (ASCII trim + OrdinalIgnoreCase + reorder/tail)
        {
            var s = new SettingsSession();
            s.ApplyInitialState(
                new BarSettings { StageExcluded = ["snippingtool.exe", "screenclippinghost.exe"] },
                new BarSettings { StageExcluded = ["snippingtool.exe", "screenclippinghost.exe"] },
                "i1", 1);
            s.Draft.StageExcluded = [" screenclippinghost.exe ", "snippingtool.exe"];
            s.ApplyConflict(new BarSettings { StageExcluded = ["SNIPPINGTOOL.EXE", "screenclippinghost.exe", "game.exe"] }, 2, "i1");
            Require(s.Draft.StageExcluded.SequenceEqual([" screenclippinghost.exe ", "SNIPPINGTOOL.EXE", "game.exe"]),
                "StageExcluded 3-way merge must preserve reorder, ASCII-trimmed CI identity, target edit, and tail addition.");

            var se2 = new SettingsSession();
            se2.ApplyInitialState(
                new BarSettings { StageExcluded = ["snippingtool.exe", "screenclippinghost.exe"] },
                new BarSettings { StageExcluded = ["snippingtool.exe", "screenclippinghost.exe"] },
                "i1", 1);
            se2.Draft.StageExcluded = ["screenclippinghost.exe"];
            se2.ApplyExternalChange(new BarSettings { StageExcluded = ["snippingtool.exe", "screenclippinghost.exe", "obs64.exe"] }, 2);
            Require(se2.Draft.StageExcluded.SequenceEqual(["screenclippinghost.exe", "obs64.exe"]),
                "StageExcluded 3-way merge must drop locally removed item while keeping remote addition.");
        }

        // NowPlayingApps 3-way merge (max 64, ASCII trim + OrdinalIgnoreCase, concurrent scalar doesn't clobber)
        {
            var np = new SettingsSession();
            np.ApplyInitialState(
                new BarSettings { Height = 60, NowPlayingApps = ["spotify.exe", "qqmusic.exe"] },
                new BarSettings { Height = 60, NowPlayingApps = ["spotify.exe", "qqmusic.exe"] },
                "i1", 1);
            np.Draft.NowPlayingApps = [" qqmusic.exe ", "cloudmusic.exe"];
            np.ApplyConflict(new BarSettings { Height = 72, NowPlayingApps = ["SPOTIFY.EXE", "qqmusic.exe", "applemusic.exe"] }, 2, "i1");
            Require(np.Draft.Height == 72 && np.Draft.NowPlayingApps.SequenceEqual([" qqmusic.exe ", "applemusic.exe", "cloudmusic.exe"]),
                "NowPlayingApps 3-way merge must keep local removal/add alongside remote scalar and list addition.");

            var npReorder = new SettingsSession();
            npReorder.ApplyInitialState(
                new BarSettings { NowPlayingApps = ["a.exe", "b.exe"] },
                new BarSettings { NowPlayingApps = ["a.exe", "b.exe"] },
                "i1", 1);
            npReorder.Draft.NowPlayingApps = ["b.exe", "a.exe"];
            npReorder.ApplyExternalChange(new BarSettings { NowPlayingApps = ["A.EXE", "b.exe", "c.exe"] }, 2);
            Require(npReorder.Draft.NowPlayingApps.SequenceEqual(["b.exe", "A.EXE", "c.exe"]),
                "NowPlayingApps 3-way merge must preserve local reorder, target edit, and tail addition.");

            var npCap = new SettingsSession();
            var npBase = Enumerable.Range(0, 60).Select(i => $"app{i}.exe").ToList();
            npCap.ApplyInitialState(new BarSettings { NowPlayingApps = [.. npBase] }, new BarSettings { NowPlayingApps = [.. npBase] }, "i1", 1);
            npCap.Draft.NowPlayingApps = [.. npBase, .. Enumerable.Range(60, 4).Select(i => $"app{i}.exe")];
            npCap.ApplyExternalChange(new BarSettings { NowPlayingApps = [.. npBase, "r1.exe", "r2.exe", "r3.exe"] }, 2);
            Require(npCap.Draft.NowPlayingApps.Count == 64, "NowPlayingApps 3-way merge must cap at 64 items.");
        }

        // FolderStacks 3-way merge (max 32, slash-trimmed + drive root preserved + case-insensitive)
        {
            var fs = new SettingsSession();
            fs.ApplyInitialState(
                new BarSettings { FolderStacks = [@"C:\", @"D:\Work"] },
                new BarSettings { FolderStacks = [@"C:\", @"D:\Work"] },
                "i1", 1);
            fs.Draft.FolderStacks = [@"D:\Work\", @"C:\", @"E:\Media\"];
            fs.ApplyConflict(new BarSettings { FolderStacks = [@"c:\", @"d:/work/", @"F:\Docs"] }, 2, "i1");
            Require(fs.Draft.FolderStacks.SequenceEqual([@"D:\Work\", @"c:\", @"E:\Media\", @"F:\Docs"]),
                "FolderStacks 3-way merge must match slash-trimmed paths, preserve drive roots, and keep local reorder + additions.");

            var fsDrop = new SettingsSession();
            fsDrop.ApplyInitialState(
                new BarSettings { FolderStacks = [@"C:\", @"D:\Work"] },
                new BarSettings { FolderStacks = [@"C:\", @"D:\Work"] },
                "i1", 1);
            fsDrop.Draft.FolderStacks = [@"C:\"];
            fsDrop.ApplyExternalChange(new BarSettings { FolderStacks = [@"C:\", @"D:\Work\", @"E:\Temp"] }, 2);
            Require(fsDrop.Draft.FolderStacks.SequenceEqual([@"C:\", @"E:\Temp"]),
                "FolderStacks 3-way merge must drop locally removed folder even when remote normalized its trailing slash.");

            var fsCap = new SettingsSession();
            var fsBase = Enumerable.Range(0, 30).Select(i => $@"D:\Dir{i}").ToList();
            fsCap.ApplyInitialState(new BarSettings { FolderStacks = [.. fsBase] }, new BarSettings { FolderStacks = [.. fsBase] }, "i1", 1);
            fsCap.Draft.FolderStacks = [.. fsBase, @"D:\Local1", @"D:\Local2"];
            fsCap.ApplyExternalChange(new BarSettings { FolderStacks = [.. fsBase, @"D:\Remote1", @"D:\Remote2", @"D:\Remote3"] }, 2);
            Require(fsCap.Draft.FolderStacks.Count == 32, "FolderStacks 3-way merge must cap at 32 items.");
        }

        // FolderStackFiles 3-way merge (max 32, case-insensitive slash-normalized path dedupe)
        {
            var fsf = new SettingsSession();
            fsf.ApplyInitialState(
                new BarSettings { FolderStackFiles = [@"C:\Docs\A.pdf", @"D:\Notes.txt"] },
                new BarSettings { FolderStackFiles = [@"C:\Docs\A.pdf", @"D:\Notes.txt"] },
                "i1", 1);
            fsf.Draft.FolderStackFiles = [@"D:\Notes.txt", @"C:\Docs\A.pdf", @"E:\Media\Song.mp3"];
            fsf.ApplyConflict(new BarSettings { FolderStackFiles = [@"c:/docs/a.pdf", @"d:\notes.txt", @"F:\Book.epub"] }, 2, "i1");
            Require(fsf.Draft.FolderStackFiles.SequenceEqual([@"d:\notes.txt", @"c:/docs/a.pdf", @"E:\Media\Song.mp3", @"F:\Book.epub"]),
                "FolderStackFiles 3-way merge must match case-insensitive paths and keep local reorder + additions.");

            var fsfDrop = new SettingsSession();
            fsfDrop.ApplyInitialState(
                new BarSettings { FolderStackFiles = [@"C:\Docs\A.pdf", @"D:\Notes.txt"] },
                new BarSettings { FolderStackFiles = [@"C:\Docs\A.pdf", @"D:\Notes.txt"] },
                "i1", 1);
            fsfDrop.Draft.FolderStackFiles = [@"C:\Docs\A.pdf"];
            fsfDrop.ApplyExternalChange(new BarSettings { FolderStackFiles = [@"C:\Docs\A.pdf", @"d:/notes.txt", @"E:\New.pdf"] }, 2);
            Require(fsfDrop.Draft.FolderStackFiles.SequenceEqual([@"C:\Docs\A.pdf", @"E:\New.pdf"]),
                "FolderStackFiles 3-way merge must drop locally removed file even when remote casing or slashes differ.");

            var fsfCap = new SettingsSession();
            var fsfBase = Enumerable.Range(0, 30).Select(i => $@"D:\File{i}.pdf").ToList();
            fsfCap.ApplyInitialState(new BarSettings { FolderStackFiles = [.. fsfBase] }, new BarSettings { FolderStackFiles = [.. fsfBase] }, "i1", 1);
            fsfCap.Draft.FolderStackFiles = [.. fsfBase, @"D:\Local1.pdf", @"D:\Local2.pdf"];
            fsfCap.ApplyExternalChange(
                new BarSettings { FolderStackFiles = [.. fsfBase, @"D:\Remote1.pdf", @"D:\Remote2.pdf", @"D:\Remote3.pdf"] }, 2);
            Require(fsfCap.Draft.FolderStackFiles.Count == 32, "FolderStackFiles 3-way merge must cap at 32 items.");
        }

        // ControlModules 3-way merge (dedupe, max 4, filtered by ControlModuleIds)
        {
            var cm = new SettingsSession();
            cm.ApplyInitialState(
                new BarSettings { Height = 60, ControlModules = ["stage"] },
                new BarSettings { Height = 60, ControlModules = ["stage"] },
                "i1", 1);
            cm.Draft.ControlModules = ["darkmode", "wifi", "stage", "darkmode"];
            cm.ApplyConflict(new BarSettings { Height = 72, ControlModules = ["stage", "hotspot"] }, 2, "i1");
            Require(cm.Draft.Height == 72 && cm.Draft.ControlModules.SequenceEqual(["darkmode", "stage", "hotspot"]),
                "ControlModules 3-way merge must filter unknown ids, dedupe, and merge local + remote additions.");

            var cmDrop = new SettingsSession();
            cmDrop.ApplyInitialState(
                new BarSettings { ControlModules = ["stage", "darkmode"] },
                new BarSettings { ControlModules = ["stage", "darkmode"] },
                "i1", 1);
            cmDrop.Draft.ControlModules = ["darkmode"];
            cmDrop.ApplyExternalChange(new BarSettings { ControlModules = ["stage", "nightlight", "energysaver"] }, 2);
            Require(cmDrop.Draft.ControlModules.SequenceEqual(["darkmode", "energysaver"]),
                "ControlModules 3-way merge must drop locally removed module and normalize nightlight.");

            var cmAll = new SettingsSession();
            cmAll.ApplyInitialState(
                new BarSettings { ControlModules = ["stage", "darkmode"] },
                new BarSettings { ControlModules = ["stage", "darkmode"] },
                "i1", 1);
            cmAll.Draft.ControlModules = ["darkmode", "stage", "energysaver", "dnd"];
            cmAll.ApplyConflict(new BarSettings { ControlModules = ["stage", "darkmode", "hotspot", "airplane"] }, 2, "i1");
            Require(cmAll.Draft.ControlModules.Count <= 6 &&
                    cmAll.Draft.ControlModules.SequenceEqual(["darkmode", "stage", "energysaver", "dnd", "hotspot", "airplane"]),
                "ControlModules 3-way merge must preserve reorder and cap at 6 known modules.");
        }

        // 7c. SettingsSession.Initialized & early-edit before ConnectAsync/GetAsync completes
        {
            var early = new SettingsSession();
            Require(!early.Initialized, "Fresh SettingsSession must start with Initialized == false.");
            long draftBefore = early.DraftVersion;
            early.Draft.Height = 84;
            early.MarkEdited();
            var srvSaved = new BarSettings { Height = 60, ShowClock = false, Transparency = 0.5 };
            bool keep = early.ApplyReconnectSnapshot(srvSaved, srvSaved.Clone(), "i1", 1, draftBefore);
            Require(early.Initialized && keep && early.HasUnsavedChanges &&
                    Math.Abs(early.Draft.Height - 84) < 1e-6 && !early.Draft.ShowClock &&
                    Math.Abs(early.Draft.Transparency - 0.5) < 1e-6,
                "Early edit before baseline load must merge with server baseline instead of being overwritten.");

            var init = new SettingsSession();
            Require(!init.Initialized, "Fresh SettingsSession before ApplyInitialState must have Initialized == false.");
            bool dirty = init.ApplyInitialState(srvSaved, srvSaved.Clone(), "i1", 1);
            Require(init.Initialized && !dirty && !init.HasUnsavedChanges,
                "ApplyInitialState without pending edits must set Initialized == true and clean state.");
        }

        // 8. ReleaseInstaller.ReadPreparedRecovering: corrupt ready.json, missing files, and escaping path auto-clean and return null
        string updCache = Path.Combine(sessionTemp, "upd-cache");
        string updState = Path.Combine(updCache, "ready.json");
        string updWork = Path.Combine(updCache, "package-background");
        Version curVer = new(2, 7, 0, 0), nextVer = new(2, 8, 0, 0);

        Directory.CreateDirectory(updWork);
        File.WriteAllText(updState, "{corrupt-ready-json");
        Exception? loggedRecover = null;
        Require(ReleaseInstaller.ReadPreparedRecovering(curVer, updCache, updState, updWork, e => loggedRecover = e) == null &&
                loggedRecover is JsonException && !File.Exists(updState) && !Directory.Exists(updWork),
            "ReadPreparedRecovering must clean corrupt ready.json and work directory.");

        string stagedPkg = Path.Combine(updWork, "unpacked");
        Directory.CreateDirectory(stagedPkg);
        ReleaseValidation.SaveJsonAtomic(updState, new ReleaseInstaller.Prepared("v2.8.0", "2.8.0.0", stagedPkg));
        loggedRecover = null;
        Require(ReleaseInstaller.ReadPreparedRecovering(curVer, updCache, updState, updWork, e => loggedRecover = e) == null &&
                loggedRecover is IOException && !File.Exists(updState) && !Directory.Exists(updWork),
            "ReadPreparedRecovering must clean staged package missing binaries.");

        Directory.CreateDirectory(updWork);
        string outsidePkg = Path.Combine(sessionTemp, "outside");
        Directory.CreateDirectory(outsidePkg);
        ReleaseValidation.SaveJsonAtomic(updState, new ReleaseInstaller.Prepared("v2.8.0", "2.8.0.0", outsidePkg));
        loggedRecover = null;
        Require(ReleaseInstaller.ReadPreparedRecovering(curVer, updCache, updState, updWork, e => loggedRecover = e) == null &&
                loggedRecover is IOException && !File.Exists(updState) && !Directory.Exists(updWork),
            "ReadPreparedRecovering must reject and clean cache-escaping ready.json.");

        Directory.CreateDirectory(Path.Combine(stagedPkg, "Settings"));
        File.WriteAllText(Path.Combine(stagedPkg, "Atrium.exe"), "");
        File.WriteAllText(Path.Combine(stagedPkg, "Settings", "Atrium.Settings.exe"), "");
        File.WriteAllText(Path.Combine(stagedPkg, "Settings", "Atrium.Settings.dll"), "");
        ReleaseValidation.SaveJsonAtomic(updState, new ReleaseInstaller.Prepared("v2.8.0", "2.8.0.0", stagedPkg));
        var validPrepared = ReleaseInstaller.ReadPreparedRecovering(curVer, updCache, updState, updWork, null, _ => nextVer);
        Require(validPrepared != null && validPrepared.Version == "2.8.0.0" && File.Exists(updState),
            "ReadPreparedRecovering must return valid prepared package untouched.");
    }
    finally
    {
        if (Directory.Exists(sessionTemp)) Directory.Delete(sessionTemp, true);
    }

    // 9. CoreClient.CoreStartInfo: propagate --development and normalized --data-dir only in dev mode
    {
        string exe = @"C:\Apps\Atrium\Atrium.exe";
        var prod = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe", "--development", "--data-dir", @"C:\tmp\iso"], development: false);
        Require(prod.FileName == exe && prod.UseShellExecute && prod.ArgumentList.Count == 0,
            "Production mode CoreStartInfo must use shell execute and ignore --data-dir.");

        var devDefault = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe", "--development"], development: true);
        Require(devDefault.FileName == exe && devDefault.UseShellExecute && devDefault.ArgumentList.Count == 0,
            "Dev mode CoreStartInfo without --data-dir must fall back to default UseShellExecute=true launch.");

        var devExitCase = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe", "--development", "--data-dir", @"D:\tmp\--exit-case"], development: true);
        Require(devExitCase.FileName == exe && !devExitCase.UseShellExecute &&
                devExitCase.ArgumentList.SequenceEqual(["--development", "--data-dir", @"D:\tmp\--exit-case"]),
            "Dev mode CoreStartInfo must forward --development and --data-dir via ArgumentList.");

        var devSpaces = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe", "--data-dir", @"D:\tmp\my data\iso\"], development: true);
        Require(!devSpaces.UseShellExecute &&
                devSpaces.ArgumentList.SequenceEqual(["--development", "--data-dir", @"D:\tmp\my data\iso"]),
            "Dev mode CoreStartInfo must normalize trailing separator and preserve spaces in ArgumentList.");

        var devRoot = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe", "--data-dir", @"D:\"], development: true);
        Require(!devRoot.UseShellExecute &&
                devRoot.ArgumentList.SequenceEqual(["--development", "--data-dir", @"D:\"]),
            "Dev mode CoreStartInfo must preserve root drive trailing backslash in ArgumentList.");

        var adminStart = CoreClient.CoreStartInfo(exe, ["Atrium.Settings.exe"], development: false, runAsAdmin: true, replace: true);
        Require(adminStart.UseShellExecute && adminStart.Verb == "runas" && adminStart.Arguments.Contains("--replace"),
            "Elevated CoreStartInfo must set UseShellExecute, Verb=runas, and --replace in Arguments.");
    }
}
Console.WriteLine("Hardening regression checks passed.");

internal partial class Program
{
    private static readonly string[] BuildConfigs = ["win-x64", "tests", "development", "tidy"];
    private static readonly string[] TintSamples = ["#000000", "#abcdef", "#123456", "# 12345", "#GGGGGG", "123456"];
    private static readonly string[] LangSamples = ["auto", "zh", "zh-Hans", "en", "fr-CA", "-en", "en--US", "bad tag", "中文"];
    [GeneratedRegex(@"^\|\s*`(\w+)`\s*\|", RegexOptions.Multiline)]
    private static partial Regex DocKeyRegex();
    [GeneratedRegex(@"^#[0-9a-fA-F]{6}$")]
    private static partial Regex TintRegex();
    [GeneratedRegex(@"new\(""[^""]+"",\s*Loc\.T\(""[^""]+""\),\s*([0-9.]+),\s*([0-9.]+),\s*[0-9.]+,\s*""[^""]*"",\s*s\s*=>\s*s\.(\w+)")]
    private static partial Regex SliderRegex();
    [GeneratedRegex(@"\{L""([^""]+)"",\s*L""([^""]+)""\}")]
    private static partial Regex CppCityRegex();
    [GeneratedRegex(@"\{L""([^""]+)"",\s*L""([^""]+)""\}")]
    private static partial Regex CppLegacyRegex();
    [GeneratedRegex(@"\(""([^""]+)"",\s*""([^""]+)"",\s*""([^""]+)""\)")]
    private static partial Regex CsCityRegex();
}
