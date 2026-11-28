#include "settings.h"
#include "shell/displays.h"
#include "shell/system_chrome.h"
#include "shell/task_model.h"
#include "ui/notification_layout.h"
#include "ui/widget_stacking.h"
#include "motion.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#include <iostream>
#include <string_view>

using namespace atrium;

namespace {
bool g_failed = false;

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    g_failed = true;
    return false;
}

void CheckClockAndBadgeSettings(Settings& settings, Settings& parsed)
{
    for (const wchar_t* mode : {L"follow", L"primary", L"fixed", L"all"}) {
        settings.dockDisplayMode = mode;
        Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.dockDisplayMode == mode, "every Dock mode survives saving");
    }
    settings.showClockSeconds = true;
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.showClockSeconds, "clock seconds survive saving");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.showClockSeconds && parsed.showClock && parsed.barBadges,
            "old settings keep the clock at minutes and unread badges on");
    {
        Settings noBadges;
        noBadges.barBadges = false;
        Require(Settings::FromJson(noBadges.ToJson(), Settings{}, parsed) && !parsed.barBadges, "barBadges false survives round trip");
        Require(Settings::FromJson(LR"({"barBadges":"bad"})", noBadges, parsed) && !parsed.barBadges, "malformed barBadges retains current value");
    }
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.magnifyNear, "icons do not magnify above the bar unless chosen");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.remoteVisible, "Atrium stays out of the screen's capture unless chosen");
    {
        Settings remote;
        remote.remoteVisible = true;
        Require(Settings::FromJson(remote.ToJson(), Settings{}, parsed) && parsed.remoteVisible, "remote-control mode survives saving");
    }
    Require(Settings::FromJson(LR"({"showClockSeconds":true})", settings, parsed) && parsed.showClockSeconds && parsed.showClock,
            "clock seconds toggle independently of the clock");
    Require(Settings::FromJson(LR"({"showClockSeconds":"bad"})", settings, parsed) && parsed.showClockSeconds,
            "malformed clock seconds retain the current value");
    settings.showClockSeconds = false;
    Require(Settings::FromJson(LR"({"showClockSeconds":true})", settings, parsed) && parsed.showClockSeconds && parsed.dockDisplayMode == L"all",
            "clock seconds preview without changing other keys");
    Settings::FromJson(LR"({})", settings, parsed);
}

void CheckSettings()
{
    Settings settings, parsed;
    settings.dockDisplayMode = L"all";
    settings.dockDisplayDelay = 750;
    settings.dockDisplay = L"\\\\?\\DISPLAY#external";
    settings.desktopDisplay = L"disconnected-screen";
    settings.taskbarApps = L"primaryAndWindow";
    settings.taskbarRightPrimaryOnly = true;
    settings.notifyPlacement = L"topRight";
    settings.hoverRise = false;
    settings.nearEnter = 40;
    settings.nearLeave = 4;
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed), "settings round trip parses");
    Require(parsed.nearEnter == 40 && parsed.nearLeave == 4, "a four-DIP leave setting survives the native round trip independently of enter");
    Require(Settings::FromJson(LR"({"nearLeave":4})", Settings{}, parsed) && parsed.nearLeave == 4,
            "a partial four-DIP leave update is not replaced with the default forty-eight");
    Require(!parsed.hoverRise, "disabled hover rise survives saving");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.hoverRise, "old settings keep the glass flat");
    Require(Settings::FromJson(LR"({})", settings, parsed) && !parsed.hoverRise, "partial updates retain disabled hover rise");
    Settings raised = settings;
    raised.hoverRise = true;
    Require(Settings::FromJson(LR"({})", raised, parsed) && parsed.hoverRise, "partial updates retain enabled hover rise");
    Require(Settings::FromJson(LR"({"hoverRise":"bad"})", settings, parsed) && !parsed.hoverRise, "malformed hover rise retains the current value");
    Require(Settings::FromJson(LR"({"hoverRise":true})", settings, parsed) && parsed.hoverRise && parsed.magnify == settings.magnify,
            "hover rise toggles without changing icon magnification");
    settings.language = L"en";
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.language == L"en", "the interface language survives saving");
    settings.stageExcluded = {L"wechat.exe", L"tencent.qq_"};
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.stageExcluded == std::vector<std::wstring>{L"wechat.exe", L"tencent.qq_"},
            "the Stage Manager blacklist survives saving");
    Require(Settings::FromJson(LR"({"stageExcluded":[]})", Settings{}, parsed) && parsed.stageExcluded.empty(),
            "an empty blacklist stays empty instead of falling back to the defaults");
    Require(Settings::FromJson(LR"({"stageExcluded":"wechat.exe"})", Settings{}, parsed) && parsed.stageExcluded == DefaultStageExcluded(),
            "a malformed blacklist keeps the current list");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.stageExcluded == DefaultStageExcluded(),
            "old settings leave the screenshot tools out of Stage Manager");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.language == L"auto", "old settings follow the system language");
    Require(Settings::FromJson(LR"({"language":"bad tag"})", Settings{}, parsed) && parsed.language == L"auto", "an unknown language follows the system");
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed), "restore settings round trip");
    Require(parsed.dockDisplayMode == L"all" && parsed.dockDisplayDelay == 750 && parsed.dockDisplay == settings.dockDisplay &&
                parsed.desktopDisplay == settings.desktopDisplay && parsed.taskbarApps == settings.taskbarApps && parsed.taskbarRightPrimaryOnly &&
                parsed.notifyPlacement == L"topRight",
            "all display and notification preferences survive saving");
    Require(Settings::FromJson(LR"({"taskbarRightPrimaryOnly":false})", settings, parsed) && !parsed.taskbarRightPrimaryOnly && parsed.showTray &&
                parsed.showStatus && parsed.showClock && parsed.showNotifications,
            "disabling the primary-only group retains individual content switches");
    Require(Settings::FromJson(LR"({"taskbarRightPrimaryOnly":"bad"})", settings, parsed) && parsed.taskbarRightPrimaryOnly,
            "malformed primary-only visibility retains the current value");
    Require(Settings::FromJson(LR"({})", settings, parsed) && parsed.taskbarRightPrimaryOnly, "partial updates retain primary-only visibility");
    CheckClockAndBadgeSettings(settings, parsed);
    for (const wchar_t* mode : {L"all", L"primaryAndWindow", L"window"}) {
        settings.taskbarApps = mode;
        Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.taskbarApps == mode, "every task distribution mode survives saving");
    }
    Require(Settings::FromJson(LR"({"height":64})", Settings{}, parsed) && parsed.dockDisplayMode == L"follow" && parsed.taskbarApps == L"all" &&
                parsed.dockDisplayDelay == 450 && !parsed.taskbarRightPrimaryOnly && parsed.notifyPlacement == L"dock",
            "old settings retain right-side elements and default dock notifications");
    Require(Settings::FromJson(LR"({"notifyPlacement":"bad"})", Settings{}, parsed) && parsed.notifyPlacement == L"dock",
            "invalid notification placement uses the default");
    Require(Settings::FromJson(LR"({"notifyPlacement":false})", settings, parsed) && parsed.notifyPlacement == L"topRight",
            "malformed notification placement retains the current value");
    Require(Settings::FromJson(LR"({"notifyPlacement":"dock"})", settings, parsed) && parsed.notifyPlacement == L"dock",
            "notification placement can return to the default");
    for (const wchar_t* corner : {L"topLeft", L"topRight", L"bottomLeft", L"bottomRight"}) {
        settings.notifyPlacement = corner;
        Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.notifyPlacement == corner,
                "every corner of the main screen survives saving");
    }
    settings.notifyPlacement = L"topRight";
    Require(Settings::FromJson(LR"({"dockDisplayMode":"bad","taskbarApps":"bad","dockDisplayDelay":99999})", Settings{}, parsed) &&
                parsed.dockDisplayMode == L"follow" && parsed.taskbarApps == L"all" && parsed.dockDisplayDelay == 2000,
            "invalid values are normalized");
    Require(Settings::FromJson(LR"({"dockDisplayDelay":-1})", Settings{}, parsed) && parsed.dockDisplayDelay == 100, "delay has a lower bound");
    Require(Settings::FromJson(LR"({"dockDisplayDelay":"bad","dockDisplay":false})", settings, parsed) && parsed.dockDisplayDelay == 750 &&
                parsed.dockDisplay == settings.dockDisplay,
            "malformed keys retain current values");
    Require(!Settings{}.notifyFullscreen && Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.notifyFullscreen, "notifyFullscreen defaults to false");
    settings.notifyFullscreen = true;
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.notifyFullscreen, "notifyFullscreen survives saving");
    Require(Settings::FromJson(LR"({})", settings, parsed) && parsed.notifyFullscreen, "partial updates retain notifyFullscreen");
    Require(Settings::FromJson(LR"({"notifyFullscreen":"bad"})", settings, parsed) && parsed.notifyFullscreen,
            "malformed notifyFullscreen retains the current value");
    Require(!Settings{}.clickWindowPicker && Settings::FromJson(LR"({})", Settings{}, parsed) && !parsed.clickWindowPicker,
            "clickWindowPicker defaults to false");
    settings.clickWindowPicker = true;
    Require(Settings::FromJson(settings.ToJson(), Settings{}, parsed) && parsed.clickWindowPicker, "clickWindowPicker survives saving");
    Require(Settings::FromJson(LR"({})", settings, parsed) && parsed.clickWindowPicker, "partial updates retain clickWindowPicker");
    Require(Settings::FromJson(LR"({"clickWindowPicker":"bad"})", settings, parsed) && parsed.clickWindowPicker,
            "malformed clickWindowPicker retains the current value");
}

void CheckStageDragMerge()
{
    Settings defaults, off, parsed;
    Require(defaults.stageDragMerge, "stageDragMerge is enabled by default");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.stageDragMerge, "old settings receive stageDragMerge default");
    off.stageDragMerge = false;
    Require(Settings::FromJson(off.ToJson(), Settings{}, parsed) && !parsed.stageDragMerge, "disabled stageDragMerge survives saving");
    Require(Settings::FromJson(LR"({})", off, parsed) && !parsed.stageDragMerge, "partial updates retain disabled stageDragMerge");
    Require(Settings::FromJson(LR"({"stageDragMerge":"bad"})", off, parsed) && !parsed.stageDragMerge, "malformed stageDragMerge retains fallback");
    Require(defaults.stageAvoid && Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.stageAvoid, "stageAvoid is on by default");
    Require(defaults.imeBubble && Settings::FromJson(LR"({"imeBubble":false})", Settings{}, parsed) && !parsed.imeBubble &&
                Settings::FromJson(parsed.ToJson(), Settings{}, parsed) && !parsed.imeBubble,
            "imeBubble is on by default, and off survives saving");
    Require(defaults.barReserve && defaults.barFullscreenHide && Settings::FromJson(LR"({"barReserve":false,"barFullscreenHide":false})", Settings{}, parsed) &&
                !parsed.barReserve && !parsed.barFullscreenHide && Settings::FromJson(parsed.ToJson(), Settings{}, parsed) && !parsed.barReserve &&
                !parsed.barFullscreenHide,
            "barReserve and barFullscreenHide are on by default, and off survives saving");
    Require(defaults.windowPeek && Settings::FromJson(LR"({"windowPeek":false})", Settings{}, parsed) && !parsed.windowPeek &&
                Settings::FromJson(parsed.ToJson(), Settings{}, parsed) && !parsed.windowPeek,
            "windowPeek is on by default, and off survives saving");
    Require(defaults.stageCloseButton && Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.stageCloseButton, "stageCloseButton is on by default");
    off.stageCloseButton = false;
    Require(Settings::FromJson(off.ToJson(), Settings{}, parsed) && !parsed.stageCloseButton, "disabled stageCloseButton survives saving");
    Require(defaults.stageScrollWrap && Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.stageScrollWrap, "stageScrollWrap is on by default");
    off.stageScrollWrap = false;
    Require(Settings::FromJson(off.ToJson(), Settings{}, parsed) && !parsed.stageScrollWrap, "disabled stageScrollWrap survives saving");
    Require(Settings::FromJson(LR"({})", off, parsed) && !parsed.stageScrollWrap, "partial updates retain disabled stageScrollWrap");
    Require(Settings::FromJson(LR"({"stageScrollWrap":"bad"})", off, parsed) && !parsed.stageScrollWrap, "malformed stageScrollWrap retains fallback");
}

void CheckSettingsNormalization()
{
    Settings parsed;
    Require(Settings::FromJson(LR"({"widgetBackdrop":"bogus","widgetMaterial":"bogus"})", Settings{}, parsed) && parsed.widgetBackdrop == L"desktop" &&
                parsed.widgetMaterial == L"glass",
            "invalid widget backdrop and material normalize to defaults");
    Require(Settings::FromJson(LR"({"widgetBackdrop":"wallpaper","widgetMaterial":"dusk"})", Settings{}, parsed) && parsed.widgetBackdrop == L"wallpaper" &&
                parsed.widgetMaterial == L"dusk",
            "valid widget backdrop and dusk material survive");
    Require(Settings::FromJson(LR"({"widgetMaterial":"color"})", Settings{}, parsed) && parsed.widgetMaterial == L"color", "color widget material survives");
    Require(Settings::FromJson(LR"({"stageExcluded":[" \t\r\n\u000B\ffoo.exe\f\u000B\n\r\t "," \t\r\n\u000B\f","\u3000bar.exe\u00A0"]})", Settings{}, parsed) &&
                parsed.stageExcluded == std::vector<std::wstring>{L"foo.exe", L"\u3000bar.exe\u00A0"},
            "stageExcluded trims ASCII whitespace only and preserves U+3000 and U+00A0");
    Require(Settings::FromJson(LR"({"language":"-bad"})", Settings{}, parsed) && parsed.language == L"auto", "leading dash language resets to auto");
    Require(Settings::FromJson(LR"({"language":"en-"})", Settings{}, parsed) && parsed.language == L"auto", "trailing dash language resets to auto");
    Require(Settings::FromJson(LR"({"language":"en--us"})", Settings{}, parsed) && parsed.language == L"auto", "double dash language resets to auto");
    Require(Settings::FromJson(LR"({"language":""})", Settings{}, parsed) && parsed.language == L"auto", "empty language resets to auto");
    Require(Settings::FromJson(LR"({"language":"zh"})", Settings{}, parsed) && parsed.language == L"zh-Hans", "zh normalizes to zh-Hans");
    Require(Settings::FromJson(LR"({"language":"zh-Hant"})", Settings{}, parsed) && parsed.language == L"zh-Hant", "valid subtag survives");
    Settings tinted;
    tinted.tint = 0x112233;
    Require(Settings::FromJson(LR"({"tint":"# 12345"})", tinted, parsed) && parsed.tint == 0x112233, "whitespace-padded tint is rejected and retains fallback");
    Require(Settings::FromJson(LR"({"tint":"#GGGGGG"})", tinted, parsed) && parsed.tint == 0x112233, "non-hex tint is rejected and retains fallback");
    Require(Settings::FromJson(LR"({"tint":"#1a2B3c"})", tinted, parsed) && parsed.tint == 0x1A2B3C, "strict ASCII hex tint parses");

    Settings updateOff;
    updateOff.autoUpdate = false;
    Require(Settings{}.autoUpdate && Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.autoUpdate, "autoUpdate defaults to true");
    Require(Settings::FromJson(updateOff.ToJson(), Settings{}, parsed) && !parsed.autoUpdate, "autoUpdate false persists across round trip");
    Require(Settings::FromJson(LR"({"autoUpdate":"bad"})", updateOff, parsed) && !parsed.autoUpdate, "malformed autoUpdate retains current value");

    Settings taskStartup;
    taskStartup.startupMode = L"task";
    Require(Settings{}.startupMode == L"run" && Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.startupMode == L"run",
            "startupMode defaults to run");
    Require(Settings::FromJson(taskStartup.ToJson(), Settings{}, parsed) && parsed.startupMode == L"task", "startupMode task persists across round trip");
    Require(Settings::FromJson(LR"({"startupMode":"bogus"})", taskStartup, parsed) && parsed.startupMode == L"run", "invalid startupMode normalizes to run");

    Settings spacing;
    Require(Settings::FromJson(LR"({"iconSpacing":1})", Settings{}, parsed) && parsed.iconSpacing == 2, "iconSpacing clamps lower bound");
    Require(Settings::FromJson(LR"({"iconSpacing":99})", Settings{}, parsed) && parsed.iconSpacing == 40, "iconSpacing clamps upper bound");
    spacing.iconSpacing = 12.5;
    Require(Settings::FromJson(spacing.ToJson(), Settings{}, parsed) && parsed.iconSpacing == 12.5, "iconSpacing survives saving");
}

void CheckExtrasSettings()
{
    Settings defaults, parsed;
    Require(defaults.showLaunchpad && defaults.launchpadMaterial == L"bar" && defaults.launchpadLayout == L"window" && defaults.launchpadIcon.empty() &&
                defaults.showNowPlaying && defaults.nowPlayingShowPaused && defaults.nowPlayingMotion == L"normal" && defaults.nowPlayingAppMode == L"all" &&
                defaults.nowPlayingApps.empty() && !defaults.showFolderStacks && defaults.folderStackLayout == L"fan" && defaults.folderStackMax == 8 &&
                defaults.folderStacks.empty() && defaults.folderStackFiles.empty(),
            "extras settings have expected defaults");
    Require(Settings::FromJson(LR"({})", Settings{}, parsed) && parsed.showLaunchpad && parsed.launchpadIcon.empty() && parsed.showNowPlaying &&
                parsed.nowPlayingShowPaused && parsed.nowPlayingMotion == L"normal" && parsed.nowPlayingAppMode == L"all" && parsed.nowPlayingApps.empty() &&
                !parsed.showFolderStacks && parsed.folderStackLayout == L"fan" && parsed.folderStackMax == 8 && parsed.folderStacks.empty() &&
                parsed.folderStackFiles.empty(),
            "old settings receive extras defaults");

    Settings custom;
    custom.showLaunchpad = false;
    custom.launchpadMaterial = L"tint";
    custom.launchpadLayout = L"fullscreen";
    custom.launchpadIcon = L"C:\\Users\\Demo\\Pictures\\launchpad.png";
    custom.showNowPlaying = false;
    custom.nowPlayingShowPaused = false;
    custom.nowPlayingMotion = L"vivid";
    custom.nowPlayingAppMode = L"allow";
    custom.nowPlayingApps = {L"spotify.exe", L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic"};
    custom.showFolderStacks = true;
    custom.folderStackLayout = L"column";
    custom.folderStackMax = 24;
    custom.folderStacks = {L"C:\\Users\\Demo\\Desktop", L"D:\\Projects"};
    custom.folderStackFiles = {L"C:\\Users\\Demo\\Notes.pdf", L"D:\\Deck.pptx"};
    Require(Settings::FromJson(custom.ToJson(), Settings{}, parsed) && !parsed.showLaunchpad && parsed.launchpadMaterial == L"tint" &&
                parsed.launchpadLayout == L"fullscreen" && parsed.launchpadIcon == custom.launchpadIcon && !parsed.showNowPlaying &&
                !parsed.nowPlayingShowPaused && parsed.nowPlayingMotion == L"vivid" && parsed.nowPlayingAppMode == L"allow" &&
                parsed.nowPlayingApps == custom.nowPlayingApps && parsed.showFolderStacks && parsed.folderStackLayout == L"column" &&
                parsed.folderStackMax == 24 && parsed.folderStacks == custom.folderStacks && parsed.folderStackFiles == custom.folderStackFiles,
            "extras settings survive round trip");

    // The Launchpad icon: trimmed like the settings app trims it, and kept when a patch leaves it out.
    Require(Settings::FromJson(LR"({"launchpadIcon":" \tC:\\Icons\\orb.png\r\n "})", Settings{}, parsed) && parsed.launchpadIcon == L"C:\\Icons\\orb.png",
            "launchpadIcon trims ASCII whitespace");
    Require(Settings::FromJson(LR"({"launchpadIcon":""})", custom, parsed) && parsed.launchpadIcon.empty(),
            "launchpadIcon can be cleared back to the built-in mark");
    Require(Settings::FromJson(LR"({})", custom, parsed) && parsed.launchpadIcon == custom.launchpadIcon, "partial updates retain the Launchpad icon");
    Require(Settings::FromJson(LR"({"launchpadIcon":false})", custom, parsed) && parsed.launchpadIcon == custom.launchpadIcon,
            "a malformed launchpadIcon retains the fallback");

    Require(Settings::FromJson(
                LR"({"launchpadLayout":"bad","launchpadMaterial":"bad","nowPlayingMotion":"bad","nowPlayingAppMode":"bad","folderStackLayout":"bad"})", custom,
                parsed) &&
                parsed.launchpadMaterial == L"bar" && parsed.launchpadLayout == L"window" && parsed.nowPlayingMotion == L"normal" &&
                parsed.nowPlayingAppMode == L"all" && parsed.folderStackLayout == L"fan",
            "invalid extras enums normalize to defaults");
    Require(Settings::FromJson(LR"({"folderStackMax":1})", Settings{}, parsed) && parsed.folderStackMax == 4, "folderStackMax clamps lower bound");
    Require(Settings::FromJson(LR"({"folderStackMax":99})", Settings{}, parsed) && parsed.folderStackMax == 24, "folderStackMax clamps upper bound");
    Require(Settings::FromJson(LR"({"folderStackMax":"bad","showLaunchpad":"bad","nowPlayingApps":false,"folderStacks":123,"folderStackFiles":123})", custom,
                               parsed) &&
                parsed.folderStackMax == 24 && !parsed.showLaunchpad && parsed.nowPlayingApps == custom.nowPlayingApps &&
                parsed.folderStacks == custom.folderStacks && parsed.folderStackFiles == custom.folderStackFiles,
            "malformed extras keys retain fallback values");

    Settings many;
    many.nowPlayingApps.emplace_back(L"  \t\r\n ");
    for (int i = 0; i < 66; ++i)
        many.nowPlayingApps.emplace_back(L" app" + std::to_wstring(i) + L".exe ");
    many.folderStacks.emplace_back(L" \t\r\n\v\f ");
    many.folderStacks.emplace_back(L"\u3000C:\\Keep\u00A0");
    for (int i = 0; i < 35; ++i)
        many.folderStacks.emplace_back(L" D:\\Dir" + std::to_wstring(i) + L" ");
    many.folderStackFiles.emplace_back(L" \t\r\n\v\f ");
    for (int i = 0; i < 35; ++i)
        many.folderStackFiles.emplace_back(L" D:\\Doc" + std::to_wstring(i) + L".pdf ");
    many.Clamp();
    Require(many.nowPlayingApps.size() == 64 && many.nowPlayingApps.front() == L"app0.exe" && many.nowPlayingApps.back() == L"app63.exe",
            "nowPlayingApps trims, drops empty, and caps at 64");
    Require(many.folderStacks.size() == 32 && many.folderStacks.front() == L"\u3000C:\\Keep\u00A0" && many.folderStacks.back() == L"D:\\Dir30",
            "folderStacks trims ASCII whitespace only, drops empty, and caps at 32");
    Require(many.folderStackFiles.size() == 32 && many.folderStackFiles.front() == L"D:\\Doc0.pdf" && many.folderStackFiles.back() == L"D:\\Doc31.pdf",
            "folderStackFiles trims, drops empty, and caps at 32");

    Settings modules;
    Require(modules.controlModules == std::vector<std::wstring>{L"stage"}, "controlModules defaults to Stage Manager");
    modules.controlModules = {L" darkmode ", L"wifi", L"stage", L"dnd", L"darkmode", L"", L"airplane", L"hotspot"};
    modules.Clamp();
    Require(modules.controlModules == std::vector<std::wstring>{L"darkmode", L"stage", L"dnd", L"airplane", L"hotspot"},
            "controlModules keeps known modules once, in order");
    modules.controlModules = {L"nightlight", L"stage", L"darkmode"};
    modules.Clamp();
    Require(modules.controlModules == std::vector<std::wstring>{L"darkmode", L"stage"}, "controlModules: night light is dark mode now");
    Settings modulesParsed;
    Require(Settings::FromJson(modules.ToJson(), Settings{}, modulesParsed) && modulesParsed.controlModules == modules.controlModules &&
                Settings::FromJson(LR"({"controlModules":[]})", Settings{}, modulesParsed) && modulesParsed.controlModules.empty(),
            "controlModules round-trips, empty included");

    Settings deduped;
    deduped.folderStacks = {L"C:\\", L"c:\\\\", L" D:\\Work\\ ", L"d:/work", L"\\\\server\\share\\"};
    deduped.Clamp();
    Require(deduped.folderStacks == std::vector<std::wstring>{L"C:\\", L"D:\\Work", L"\\\\server\\share"},
            "folderStacks strips non-root trailing slashes and deduplicates case-insensitively");
}

void CheckDisplays()
{
    HMONITOR primary = MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    auto* external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    std::vector<DisplayInfo> displays{{primary, L"primary", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, true},
                                      {external, L"external", {-2560, -200, 0, 1240}, {-2560, -200, 0, 1200}, false}};
    Require(ResolveDisplay(displays, L"external") == external, "select an external display with negative coordinates");
    Require(ResolveDisplay(displays, L"unplugged") == primary, "disconnected display falls back to system primary");
    displays.pop_back();
    Require(ResolveDisplay(displays, L"external") == primary, "unplug falls back without changing the saved ID");
    Require(ResolveDisplay({}, L"external") == primary, "empty display list falls back to system primary");
    displays.push_back({external, L"external", {}, {}, false});
    Require(ResolveDisplay(displays, L"external") == external, "reconnection restores the selected display");
    auto actual = ConnectedDisplays();
    Require(!actual.empty(), "enumerate connected physical displays");
    for (const DisplayInfo& display : actual)
        Require(!display.id.empty() && ResolveDisplay(actual, display.id) == display.monitor, "enumerated device IDs resolve to their monitor");
}

void CheckTaskDistribution()
{
    auto primary = reinterpret_cast<HMONITOR>(INT_PTR(1)), external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    HWND first = reinterpret_cast<HWND>(INT_PTR(11)), second = reinterpret_cast<HWND>(INT_PTR(22));
    auto monitorOf = [&](HWND window) { return window == first ? primary : external; };
    TaskItem pinned, shared, other;
    pinned.key = L"pin";
    pinned.pinned = true;
    pinned.windows = {first};
    pinned.active = pinned.attention = true;
    pinned.badge = 5;
    shared.key = L"shared";
    shared.windows = {first, second};
    shared.active = true;
    other.key = L"external";
    other.windows = {second};
    std::vector<TaskItem> items{pinned, shared, other};
    auto all = TaskItemsForDisplay(items, L"all", false, external, first, monitorOf);
    Require(all.size() == 3 && all[1].windows.size() == 2, "all taskbars include all app windows");
    auto main = TaskItemsForDisplay(items, L"primaryAndWindow", true, primary, first, monitorOf);
    Require(main.size() == 3 && main[1].windows.size() == 2, "primary taskbar includes every app");
    auto local = TaskItemsForDisplay(items, L"primaryAndWindow", false, external, first, monitorOf);
    Require(local.size() == 3 && local[0].windows.empty() && !local[0].active && !local[0].attention && local[0].badge == 5,
            "remote pins stay without running or active indicators, keeping their badge");
    Require(local.size() >= 2 && local[1].windows == std::vector<HWND>{second} && !local[1].active, "window previews only include local windows");
    local = TaskItemsForDisplay(items, L"window", true, primary, first, monitorOf);
    Require(local.size() == 2 && local[1].windows == std::vector<HWND>{first} && local[1].active, "window-only mode also filters the primary taskbar");
    auto moved = TaskItemsForDisplay(items, L"window", false, external, second, [&](HWND) { return external; });
    Require(moved.size() == 3 && moved[0].windows.size() == 1 && moved[1].windows.size() == 2 && moved[1].active,
            "moving windows across screens updates membership and active state");
    Require(items[1].windows.size() == 2 && items[0].active, "per-display filtering leaves the shared model intact");
}

void CheckFullscreenCoverage()
{
    const auto primary = reinterpret_cast<HMONITOR>(INT_PTR(1)), external = reinterpret_cast<HMONITOR>(INT_PTR(2));
    const RECT screen{0, 0, 1920, 1080}, leftScreen{-2560, -200, 0, 1240};
    struct Window {
        HMONITOR monitor;
        RECT frame;
        bool maximized = false;
    };
    const Window fullscreen{primary, screen}, normal{primary, {100, 100, 900, 700}}, other{external, {-2400, 0, -1000, 900}};
    auto covered = [&](std::initializer_list<Window> windows) {
        return std::ranges::any_of(windows, [&](const Window& window) {
            return WindowCoverOnMonitor(window.monitor, window.frame, window.maximized, primary, screen).value_or(false);
        });
    };
    Require(covered({fullscreen}), "focused full-screen app hides its bar");
    Require(covered({other, fullscreen}), "focus on another monitor must not reveal the full-screen monitor's bar");
    Require(covered({{external, leftScreen}, fullscreen}), "each full-screen monitor hides its own bar independently");
    Require(covered({normal, fullscreen}), "a smaller window opened over a full-screen app keeps its bar hidden");
    Require(covered({other, normal, fullscreen}), "an unrelated foreground monitor does not reveal the bar either");
    Require(!covered({normal}), "normal windows alone keep the bar");
    Require(!covered({other}), "minimizing or closing the local full-screen app restores its bar");
    Require(covered({other, {primary, {0, 0, 1920, 1040}, true}}), "an inactive maximized local app keeps auto-hide");
    Require(covered({{primary, {}}, fullscreen}), "empty frames do not override a full-screen app");
    Require(WindowCoverOnMonitor(external, leftScreen, false, external, leftScreen) == true, "full-screen detection supports negative display coordinates");

    const RECT work{0, 0, 1920, 1032}, padded{-8, -8, 1928, 1040}, borderlessMax{-8, -8, 1928, 1088};
    Require(IsFullscreenWindowBounds(screen, screen, screen, work, WS_POPUP, false), "unmaximized fullscreen covers monitor");
    Require(IsFullscreenWindowBounds(borderlessMax, screen, screen, work, WS_POPUP | WS_MAXIMIZE, true),
            "Edge F11 fullscreen entered from maximized without caption still counts as fullscreen");
    Require(!IsFullscreenWindowBounds(screen, screen, screen, screen, WS_OVERLAPPEDWINDOW | WS_MAXIMIZE, true),
            "ordinary captioned maximized window with full work area is not fullscreen");
    Require(!IsFullscreenWindowBounds(padded, work, screen, work, WS_OVERLAPPEDWINDOW | WS_MAXIMIZE, true),
            "captioned maximized window bounded by work area is not fullscreen");
    Require(IsFullscreenWindowBounds(screen, screen, screen, work, WS_OVERLAPPEDWINDOW | WS_MAXIMIZE, true),
            "window extending past reduced work area with full extended frame is fullscreen");
    Require(!IsFullscreenWindowBounds(normal.frame, normal.frame, screen, work, WS_OVERLAPPEDWINDOW, false), "normal window does not cover monitor");

    const RECT left{0, 0, 960, 1080}, right{960, 0, 1920, 1080}, gapRight{965, 0, 1920, 1080};
    Require(IsTile(left, screen, 8) && IsTile({960, 540, 1920, 1080}, screen, 8), "halves and quarters are tiles");
    Require(!IsTile({100, 100, 900, 700}, screen, 8) && !IsTile({-500, 0, 960, 1080}, screen, 8), "floating or off-screen windows are not tiles");
    Require(TilesFill({left, right}, screen, 8) && TilesFill({left, gapRight}, screen, 8), "two snapped halves fill the screen, a seam allowed");
    Require(!TilesFill({left}, screen, 8) && !TilesFill({left, {1200, 0, 1920, 1080}}, screen, 8), "a half or a gap leaves the screen open");
    Require(TilesFill({left, {960, 0, 1920, 540}, {960, 540, 1920, 1080}}, screen, 8), "a half and two quarters fill the screen");
    Require(TilesFill({{-2560, -200, -1280, 1240}, {-1280, -200, 0, 1240}}, leftScreen, 8), "tiles support negative display coordinates");

    Require(AtDisplayEdge(BarEdge::Bottom, {960, 1079}, screen) && AtDisplayEdge(BarEdge::Bottom, {960, 1078}, screen),
            "the local bottom edge can still reveal the bar");
    Require(!AtDisplayEdge(BarEdge::Bottom, {960, 1080}, screen) && !AtDisplayEdge(BarEdge::Bottom, {960, 1600}, screen),
            "a lower display must not repeatedly reveal the upper display's bar");
    Require(!AtDisplayEdge(BarEdge::Bottom, {1920, 1079}, screen) && !AtDisplayEdge(BarEdge::Bottom, {960, 1077}, screen),
            "another display or a point above the edge does not reveal the bar");
    Require(AtDisplayEdge(BarEdge::Top, {960, 0}, screen) && AtDisplayEdge(BarEdge::Top, {960, 1}, screen) && !AtDisplayEdge(BarEdge::Top, {960, 2}, screen) &&
                !AtDisplayEdge(BarEdge::Top, {960, -1}, screen),
            "top edge reveal covers only its two-pixel strip inside the display");
    Require(AtDisplayEdge(BarEdge::Left, {0, 540}, screen) && AtDisplayEdge(BarEdge::Left, {1, 540}, screen) &&
                !AtDisplayEdge(BarEdge::Left, {2, 540}, screen) && !AtDisplayEdge(BarEdge::Left, {-1, 540}, screen),
            "left edge reveal covers only its two-pixel strip inside the display");
    Require(AtDisplayEdge(BarEdge::Right, {1919, 540}, screen) && AtDisplayEdge(BarEdge::Right, {1918, 540}, screen) &&
                !AtDisplayEdge(BarEdge::Right, {1917, 540}, screen) && !AtDisplayEdge(BarEdge::Right, {1920, 540}, screen),
            "right edge reveal covers only its two-pixel strip inside the display");
    Require(AtDisplayEdge(BarEdge::Bottom, {-1280, 1239}, leftScreen) && AtDisplayEdge(BarEdge::Left, {-2560, 500}, leftScreen) &&
                AtDisplayEdge(BarEdge::Right, {-1, 500}, leftScreen) && !AtDisplayEdge(BarEdge::Right, {0, 500}, leftScreen) &&
                AtDisplayEdge(BarEdge::Top, {-1280, -200}, leftScreen),
            "edge reveal supports negative display coordinates without spilling onto neighbours");

    // Follow-mode summon margins: 4px to arm, 16px hysteresis once armed.
    Require(FollowEdgeMargin(false) == 4 && FollowEdgeMargin(true) == 16, "follow edge hysteresis margins are 4px arm and 16px hold");
    Require(!AtDisplayEdge(BarEdge::Bottom, {960, 1077}, screen) && AtDisplayEdge(BarEdge::Bottom, {960, 1076}, screen, FollowEdgeMargin(false)) &&
                !AtDisplayEdge(BarEdge::Bottom, {960, 1075}, screen, FollowEdgeMargin(false)),
            "4px margin arms follow summon where 2px does not");
    Require(AtDisplayEdge(BarEdge::Bottom, {960, 1064}, screen, FollowEdgeMargin(true)) &&
                !AtDisplayEdge(BarEdge::Bottom, {960, 1063}, screen, FollowEdgeMargin(true)) &&
                !AtDisplayEdge(BarEdge::Bottom, {960, 1080}, screen, FollowEdgeMargin(true)),
            "16px hold margin tolerates pointer jitter inside the display without spilling outside");
    Require(AtDisplayEdge(BarEdge::Top, {960, 3}, screen, 4) && !AtDisplayEdge(BarEdge::Top, {960, 4}, screen, 4) &&
                AtDisplayEdge(BarEdge::Top, {960, 15}, screen, 16) && !AtDisplayEdge(BarEdge::Top, {960, 16}, screen, 16) &&
                AtDisplayEdge(BarEdge::Left, {3, 540}, screen, 4) && AtDisplayEdge(BarEdge::Left, {15, 540}, screen, 16) &&
                AtDisplayEdge(BarEdge::Right, {1916, 540}, screen, 4) && AtDisplayEdge(BarEdge::Right, {1904, 540}, screen, 16),
            "explicit edge margins work across all four screen edges");
}

void CheckTaskbarReshowGuard()
{
    TaskbarReshowGuard guard;
    Require(!guard.ShouldBackOff(1000), "fresh guard does not back off");
    for (ULONGLONG i = 0; i < 7; ++i)
        Require(!guard.Record(1000 + i * 200) && !guard.ShouldBackOff(1000 + i * 200), "seven rapid re-shows do not trip backoff");
    Require(guard.Record(2900) && guard.ShouldBackOff(2900), "eighth re-show within 2000ms trips backoff");
    Require(guard.ShouldBackOff(7899) && !guard.ShouldBackOff(7900), "backoff lasts 5000ms");
    Require(!guard.Record(3000) && !guard.ShouldBackOff(7900), "events during backoff do not extend the 5000ms window");
    for (ULONGLONG i = 0; i < 7; ++i)
        Require(!guard.Record(7900 + i * 100), "guard recovers cleanly after backoff expires");
    Require(guard.Record(8600) && guard.ShouldBackOff(8600), "eight more rapid re-shows after recovery trip backoff again");
    guard.Reset();
    Require(!guard.ShouldBackOff(8601), "explicit Reset clears active backoff immediately");
    for (ULONGLONG i = 0; i < 7; ++i)
        Require(!guard.Record(9000 + i * 350), "spread-out re-shows stay below threshold");
    Require(!guard.Record(12000) && !guard.ShouldBackOff(12000), "evicting events older than 2000ms avoids false trip");
}

void CheckDataDirArgument()
{
    std::wstring dir;
    bool specified = true;
    Require(ParseDataDirArgument({L"Atrium.exe", L"--development"}, dir, specified) && !specified && dir.empty(), "missing --data-dir leaves specified false");
    Require(ParseDataDirArgument({L"Atrium.exe", L"--data-dir", L"C:/temp/foo/../atrium-data/"}, dir, specified) && specified &&
                dir == L"C:\\temp\\atrium-data",
            "valid drive-rooted --data-dir normalizes");
    Require(ParseDataDirArgument({L"Atrium.exe", L"--data-dir", L"\\\\server\\share\\atrium\\"}, dir, specified) && specified &&
                dir == L"\\\\server\\share\\atrium",
            "valid UNC --data-dir normalizes");
    for (const auto& bad : std::vector<std::vector<std::wstring>>{
             {L"Atrium.exe", L"--data-dir"},
             {L"Atrium.exe", L"--data-dir", L""},
             {L"Atrium.exe", L"--data-dir", L"relative\\data"},
             {L"Atrium.exe", L"--data-dir", L"\\drive_relative"},
             {L"Atrium.exe", L"--data-dir", L"C:drive_relative"},
             {L"Atrium.exe", L"--data-dir", L"\\\\server"},
             {L"Atrium.exe", L"--data-dir", L"\\\\server\\"},
             {L"Atrium.exe", L"--data-dir", L"--development"},
         }) {
        Require(!ParseDataDirArgument(bad, dir, specified) && specified && dir.empty(), "invalid --data-dir is rejected");
    }
}

void CheckNotificationLayout()
{
    const RECT work{0, 0, 1920, 1040};
    auto single = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 1);
    auto burst = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 5);
    Require(single.rect.right == 1908 && single.rect.top == 12, "popup stays at the primary work area's top right");
    Require(EqualRect(&single.rect, &burst.rect), "new arrivals do not resize or move the popup viewport");
    Require(burst.contentHeight == 5 * 112 + 4 * 10, "all notifications have full-height slots separated by gaps");
    const float oldTop = notification::CardTop(0, 112, 10, 0), newTop = notification::CardTop(1, 112, 10, 0);
    const float moving = motion::Approach(oldTop, newTop, 16, 16);
    Require(oldTop == 0 && newTop == 122 && moving > oldTop && moving < newTop, "a new top notification makes existing cards animate down");
    Require(notification::CardTop(2, 112, 10, 122) == 122, "overflow scrolls older notifications upward into view");
    auto many = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 50);
    Require(many.contentHeight > many.viewHeight && many.rect.bottom <= work.bottom, "notification bursts stay inside the work area and can scroll");
    const RECT negative{-2560, -200, 0, 1200};
    auto scaled = notification::TopRightLayout(negative, 1.5f, 384, 112, 10, 12, 2);
    Require(scaled.rect.right == -18 && scaled.rect.top == -182 && scaled.rect.right - scaled.rect.left == 576,
            "top-right placement supports negative coordinates and monitor DPI");
    const RECT tiny{0, 40, 320, 220};
    auto narrow = notification::TopRightLayout(tiny, 2, 384, 112, 10, 12, 3);
    Require(narrow.rect.left >= tiny.left && narrow.rect.top >= tiny.top && narrow.rect.right <= tiny.right && narrow.rect.bottom <= tiny.bottom,
            "tiny or highly scaled work areas clamp the notification viewport");

    const float controls = 44 + 10; // existing centre capsule and card gap
    auto centre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 5, controls);
    Require(EqualRect(&centre.rect, &burst.rect), "the bell's notification centre uses the same top-right corner as popup notifications");
    Require(centre.contentTop == controls && centre.viewHeight + centre.contentTop == burst.viewHeight,
            "the centre reserves visible controls above its list without leaving the primary work area");
    Require(centre.contentHeight == burst.contentHeight && burst.contentTop == 0,
            "the notification centre shows every card while popup spacing stays unchanged");
    Require(centre.contentTop + notification::CardTop(0, 112, 10, 0) == controls && centre.contentTop + notification::CardTop(1, 112, 10, 0) == controls + 122,
            "the centre's newest notification starts below its controls and older notifications follow downward");
    auto emptyCentre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 0, controls);
    Require(EqualRect(&emptyCentre.rect, &centre.rect) && emptyCentre.contentHeight == 0 && emptyCentre.contentTop == controls,
            "an empty notification centre keeps its controls at the top-right corner");
    auto longCentre = notification::TopRightLayout(work, 1, 384, 112, 10, 12, 50, controls);
    Require(longCentre.contentHeight > longCentre.viewHeight && longCentre.rect.bottom <= work.bottom,
            "long centre lists scroll below the fixed top-right controls");
    auto scaledCentre = notification::TopRightLayout(negative, 1.5f, 384, 112, 10, 12, 2, controls);
    Require(EqualRect(&scaledCentre.rect, &scaled.rect) && scaledCentre.contentTop == controls,
            "the notification centre shares the popup's negative-coordinate and DPI placement");

    auto topLeft = notification::CornerLayout(work, 1, 384, 112, 10, 12, 1, 0, true, false);
    Require(topLeft.rect.left == 12 && topLeft.rect.top == 12 && topLeft.rect.right == 396, "the top left corner mirrors the top right one");
    auto bottomRight = notification::CornerLayout(work, 1, 384, 112, 10, 12, 5, controls, false, true);
    Require(EqualRect(&bottomRight.rect, &burst.rect) && bottomRight.contentTop == 0 && bottomRight.viewHeight == centre.viewHeight,
            "a bottom corner's centre keeps its controls below the cards, in the same room");
    auto bottomLeft = notification::CornerLayout(negative, 1.5f, 384, 112, 10, 12, 2, 0, true, true);
    Require(bottomLeft.rect.left == -2542 && bottomLeft.rect.bottom == 1182 && bottomLeft.rect.right - bottomLeft.rect.left == 576,
            "the bottom left corner supports negative coordinates and monitor DPI");
}

void CheckNotificationQuietState()
{
    const QUERY_USER_NOTIFICATION_STATE states[] = {
        QUNS_ACCEPTS_NOTIFICATIONS, QUNS_APP, QUNS_BUSY, QUNS_RUNNING_D3D_FULL_SCREEN, QUNS_PRESENTATION_MODE, QUNS_NOT_PRESENT, QUNS_QUIET_TIME,
    };
    for (QUERY_USER_NOTIFICATION_STATE s : states) {
        for (int mask = 0; mask < 16; ++mask) {
            const bool ours = (mask & 1) != 0, fs = (mask & 2) != 0, opt = (mask & 4) != 0, game = (mask & 8) != 0;
            const bool allowFs = !fs || (opt && !game);
            const bool expected = (s == QUNS_QUIET_TIME && ours) || ((s == QUNS_ACCEPTS_NOTIFICATIONS || s == QUNS_APP || s == QUNS_BUSY) && allowFs);
            Require(notification::AcceptsNotificationPopup(s, ours, fs, opt, game) == expected, "notification quiet-state decision matches rule");
        }
    }
}

std::wstring Utf8ToWide(std::string_view bytes)
{
    if (bytes.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), wide.data(), n);
    return wide;
}

std::string WideToUtf8(std::wstring_view wide)
{
    if (wide.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string bytes(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), bytes.data(), n, nullptr, nullptr);
    return bytes;
}

void CheckNotificationWithdrawAndMerge()
{
    Settings defaults;
    Require(!defaults.notifyAlways && defaults.notifyTimeout == 6, "notifyAlways defaults to false");
    Settings always;
    always.notifyAlways = true;
    always.notifyTimeout = 15;
    Settings parsed;
    Require(Settings::FromJson(always.ToJson(), Settings{}, parsed) && parsed.notifyAlways && parsed.notifyTimeout == 15,
            "notifyAlways and notifyTimeout survive round trip");
    Require(Settings::FromJson(LR"({})", always, parsed) && parsed.notifyAlways, "partial update keeps notifyAlways");
    Require(Settings::FromJson(LR"({"notifyAlways":"bad"})", always, parsed) && parsed.notifyAlways, "malformed notifyAlways keeps current value");

    Notice merged;
    merged.source = L"windows";
    merged.title = L"Alice";
    notification::AddExternalId(merged, 101);
    notification::AddExternalId(merged, 102);
    Require(notification::HasExternalId(merged, 101) && notification::HasExternalId(merged, 102) && !notification::HasExternalId(merged, 103),
            "merged card tracks every externalId");

    Notice otherSource{0, L"wechat", L"", L"", L"Alice", L"", L"", 101, {}, 0, 1, false};
    Require(!notification::WithdrawNotice(merged, otherSource), "withdraw from different source does not match");

    Notice w101{0, L"windows", L"", L"", L"", L"", L"", 101, {}, 0, 1, false};
    Require(!notification::WithdrawNotice(merged, w101), "withdrawing one merged toast keeps the card");
    Require(!notification::HasExternalId(merged, 101) && notification::HasExternalId(merged, 102), "withdrawn id is removed while remaining id stays");

    Notice w102{0, L"windows", L"", L"", L"", L"", L"", 102, {}, 0, 1, false};
    Require(notification::WithdrawNotice(merged, w102), "withdrawing the last merged toast removes the card");

    Notice wechat{0, L"wechat", L"", L"", L"Bob", L"", L"", 0, {}, 0, 1, true};
    Notice wechatWithdraw{0, L"wechat", L"", L"", L"Bob", L"", L"", 0, {}, 0, 1, false};
    Require(notification::WithdrawNotice(wechat, wechatWithdraw), "title-based withdraw still removes matching card");
}

bool RunNormalizer()
{
    namespace json = winrt::Windows::Data::Json;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        json::JsonObject obj{nullptr};
        if (!Require(json::JsonObject::TryParse(Utf8ToWide(line), obj), "valid ndjson line"))
            return false;
        Settings base;
        if (obj.HasKey(L"base") && obj.GetNamedValue(L"base").ValueType() == json::JsonValueType::Object) {
            if (!Require(Settings::FromJson(std::wstring(obj.GetNamedObject(L"base").Stringify()), Settings{}, base), "valid base"))
                return false;
        }
        std::wstring patchText;
        if (obj.HasKey(L"patchText") && obj.GetNamedValue(L"patchText").ValueType() == json::JsonValueType::String) {
            patchText = std::wstring(obj.GetNamedString(L"patchText"));
        } else {
            if (!Require(obj.HasKey(L"patch") && obj.GetNamedValue(L"patch").ValueType() == json::JsonValueType::Object, "patch object required"))
                return false;
            patchText = std::wstring(obj.GetNamedObject(L"patch").Stringify());
        }
        Settings out;
        if (Settings::FromJson(patchText, base, out))
            std::cout << R"({"ok":true,"result":)" << WideToUtf8(out.ToJson()) << "}\n";
        else
            std::cout << R"({"ok":false})" << '\n';
        std::cout.flush();
    }
    return true;
}

StackWindow MakeStackWin(uintptr_t id, const wchar_t* cls, RECT r, bool caption = true, bool topmost = false)
{
    return StackWindow{reinterpret_cast<HWND>(id), cls, r, true, false, false, topmost, false, false, false, false, caption};
}

void CheckWidgetStacking()
{
    const RECT mon1{0, 0, 2560, 1440}, mon2{2560, 0, 6400, 2160}, both{0, 0, 6400, 2160};
    const StackWindow bar = MakeStackWin(1, L"AtriumBar", mon1, false, true), progman = MakeStackWin(2, L"Progman", both, false);
    const StackWindow bambu = MakeStackWin(3, L"BambuStudio", mon1), aux = MakeStackWin(4, L"WorkerW", mon1, false);
    StackWindow defViewW = MakeStackWin(5, L"WorkerW", both, false);
    defViewW.hasDefView = true;
    StackWindow menu = MakeStackWin(6, L"AtriumMenu", mon2, false), widget = MakeStackWin(7, L"AtriumWidget", mon1, false);
    menu.ours = widget.ours = widget.widget = true;

    Require(ComputeWidgetStackPlan({bar, bambu, progman}, mon1, bambu.hwnd, false, false).after == bambu.hwnd, "normal: below app");
    auto p = ComputeWidgetStackPlan({bar, aux, bambu, progman}, mon1, progman.hwnd, false, false);
    Require(p.after == bambu.hwnd && !p.showDesktop, "#145: helper WorkerW + desktop fg stays below app");
    Require(ComputeWidgetStackPlan({bar, aux, bambu, progman}, mon1, menu.hwnd, true, false).after == bambu.hwnd, "#145: helper WorkerW + ours fg");
    const StackWindow borderless = MakeStackWin(8, L"Borderless", RECT{100, 100, 300, 300}, false);
    Require(ComputeWidgetStackPlan({bar, borderless, progman}, mon1, progman.hwnd, false, false).after == borderless.hwnd, "captionless >=64x64");
    StackWindow tiny = MakeStackWin(9, L"Tiny", RECT{10, 10, 42, 42}, false);
    Require(ComputeWidgetStackPlan({bar, tiny, progman}, mon1, progman.hwnd, false, false).lowestApp == nullptr, "tiny untitled ignored");
    tiny.hasTitle = true;
    Require(ComputeWidgetStackPlan({bar, tiny, progman}, mon1, progman.hwnd, false, false).after == tiny.hwnd, "tiny titled counted");
    p = ComputeWidgetStackPlan({bar, progman, bambu}, mon1, progman.hwnd, false, false);
    Require(p.showDesktop && p.after == HWND_TOP, "Win+D Progman fg lifts on smaller monitor");
    p = ComputeWidgetStackPlan({bar, defViewW, bambu}, mon1, defViewW.hwnd, false, false);
    Require(p.showDesktop && p.after == HWND_TOP, "Win+D DefView WorkerW fg lifts on smaller monitor");
    p = ComputeWidgetStackPlan({bar, progman, bambu}, mon1, widget.hwnd, true, true);
    Require(p.showDesktop && p.after == HWND_TOP, "wasShowDesktop kept while fg is ours");
    Require(!ComputeWidgetStackPlan({bar, progman, bambu}, mon1, bambu.hwnd, false, true).showDesktop, "wasShowDesktop exits on app");
    Require(!ComputeWidgetStackPlan({bar, progman, bambu}, mon1, menu.hwnd, true, false).showDesktop, "Explorer restart race");
    Require(ComputeWidgetStackPlan({bar, progman}, mon1, progman.hwnd, false, false).after == HWND_TOP, "only topmost above desktop");
    Require(ComputeWidgetStackPlan({bar, bambu}, mon1, bambu.hwnd, false, false).after == bambu.hwnd, "no host with app");
    Require(ComputeWidgetStackPlan({bar, widget}, mon1, nullptr, false, false).after == HWND_BOTTOM, "no host no app -> HWND_BOTTOM");
    const StackWindow appMon2 = MakeStackWin(10, L"OtherMonApp", mon2);
    Require(!ComputeWidgetStackPlan({bar, appMon2, progman}, mon1, appMon2.hwnd, false, false).lowestApp, "two monitors independent");
}
} // namespace

int main(int argc, char** argv)
{
    winrt::init_apartment();
    if (std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string_view(arg) == "--normalize"; }))
        return RunNormalizer() ? 0 : 1;
    CheckSettings();
    CheckStageDragMerge();
    CheckSettingsNormalization();
    CheckExtrasSettings();
    CheckDataDirArgument();
    CheckDisplays();
    CheckTaskDistribution();
    CheckFullscreenCoverage();
    CheckTaskbarReshowGuard();
    CheckNotificationLayout();
    CheckNotificationQuietState();
    CheckWidgetStacking();
    CheckNotificationWithdrawAndMerge();
    if (g_failed)
        return 1;
    std::cout << "Display, fullscreen auto-hide, notification placement and animated reflow checks passed.\n";
    return 0;
}
