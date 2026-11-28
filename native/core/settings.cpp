#include "settings.h"

#include "localization_format.h"
#include "win32_path.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

void ReadStringArrayJson(const json::JsonObject& o, const wchar_t* key, std::vector<std::wstring>& target)
{
    if (o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::Array) {
        std::vector<std::wstring> items;
        for (const auto& v : o.GetNamedArray(key))
            if (v.ValueType() == json::JsonValueType::String)
                items.emplace_back(v.GetString());
        target = std::move(items);
    }
}

void ReadBarSettingsJson(const json::JsonObject& o, Settings& s)
{
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    if (o.HasKey(L"preset") && o.GetNamedValue(L"preset").ValueType() == json::JsonValueType::String)
        s.preset = std::wstring(o.GetNamedString(L"preset"));
    num(L"transparency", s.transparency);
    num(L"cornerRadius", s.cornerRadius);
    num(L"blur", s.blur);
    num(L"refraction", s.refraction);
    num(L"edge", s.edge);
    num(L"dispersion", s.dispersion);
    num(L"highlight", s.highlight);
    num(L"shadow", s.shadow);
    num(L"width", s.width);
    num(L"height", s.height);
    num(L"margin", s.margin);
    if (o.HasKey(L"tint") && o.GetNamedValue(L"tint").ValueType() == json::JsonValueType::String) {
        std::wstring t(o.GetNamedString(L"tint"));
        auto hex = [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); };
        if (t.size() == 7 && t[0] == L'#' && std::all_of(t.begin() + 1, t.end(), hex))
            s.tint = static_cast<uint32_t>(wcstoul(t.c_str() + 1, nullptr, 16));
    }
    num(L"iconSize", s.iconSize);
    num(L"iconSpacing", s.iconSpacing);
    num(L"magnify", s.magnify);
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    flag(L"hoverRise", s.hoverRise);
    flag(L"riseNear", s.riseNear);
    flag(L"magnifyNear", s.magnifyNear);
    flag(L"startTray", s.startTray);
    flag(L"startStatus", s.startStatus);
    flag(L"startCentered", s.startCentered);
    flag(L"startWithBar", s.startWithBar);
    if (o.HasKey(L"startSize") && o.GetNamedValue(L"startSize").ValueType() == json::JsonValueType::String)
        s.startSize = std::wstring(o.GetNamedString(L"startSize"));
    if (o.HasKey(L"startIcon") && o.GetNamedValue(L"startIcon").ValueType() == json::JsonValueType::String)
        s.startIcon = std::wstring(o.GetNamedString(L"startIcon"));
    flag(L"showClock", s.showClock);
    flag(L"showClockSeconds", s.showClockSeconds);
    flag(L"showTray", s.showTray);
    flag(L"showStatus", s.showStatus);
    flag(L"showNotifications", s.showNotifications);
    flag(L"showDesktop", s.showDesktop);
    flag(L"autoWidth", s.autoWidth);
    flag(L"showRunning", s.showRunning);
    num(L"notifyTimeout", s.notifyTimeout);
    flag(L"notifyAlways", s.notifyAlways);
    num(L"nearEnter", s.nearEnter);
    num(L"nearLeave", s.nearLeave);
    flag(L"notifyHideBody", s.notifyHideBody);
    flag(L"notifyFullscreen", s.notifyFullscreen);
    flag(L"imeStatus", s.imeStatus);
    flag(L"imeBubble", s.imeBubble);
}

void ReadModuleSettingsJson(const json::JsonObject& o, Settings& s)
{
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    // Version 1 saved the old default (off) along with everything else; from
    // version 2 the notifications are on unless turned off since.
    bool v1 = o.HasKey(L"version") && o.GetNamedValue(L"version").ValueType() == json::JsonValueType::Number && o.GetNamedNumber(L"version") < 2;
    if (!v1)
        flag(L"notifyListener", s.notifyListener);
    flag(L"notifyWeChat", s.notifyWeChat);
    flag(L"wechatBlockOfficial", s.wechatBlockOfficial);
    flag(L"notifyFeishu", s.notifyFeishu);
    flag(L"notifyQQ", s.notifyQQ);
    flag(L"notifyWeCom", s.notifyWeCom);
    flag(L"notifyDingTalk", s.notifyDingTalk);
    flag(L"attentionLight", s.attentionLight);
    flag(L"attentionBounce", s.attentionBounce);
    flag(L"barBadges", s.barBadges);
    flag(L"bar", s.bar);
    flag(L"widgets", s.widgets);
    flag(L"widgetsOnRight", s.widgetsOnRight);
    flag(L"stageManager", s.stageManager);
    flag(L"stageOnRight", s.stageOnRight);
    flag(L"stageAutoHide", s.stageAutoHide);
    flag(L"stageMinimize", s.stageMinimize);
    flag(L"stageDragMerge", s.stageDragMerge);
    flag(L"stageAvoid", s.stageAvoid);
    flag(L"stageCloseButton", s.stageCloseButton);
    flag(L"stageScrollWrap", s.stageScrollWrap);
    flag(L"hideTaskbar", s.hideTaskbar);
    flag(L"genieMinimize", s.genieMinimize);
    flag(L"windowPeek", s.windowPeek);
    flag(L"barDodge", s.barDodge);
    flag(L"barReserve", s.barReserve);
    flag(L"barFullscreenHide", s.barFullscreenHide);
    flag(L"hideBanners", s.hideBanners);
    flag(L"trayHost", s.trayHost);
    ReadStringArrayJson(o, L"stageExcluded", s.stageExcluded);
    flag(L"remoteVisible", s.remoteVisible);
    flag(L"winKeyStart", s.winKeyStart);
    flag(L"winNumbers", s.winNumbers);
    flag(L"startOnLogin", s.startOnLogin);
    flag(L"autoUpdate", s.autoUpdate);
}

void ReadPinsJson(const json::JsonObject& o, Settings& s)
{
    if (o.HasKey(L"pins") && o.GetNamedValue(L"pins").ValueType() == json::JsonValueType::Array) {
        std::vector<PinnedApp> pins;
        for (const auto& v : o.GetNamedArray(L"pins")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            auto e = v.GetObject();
            auto str = [&](const wchar_t* k) {
                return e.HasKey(k) && e.GetNamedValue(k).ValueType() == json::JsonValueType::String ? std::wstring(e.GetNamedString(k)) : std::wstring();
            };
            PinnedApp p{str(L"name"), str(L"path"), str(L"args"), str(L"aumid")};
            if (e.HasKey(L"multipleWindows") && e.GetNamedValue(L"multipleWindows").ValueType() == json::JsonValueType::Boolean)
                p.multipleWindows = e.GetNamedBoolean(L"multipleWindows");
            if (!p.path.empty() || !p.aumid.empty())
                pins.push_back(std::move(p));
        }
        s.pins = std::move(pins);
    }
}

void ReadExtrasSettingsJson(const json::JsonObject& o, Settings& s)
{
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    auto str = [&](const wchar_t* k, std::wstring& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::String)
            v = std::wstring(o.GetNamedString(k));
    };
    flag(L"showLaunchpad", s.showLaunchpad);
    str(L"launchpadMaterial", s.launchpadMaterial);
    str(L"launchpadLayout", s.launchpadLayout);
    str(L"launchpadIcon", s.launchpadIcon);
    flag(L"showNowPlaying", s.showNowPlaying);
    flag(L"nowPlayingShowPaused", s.nowPlayingShowPaused);
    str(L"nowPlayingMotion", s.nowPlayingMotion);
    str(L"nowPlayingRhythm", s.nowPlayingRhythm);
    str(L"nowPlayingAppMode", s.nowPlayingAppMode);
    ReadStringArrayJson(o, L"nowPlayingApps", s.nowPlayingApps);
    flag(L"showFolderStacks", s.showFolderStacks);
    str(L"folderStackLayout", s.folderStackLayout);
    if (o.HasKey(L"folderStackMax") && o.GetNamedValue(L"folderStackMax").ValueType() == json::JsonValueType::Number) {
        const double raw = o.GetNamedNumber(L"folderStackMax");
        if (raw == raw)
            s.folderStackMax = static_cast<int>(std::lround(std::clamp(raw, 4.0, 24.0)));
    }
    ReadStringArrayJson(o, L"folderStacks", s.folderStacks);
    ReadStringArrayJson(o, L"folderStackFiles", s.folderStackFiles);
}

void NormalizeStringList(std::vector<std::wstring>& list, size_t maxCount = SIZE_MAX)
{
    constexpr std::wstring_view kAsciiWs = L" \t\r\n\v\f";
    for (auto& entry : list) {
        const size_t first = entry.find_first_not_of(kAsciiWs);
        if (first == std::wstring::npos) {
            entry.clear();
            continue;
        }
        entry = entry.substr(first, entry.find_last_not_of(kAsciiWs) - first + 1);
    }
    std::erase_if(list, [](const std::wstring& entry) { return entry.empty(); });
    if (list.size() > maxCount)
        list.resize(maxCount);
}

// Known modules only, each once, in the order given. The night light module that was (“夜间模式”, read as a
// dark theme) is dark mode now.
void NormalizeControlModules(std::vector<std::wstring>& list)
{
    NormalizeStringList(list);
    std::vector<std::wstring> kept;
    for (std::wstring& id : list)
        if (id == L"nightlight")
            id = L"darkmode";
    for (std::wstring& id : list)
        if (std::ranges::any_of(kControlModules, [&](const wchar_t* m) { return id == m; }) && std::ranges::find(kept, id) == kept.end())
            kept.push_back(std::move(id));
    list = std::move(kept);
}

bool IsFolderSlash(wchar_t ch) { return ch == L'\\' || ch == L'/'; }

bool IsFolderRootSlash(const std::wstring& path)
{
    if (path.size() == 1)
        return IsFolderSlash(path[0]);
    if (path.size() == 2)
        return path[0] == L'\\' && path[1] == L'\\';
    if (path.size() == 3)
        return std::iswalpha(static_cast<wint_t>(path[0])) != 0 && path[1] == L':' && IsFolderSlash(path[2]);
    if (path.size() == 7 && (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")))
        return std::iswalpha(static_cast<wint_t>(path[4])) != 0 && path[5] == L':' && IsFolderSlash(path[6]);
    return false;
}

std::wstring FolderDedupeKey(std::wstring path)
{
    for (wchar_t& ch : path) {
        if (ch == L'/')
            ch = L'\\';
    }
    return FoldCase(path);
}

void NormalizeFolderStacks(std::vector<std::wstring>& list, size_t maxCount, bool stripTrailingSlashes = true)
{
    NormalizeStringList(list);
    std::vector<std::wstring> deduped;
    deduped.reserve(std::min(list.size(), maxCount));
    std::unordered_set<std::wstring> seen;
    for (auto& entry : list) {
        if (stripTrailingSlashes)
            while (!entry.empty() && IsFolderSlash(entry.back()) && !IsFolderRootSlash(entry))
                entry.pop_back();
        if (entry.empty())
            continue;
        if (seen.insert(FolderDedupeKey(entry)).second) {
            deduped.push_back(std::move(entry));
            if (deduped.size() >= maxCount)
                break;
        }
    }
    list = std::move(deduped);
}

void WriteStringArrayJson(const json::JsonObject& o, const wchar_t* key, const std::vector<std::wstring>& list)
{
    json::JsonArray a;
    for (const std::wstring& item : list)
        a.Append(json::JsonValue::CreateStringValue(item));
    o.SetNamedValue(key, a);
}

void WriteBarJson(const json::JsonObject& o, const Settings& s)
{
    auto num = [&](const wchar_t* k, double v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(v)); };
    auto flag = [&](const wchar_t* k, bool v) { o.SetNamedValue(k, json::JsonValue::CreateBooleanValue(v)); };
    auto str = [&](const wchar_t* k, const std::wstring& v) { o.SetNamedValue(k, json::JsonValue::CreateStringValue(v)); };
    wchar_t tintText[8];
    swprintf_s(tintText, L"#%06X", s.tint);
    num(L"version", Settings::kVersion);
    str(L"preset", s.preset);
    num(L"transparency", s.transparency);
    num(L"cornerRadius", s.cornerRadius);
    num(L"blur", s.blur);
    num(L"refraction", s.refraction);
    num(L"edge", s.edge);
    num(L"dispersion", s.dispersion);
    num(L"highlight", s.highlight);
    num(L"shadow", s.shadow);
    str(L"tint", tintText);
    num(L"width", s.width);
    num(L"height", s.height);
    num(L"margin", s.margin);
    num(L"iconSize", s.iconSize);
    num(L"iconSpacing", s.iconSpacing);
    num(L"magnify", s.magnify);
    flag(L"hoverRise", s.hoverRise);
    flag(L"riseNear", s.riseNear);
    flag(L"magnifyNear", s.magnifyNear);
    flag(L"startTray", s.startTray);
    flag(L"startStatus", s.startStatus);
    flag(L"startCentered", s.startCentered);
    flag(L"startWithBar", s.startWithBar);
    if (!s.startSize.empty())
        str(L"startSize", s.startSize);
    if (!s.startIcon.empty())
        str(L"startIcon", s.startIcon);
    flag(L"showClock", s.showClock);
    flag(L"showClockSeconds", s.showClockSeconds);
    flag(L"showTray", s.showTray);
    flag(L"showStatus", s.showStatus);
    flag(L"showNotifications", s.showNotifications);
    flag(L"showDesktop", s.showDesktop);
    flag(L"autoWidth", s.autoWidth);
    flag(L"showRunning", s.showRunning);
}

void WriteExtrasJson(const json::JsonObject& o, const Settings& s)
{
    auto flag = [&](const wchar_t* k, bool v) { o.SetNamedValue(k, json::JsonValue::CreateBooleanValue(v)); };
    auto str = [&](const wchar_t* k, const std::wstring& v) { o.SetNamedValue(k, json::JsonValue::CreateStringValue(v)); };
    flag(L"showLaunchpad", s.showLaunchpad);
    str(L"launchpadMaterial", s.launchpadMaterial);
    str(L"launchpadLayout", s.launchpadLayout);
    str(L"launchpadIcon", s.launchpadIcon);
    flag(L"showNowPlaying", s.showNowPlaying);
    flag(L"nowPlayingShowPaused", s.nowPlayingShowPaused);
    str(L"nowPlayingMotion", s.nowPlayingMotion);
    str(L"nowPlayingRhythm", s.nowPlayingRhythm);
    str(L"nowPlayingAppMode", s.nowPlayingAppMode);
    WriteStringArrayJson(o, L"nowPlayingApps", s.nowPlayingApps);
    flag(L"showFolderStacks", s.showFolderStacks);
    str(L"folderStackLayout", s.folderStackLayout);
    o.SetNamedValue(L"folderStackMax", json::JsonValue::CreateNumberValue(s.folderStackMax));
    WriteStringArrayJson(o, L"folderStacks", s.folderStacks);
    WriteStringArrayJson(o, L"folderStackFiles", s.folderStackFiles);
}

void WriteSystemJson(const json::JsonObject& o, const Settings& s)
{
    auto num = [&](const wchar_t* k, double v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(v)); };
    auto flag = [&](const wchar_t* k, bool v) { o.SetNamedValue(k, json::JsonValue::CreateBooleanValue(v)); };
    auto str = [&](const wchar_t* k, const std::wstring& v) { o.SetNamedValue(k, json::JsonValue::CreateStringValue(v)); };
    str(L"language", s.language);
    str(L"textColor", s.textColor);
    str(L"autoHide", s.autoHide);
    str(L"barEdge", s.barEdge);
    str(L"statusStyle", s.statusStyle);
    WriteStringArrayJson(o, L"controlModules", s.controlModules);
    str(L"dockDisplayMode", s.dockDisplayMode);
    num(L"dockDisplayDelay", s.dockDisplayDelay);
    str(L"dockDisplay", s.dockDisplay);
    str(L"desktopDisplay", s.desktopDisplay);
    str(L"taskbarApps", s.taskbarApps);
    flag(L"taskbarRightPrimaryOnly", s.taskbarRightPrimaryOnly);
    flag(L"windowsContextMenu", s.windowsContextMenu);
    flag(L"clickWindowPicker", s.clickWindowPicker);
    num(L"notifyTimeout", s.notifyTimeout);
    flag(L"notifyAlways", s.notifyAlways);
    num(L"nearEnter", s.nearEnter);
    num(L"nearLeave", s.nearLeave);
    str(L"notifyPlacement", s.notifyPlacement);
    flag(L"notifyHideBody", s.notifyHideBody);
    flag(L"notifyFullscreen", s.notifyFullscreen);
    flag(L"imeStatus", s.imeStatus);
    flag(L"imeBubble", s.imeBubble);
    flag(L"notifyListener", s.notifyListener);
    flag(L"notifyWeChat", s.notifyWeChat);
    flag(L"wechatBlockOfficial", s.wechatBlockOfficial);
    flag(L"notifyFeishu", s.notifyFeishu);
    flag(L"notifyQQ", s.notifyQQ);
    flag(L"notifyWeCom", s.notifyWeCom);
    flag(L"notifyDingTalk", s.notifyDingTalk);
    flag(L"attentionLight", s.attentionLight);
    flag(L"attentionBounce", s.attentionBounce);
    flag(L"barBadges", s.barBadges);
    flag(L"bar", s.bar);
    flag(L"widgets", s.widgets);
    flag(L"widgetsOnRight", s.widgetsOnRight);
    str(L"widgetBackdrop", s.widgetBackdrop);
    str(L"widgetMaterial", s.widgetMaterial);
    flag(L"stageManager", s.stageManager);
    flag(L"stageOnRight", s.stageOnRight);
    flag(L"stageAutoHide", s.stageAutoHide);
    flag(L"stageMinimize", s.stageMinimize);
    flag(L"stageDragMerge", s.stageDragMerge);
    flag(L"stageAvoid", s.stageAvoid);
    flag(L"stageCloseButton", s.stageCloseButton);
    flag(L"stageScrollWrap", s.stageScrollWrap);
    WriteStringArrayJson(o, L"stageExcluded", s.stageExcluded);
    flag(L"hideTaskbar", s.hideTaskbar);
    flag(L"genieMinimize", s.genieMinimize);
    flag(L"windowPeek", s.windowPeek);
    flag(L"barDodge", s.barDodge);
    flag(L"barReserve", s.barReserve);
    flag(L"barFullscreenHide", s.barFullscreenHide);
    flag(L"hideBanners", s.hideBanners);
    flag(L"trayHost", s.trayHost);
    flag(L"remoteVisible", s.remoteVisible);
    flag(L"winKeyStart", s.winKeyStart);
    flag(L"winNumbers", s.winNumbers);
    flag(L"startOnLogin", s.startOnLogin);
    str(L"startupMode", s.startupMode);
    flag(L"autoUpdate", s.autoUpdate);
}

void WritePinsJson(const json::JsonObject& o, const std::optional<std::vector<PinnedApp>>& pins)
{
    if (!pins)
        return;
    json::JsonArray a;
    for (const auto& p : *pins) {
        json::JsonObject e;
        e.SetNamedValue(L"name", json::JsonValue::CreateStringValue(p.name));
        e.SetNamedValue(L"path", json::JsonValue::CreateStringValue(p.path));
        e.SetNamedValue(L"args", json::JsonValue::CreateStringValue(p.args));
        e.SetNamedValue(L"aumid", json::JsonValue::CreateStringValue(p.aumid));
        if (p.multipleWindows)
            e.SetNamedValue(L"multipleWindows", json::JsonValue::CreateBooleanValue(*p.multipleWindows));
        a.Append(e);
    }
    o.SetNamedValue(L"pins", a);
}

} // namespace

void Settings::Clamp()
{
    auto c = [](double& v, double lo, double hi, double fallback) {
        v = v != v ? fallback : std::clamp(v, lo, hi); // NaN -> fallback
    };
    Settings d;
    c(transparency, 0, 1, d.transparency);
    c(cornerRadius, 0, 200, d.cornerRadius);
    c(blur, 0, 1, d.blur);
    c(refraction, 0, 60, d.refraction);
    c(edge, 1, 80, d.edge);
    c(dispersion, 0, 0.5, d.dispersion);
    c(highlight, 0, 1, d.highlight);
    c(shadow, 0, 1, d.shadow);
    c(width, 200, 4000, d.width);
    c(height, 32, 160, d.height);
    c(margin, 0, 400, d.margin);
    c(iconSize, 16, 64, d.iconSize);
    c(iconSpacing, 2, 40, d.iconSpacing);
    c(magnify, 1, 2, d.magnify);
    c(notifyTimeout, 3, 30, d.notifyTimeout);
    c(nearEnter, 2, 160, d.nearEnter);
    c(nearLeave, 2, 200, d.nearLeave);
    c(dockDisplayDelay, 100, 2000, d.dockDisplayDelay);
    tint &= 0xFFFFFF;
    if (!localization::ValidTag(language))
        language = L"auto";
    else if (language == L"zh")
        language = L"zh-Hans";
    if (textColor != L"light" && textColor != L"dark")
        textColor = L"auto";
    if (autoHide != L"never" && autoHide != L"always")
        autoHide = L"fullscreen";
    if (barEdge != L"top" && barEdge != L"left" && barEdge != L"right")
        barEdge = L"bottom";
    if (statusStyle != L"icons")
        statusStyle = L"ring";
    if (startSize != L"small" && startSize != L"medium" && startSize != L"large" && startSize != L"xlarge")
        startSize.clear();
    if (startIcon != L"small" && startIcon != L"medium" && startIcon != L"large" && startIcon != L"xlarge")
        startIcon.clear();
    if (dockDisplayMode != L"primary" && dockDisplayMode != L"fixed" && dockDisplayMode != L"all")
        dockDisplayMode = L"follow";
    if (startupMode != L"task")
        startupMode = L"run";
    if (taskbarApps != L"primaryAndWindow" && taskbarApps != L"window")
        taskbarApps = L"all";
    if (notifyPlacement != L"topLeft" && notifyPlacement != L"topRight" && notifyPlacement != L"bottomLeft" && notifyPlacement != L"bottomRight")
        notifyPlacement = L"dock";
    if (widgetBackdrop != L"wallpaper")
        widgetBackdrop = L"desktop";
    if (widgetMaterial != L"dusk" && widgetMaterial != L"color")
        widgetMaterial = L"glass";
    if (nowPlayingMotion != L"off" && nowPlayingMotion != L"subtle" && nowPlayingMotion != L"vivid")
        nowPlayingMotion = L"normal";
    if (launchpadMaterial != L"tint")
        launchpadMaterial = L"bar";
    if (launchpadLayout != L"fullscreen")
        launchpadLayout = L"window";
    launchpadIcon = TrimAsciiWhitespace(launchpadIcon);
    if (nowPlayingRhythm != L"loop")
        nowPlayingRhythm = L"live";
    if (nowPlayingAppMode != L"allow" && nowPlayingAppMode != L"block")
        nowPlayingAppMode = L"all";
    if (folderStackLayout != L"column")
        folderStackLayout = L"fan";
    folderStackMax = std::clamp(folderStackMax, 4, 24);
    NormalizeStringList(stageExcluded);
    NormalizeStringList(nowPlayingApps, 64);
    NormalizeFolderStacks(folderStacks, 32, true);
    NormalizeFolderStacks(folderStackFiles, 32, false);
    NormalizeControlModules(controlModules);
    if (pins && pins->size() > 64)
        pins->resize(64);
    if (preset != L"liquid" && preset != L"frosted")
        preset = L"custom";
}

std::wstring Settings::ToJson() const
{
    json::JsonObject o;
    WriteBarJson(o, *this);
    WriteExtrasJson(o, *this);
    WriteSystemJson(o, *this);
    WritePinsJson(o, pins);
    return std::wstring(o.Stringify());
}

bool Settings::FromJson(const std::wstring& text, const Settings& base, Settings& out)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(text, o))
        return false;
    Settings s = base;
    ReadBarSettingsJson(o, s);
    ReadModuleSettingsJson(o, s);
    ReadExtrasSettingsJson(o, s);
    auto num = [&](const wchar_t* k, double& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Number)
            v = o.GetNamedNumber(k);
    };
    auto flag = [&](const wchar_t* k, bool& v) {
        if (o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean)
            v = o.GetNamedBoolean(k);
    };
    if (o.HasKey(L"widgetBackdrop") && o.GetNamedValue(L"widgetBackdrop").ValueType() == json::JsonValueType::String)
        s.widgetBackdrop = std::wstring(o.GetNamedString(L"widgetBackdrop"));
    if (o.HasKey(L"widgetMaterial") && o.GetNamedValue(L"widgetMaterial").ValueType() == json::JsonValueType::String)
        s.widgetMaterial = std::wstring(o.GetNamedString(L"widgetMaterial"));
    if (o.HasKey(L"language") && o.GetNamedValue(L"language").ValueType() == json::JsonValueType::String)
        s.language = std::wstring(o.GetNamedString(L"language"));
    if (o.HasKey(L"textColor") && o.GetNamedValue(L"textColor").ValueType() == json::JsonValueType::String)
        s.textColor = std::wstring(o.GetNamedString(L"textColor"));
    if (o.HasKey(L"autoHide") && o.GetNamedValue(L"autoHide").ValueType() == json::JsonValueType::String) {
        s.autoHide = std::wstring(o.GetNamedString(L"autoHide"));
        if (s.autoHide == L"dodge" && !o.HasKey(L"barDodge"))
            s.barDodge = true;
    }
    if (o.HasKey(L"barEdge") && o.GetNamedValue(L"barEdge").ValueType() == json::JsonValueType::String)
        s.barEdge = std::wstring(o.GetNamedString(L"barEdge"));
    if (o.HasKey(L"statusStyle") && o.GetNamedValue(L"statusStyle").ValueType() == json::JsonValueType::String)
        s.statusStyle = std::wstring(o.GetNamedString(L"statusStyle"));
    ReadStringArrayJson(o, L"controlModules", s.controlModules);
    auto display = [&](const wchar_t* key, std::wstring& value) {
        if (o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String)
            value = std::wstring(o.GetNamedString(key));
    };
    display(L"dockDisplayMode", s.dockDisplayMode);
    num(L"dockDisplayDelay", s.dockDisplayDelay);
    display(L"dockDisplay", s.dockDisplay);
    display(L"desktopDisplay", s.desktopDisplay);
    display(L"taskbarApps", s.taskbarApps);
    flag(L"taskbarRightPrimaryOnly", s.taskbarRightPrimaryOnly);
    flag(L"windowsContextMenu", s.windowsContextMenu);
    flag(L"clickWindowPicker", s.clickWindowPicker);
    display(L"notifyPlacement", s.notifyPlacement);
    display(L"startupMode", s.startupMode);
    ReadPinsJson(o, s);
    s.Clamp();
    out = s;
    return true;
}

} // namespace atrium
