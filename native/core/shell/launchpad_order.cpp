#include "shell/launchpad_order.h"

#include "log.h"
#include "settings.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cstdlib>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

std::wstring OrderPath()
{
    std::wstring path = SettingsPath(); // ...\Atrium\settings.json
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"launchpad.json";
}

std::wstring Str(const json::JsonObject& o, const wchar_t* key)
{
    return o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String ? std::wstring(o.GetNamedString(key)) : std::wstring();
}

} // namespace

std::wstring LaunchpadLayout::NewFolderId() const
{
    unsigned long next = 1;
    for (const LaunchpadFolder& f : folders)
        if (f.id.starts_with(L"folder:"))
            next = std::max(next, std::wcstoul(f.id.c_str() + 7, nullptr, 10) + 1);
    return L"folder:" + std::to_wstring(next);
}

void LaunchpadLayout::Clamp()
{
    std::vector<std::wstring> cleanHidden;
    for (std::wstring& id : hidden)
        if (!id.empty() && std::ranges::find(cleanHidden, id) == cleanHidden.end())
            cleanHidden.push_back(std::move(id));
    hidden = std::move(cleanHidden);
    auto isHidden = [&](const std::wstring& id) { return std::ranges::find(hidden, id) != hidden.end(); };

    std::vector<std::wstring> seenFolderIds, seenFolderApps;
    std::vector<LaunchpadFolder> cleanFolders;
    for (LaunchpadFolder& f : folders) {
        if (!f.id.starts_with(L"folder:") || std::ranges::find(seenFolderIds, f.id) != seenFolderIds.end())
            continue;
        std::vector<std::wstring> cleanApps;
        for (std::wstring& a : f.apps) {
            if (a.empty() || a.starts_with(L"folder:") || a.starts_with(L'#') || isHidden(a))
                continue;
            if (std::ranges::find(seenFolderApps, a) != seenFolderApps.end())
                continue;
            seenFolderApps.push_back(a);
            cleanApps.push_back(std::move(a));
        }
        if (cleanApps.empty())
            continue;
        seenFolderIds.push_back(f.id);
        f.apps = std::move(cleanApps);
        cleanFolders.push_back(std::move(f));
    }
    folders = std::move(cleanFolders);

    std::vector<std::wstring> cleanOrder;
    for (std::wstring& id : order) {
        if (id.empty() || isHidden(id) || std::ranges::find(cleanOrder, id) != cleanOrder.end())
            continue;
        cleanOrder.push_back(std::move(id));
    }
    order = std::move(cleanOrder);
}

bool ParseLaunchpadLayoutJson(std::wstring_view text, LaunchpadLayout& layout)
{
    layout = {};
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o))
        return false;
    if (o.HasKey(L"order") && o.GetNamedValue(L"order").ValueType() == json::JsonValueType::Array)
        for (const auto& v : o.GetNamedArray(L"order"))
            if (v.ValueType() == json::JsonValueType::String && layout.order.size() < 4096)
                layout.order.emplace_back(v.GetString());
    if (o.HasKey(L"folders") && o.GetNamedValue(L"folders").ValueType() == json::JsonValueType::Array) {
        for (const auto& v : o.GetNamedArray(L"folders")) {
            if (v.ValueType() != json::JsonValueType::Object)
                continue;
            const auto fo = v.GetObject();
            LaunchpadFolder f{Str(fo, L"id"), Str(fo, L"name"), {}};
            if (fo.HasKey(L"apps") && fo.GetNamedValue(L"apps").ValueType() == json::JsonValueType::Array)
                for (const auto& av : fo.GetNamedArray(L"apps"))
                    if (av.ValueType() == json::JsonValueType::String && f.apps.size() < 512)
                        f.apps.emplace_back(av.GetString());
            layout.folders.push_back(std::move(f));
        }
    }
    if (o.HasKey(L"hidden") && o.GetNamedValue(L"hidden").ValueType() == json::JsonValueType::Array)
        for (const auto& v : o.GetNamedArray(L"hidden"))
            if (v.ValueType() == json::JsonValueType::String && layout.hidden.size() < 4096)
                layout.hidden.emplace_back(v.GetString());
    layout.Clamp();
    return true;
}

std::wstring SerializeLaunchpadLayoutJson(const LaunchpadLayout& layout)
{
    json::JsonArray order;
    for (const std::wstring& id : layout.order)
        order.Append(json::JsonValue::CreateStringValue(id));
    json::JsonObject o;
    o.SetNamedValue(L"order", order);
    if (!layout.folders.empty()) {
        json::JsonArray folders;
        for (const LaunchpadFolder& f : layout.folders) {
            json::JsonObject fo;
            fo.SetNamedValue(L"id", json::JsonValue::CreateStringValue(f.id));
            fo.SetNamedValue(L"name", json::JsonValue::CreateStringValue(f.name));
            json::JsonArray apps;
            for (const std::wstring& a : f.apps)
                apps.Append(json::JsonValue::CreateStringValue(a));
            fo.SetNamedValue(L"apps", apps);
            folders.Append(fo);
        }
        o.SetNamedValue(L"folders", folders);
    }
    if (!layout.hidden.empty()) {
        json::JsonArray hidden;
        for (const std::wstring& id : layout.hidden)
            hidden.Append(json::JsonValue::CreateStringValue(id));
        o.SetNamedValue(L"hidden", hidden);
    }
    return std::wstring(o.Stringify());
}

LaunchpadLayout LoadLaunchpadLayout()
{
    LaunchpadLayout layout;
    const std::wstring text = ReadTextFile(OrderPath());
    if (!text.empty() && !ParseLaunchpadLayoutJson(text, layout))
        Log(L"launchpad: %s is not valid JSON, ignoring", OrderPath().c_str());
    return layout;
}

bool SaveLaunchpadLayout(const LaunchpadLayout& layout)
{
    if (WriteTextFile(OrderPath(), SerializeLaunchpadLayoutJson(layout)))
        return true;
    Log(L"launchpad: could not save %s", OrderPath().c_str());
    return false;
}

} // namespace atrium
