#include "app_menus.h"

#include "bar_edge.h"
#include "desktop_helpers.h"
#include "host_messages.h"
#include "localization.h"
#include "log.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/jump_list.h"
#include "shell/quick_links.h"
#include "shell/shell_launch.h"
#include "shell/shell_verbs.h"
#include "ui/stage_manager.h"
#include "win32_path.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <utility>

namespace atrium {

namespace {

enum MenuCmd : UINT {
    CmdSettings = 100,
    CmdToggle,
    CmdExit,
    CmdNewWindow,
    CmdPin,
    CmdUnpin,
    CmdClose,
    CmdOpenLocation,
    CmdDesktop,
    CmdTaskManager,
    CmdTerminate,
    CmdTerminateConfirm,
    CmdWidgets,
    CmdStage,
    CmdWidgetsModule,
    CmdStageAdd,
    CmdRunAs,
    CmdProperties,
};
constexpr int kCmdRecent = 2000;
constexpr int kCmdExtra = 4000;

template<class Fn> void ModifyPins(SettingsServer* server, Fn&& fn)
{
    if (!server)
        return;
    server->Modify([&](Settings& s) {
        auto pins = s.pins.value_or(std::vector<PinnedApp>{});
        if (fn(pins))
            s.pins = std::move(pins);
    });
}

std::vector<std::pair<std::wstring, std::vector<JumpItem>>> BuildFallbackJumpSections(const std::wstring& appId)
{
    if (appId.empty())
        return {};
    std::vector<JumpItem> places, workspaces, files;
    std::set<std::wstring> seen;
    const auto take = [&](std::vector<JumpItem>& into, const ShellRecent& entry, bool folder) {
        if (into.size() >= 5 || (!IsRemotePath(entry.path) && GetFileAttributesW(entry.path.c_str()) == INVALID_FILE_ATTRIBUTES))
            return;
        if (seen.insert(FoldCase(entry.path)).second)
            into.push_back({entry.name, entry.path, {}, {}, folder});
    };
    for (const ShellRecent& entry : ShellDocumentList(appId, ADLT_FREQUENT, 24))
        if (IsDirectoryPath(entry.path))
            take(places, entry, true);
    for (const ShellRecent& entry : ShellDocumentList(appId, ADLT_RECENT, 24)) {
        const bool folder = IsDirectoryPath(entry.path);
        take(folder ? places : IsWorkspaceFile(entry.path) ? workspaces : files, entry, folder);
    }
    std::vector<std::pair<std::wstring, std::vector<JumpItem>>> sections;
    for (auto [label, list] : {std::pair{L"Folders", &places}, {L"Workspaces", &workspaces}, {L"Recent", &files}})
        if (!list->empty())
            sections.emplace_back(label, std::move(*list));
    return sections;
}

std::wstring ResolveJumpAppId(const PinnedApp& pin, const std::wstring& appPath)
{
    std::wstring appId = pin.aumid;
    if (appId.empty())
        appId = ShellAppId(appPath);
    if (appId.empty())
        appId = ShellAppIdOfProcess(pin.path.empty() ? appPath : pin.path);
    if (appId.empty()) {
        const size_t slash = appPath.find_last_of(L"\\/");
        const std::wstring exe = slash == std::wstring::npos ? appPath : appPath.substr(slash + 1);
        if (_wcsicmp(exe.c_str(), L"explorer.exe") == 0)
            appId = L"Microsoft.Windows.Explorer";
    }
    return appId;
}

std::vector<std::pair<std::wstring, std::vector<JumpItem>>> LoadJumpSections(const std::wstring& appId, const std::wstring& exePath)
{
    std::vector<std::pair<std::wstring, std::vector<JumpItem>>> sections;
    if (std::shared_ptr<const JumpListData> jump = LoadJumpList(appId, exePath)) {
        if (!jump->pinned.empty())
            sections.emplace_back(L"Pinned", jump->pinned);
        for (const JumpCategory& category : jump->categories)
            sections.emplace_back(category.title, category.items);
        if (!jump->recent.empty())
            sections.emplace_back(L"Recent", jump->recent);
        return sections;
    }
    return BuildFallbackJumpSections(appId);
}

void AppendJumpEntries(std::vector<std::pair<std::wstring, std::vector<JumpItem>>>& sections, std::vector<MenuEntry>& entries, std::vector<JumpItem>& listed)
{
    size_t command = 0;
    for (auto& [title, rows] : sections) {
        if (rows.empty())
            continue;
        entries.push_back({Tr(title.c_str()), 0, false, false, true});
        int shown = 0;
        for (JumpItem& item : rows) {
            if (shown++ >= 20)
                break;
            entries.push_back({item.name, int(kCmdRecent + command++)});
            listed.push_back(std::move(item));
        }
    }
}

void LaunchJumpItem(const JumpItem& item)
{
    if (!item.program.empty()) {
        LaunchShellAsync(L"open", item.program, item.args);
    } else if (item.folder && !item.target.empty()) {
        LaunchShellAsync(L"open", L"explorer.exe", L"\"" + item.target + L"\"");
    } else if (!item.target.empty()) {
        LaunchShellAsync(L"open", item.target);
    }
}

struct AppCommandContext {
    AppMenuEnv env;
    AppMenuTarget target;
    std::optional<PinnedApp> pinnedEntry;
    std::wstring appPath;
    std::vector<JumpItem> listed;
    std::vector<AppMenuExtra> extras;
    POINT pt{};
    MenuPlace place = MenuPlace::Above;
};

void ExecuteAppMenuCommand(const AppCommandContext& ctx, int cmd)
{
    switch (cmd) {
    case CmdNewWindow: ctx.target.launch(); break;
    case CmdStageAdd:
        if (ctx.env.stage && ctx.env.stage->Enabled())
            ctx.env.stage->OpenInCurrentGroup(ctx.target.key, ctx.target.launch);
        else if (!ctx.target.windows.empty())
            ctx.env.tasks->Activate(ctx.target.key);
        else
            ctx.target.launch();
        break;
    case CmdPin: ModifyPins(ctx.env.server, [&pin = ctx.target.pin](std::vector<PinnedApp>& pins) { return AddUniquePin(pins, pin); }); break;
    case CmdUnpin:
        ModifyPins(ctx.env.server,
                   [idx = ctx.target.pinIndex, &entry = ctx.pinnedEntry](std::vector<PinnedApp>& pins) { return UnpinMatchingPin(pins, idx, entry); });
        break;
    case CmdClose:
        for (HWND window : ctx.target.windows)
            TaskModel::CloseWindow(window);
        break;
    case CmdTerminate: {
        std::vector<MenuEntry> confirm = {{TrFormat(L"Force close “%s”? Unsaved content will be lost", ctx.target.name.c_str()), 0, false, false, true},
                                          {L"", 0},
                                          {Tr(L"Force close"), CmdTerminateConfirm},
                                          {Tr(L"Cancel"), 0}};
        ctx.env.menu->Open(std::move(confirm), ctx.pt, ctx.place, [tasks = ctx.env.tasks, key = ctx.target.key](int answer) {
            if (answer == CmdTerminateConfirm && tasks)
                tasks->Terminate(key);
        });
        break;
    }
    case CmdOpenLocation:
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(ctx.target.location.c_str())) {
            SHOpenFolderAndSelectItems(id, 0, nullptr, 0);
            ILFree(id);
        }
        break;
    case CmdRunAs:
        if (!ctx.appPath.empty()) {
            const std::wstring args = ctx.pinnedEntry ? ctx.pinnedEntry->args : std::wstring{};
            LaunchShellAsync(L"runas", ctx.appPath, args, std::filesystem::path(ctx.appPath).parent_path().wstring());
        }
        break;
    case CmdProperties: ShowShellProperties(ctx.env.host, ctx.appPath); break;
    default:
        if (cmd >= kCmdExtra && size_t(cmd - kCmdExtra) < ctx.extras.size()) {
            if (ctx.extras[size_t(cmd - kCmdExtra)].run)
                ctx.extras[size_t(cmd - kCmdExtra)].run();
        } else if (cmd >= kCmdRecent && size_t(cmd - kCmdRecent) < ctx.listed.size()) {
            LaunchJumpItem(ctx.listed[size_t(cmd - kCmdRecent)]);
        }
        break;
    }
}

AppMenuTarget MenuTargetOf(TaskModel& tasks, const TaskItem& item)
{
    AppMenuTarget t{item.key, item.name, item.pinned, item.multipleWindows, tasks.CanTerminate(item.key), item.pinIndex, item.windows};
    t.pin = tasks.PinFor(item.key);
    t.pinTarget = tasks.PinTarget(item.key);
    t.location = tasks.FileLocation(item.key);
    t.launch = [&tasks, key = item.key] { tasks.Launch(key); };
    return t;
}

} // namespace

MenuPlace BarMenuPlace(const Bar& bar, bool centered)
{
    switch (EdgeOf(bar.BarRect())) {
    case BarEdge::Top: return centered ? MenuPlace::BelowCentered : MenuPlace::Below;
    case BarEdge::Left: return centered ? MenuPlace::RightCentered : MenuPlace::Right;
    case BarEdge::Right: return centered ? MenuPlace::LeftCentered : MenuPlace::Left;
    default: return centered ? MenuPlace::AboveCentered : MenuPlace::Above;
    }
}

void LaunchShellAsync(std::wstring verb, std::wstring file, std::wstring args, std::wstring dir)
{
    LaunchRequest req;
    const bool runAs = !verb.empty() && _wcsicmp(verb.c_str(), L"runas") == 0;
    req.privilege = runAs ? LaunchPrivilege::RequestElevation : LaunchPrivilege::ShellDefault;
    if (file.starts_with(L"shell:AppsFolder\\"))
        req.kind = LaunchTargetKind::AppUserModelId;
    else if (FoldCase(file).ends_with(L".lnk"))
        req.kind = LaunchTargetKind::Shortcut;
    else
        req.kind = LaunchTargetKind::Executable;
    req.verb = std::move(verb);
    req.target = std::move(file);
    req.parameters = std::move(args);
    req.workingDirectory = std::move(dir);
    req.noAsync = true;
    ShellLaunchAsync(std::move(req));
}

bool PinsEqual(const PinnedApp& a, const PinnedApp& b)
{
    return FoldCase(a.path) == FoldCase(b.path) && FoldCase(a.aumid) == FoldCase(b.aumid) && a.args == b.args;
}

bool AddUniquePin(std::vector<PinnedApp>& pins, const PinnedApp& pin)
{
    if (pins.size() >= 64 || std::ranges::any_of(pins, [&](const PinnedApp& existing) { return PinsEqual(existing, pin); }))
        return false;
    pins.push_back(pin);
    return true;
}

bool UnpinMatchingPin(std::vector<PinnedApp>& pins, int pinIndex, const std::optional<PinnedApp>& target)
{
    if (!target)
        return false;
    if (pinIndex >= 0 && size_t(pinIndex) < pins.size() && PinsEqual(pins[size_t(pinIndex)], *target)) {
        pins.erase(pins.begin() + pinIndex);
        return true;
    }
    if (auto it = std::ranges::find_if(pins, [&](const PinnedApp& p) { return PinsEqual(p, *target); }); it != pins.end()) {
        pins.erase(it);
        return true;
    }
    return false;
}

void ShowBarContextMenu(const AppMenuEnv& env, POINT pt)
{
    const bool widgetsOn = env.widgetsOn();
    const bool barOn = env.barOn();
    const bool stageOn = env.stage && env.stage->Enabled();
    std::vector<MenuEntry> entries = {{Tr(L"Appearance Settings…"), CmdSettings}, {}, {Tr(L"Show Desktop"), CmdDesktop}, {Tr(L"Task Manager"), CmdTaskManager}};
    if (widgetsOn)
        entries.push_back({Tr(L"Add Widgets…"), CmdWidgets});
    entries.push_back({});
    entries.push_back({Tr(L"Taskbar"), CmdToggle, barOn});
    entries.push_back({Tr(L"Desktop Widgets"), CmdWidgetsModule, widgetsOn});
    entries.push_back({Tr(L"Stage Manager"), CmdStage, stageOn});
    entries.push_back({});
    entries.push_back({Tr(L"Quit Atrium"), CmdExit});
    env.menu->Open(std::move(entries), pt, BarMenuPlace(env.activeBar(), false), [env](int cmd) {
        switch (cmd) {
        case CmdSettings: env.openSettings(nullptr); break;
        case CmdWidgets: env.openSettings(L"widgets"); break;
        case CmdToggle: env.setBar(!env.barOn()); break;
        case CmdWidgetsModule: env.setWidgets(!env.widgetsOn()); break;
        case CmdDesktop: SendWinShortcut('D'); break;
        case CmdStage: env.setStage(!(env.stage && env.stage->Enabled())); break;
        case CmdTaskManager: LaunchShellAsync(L"open", L"taskmgr.exe"); break;
        case CmdExit: DestroyWindow(env.host); break;
        default: break;
        }
    });
}

void ShowStartContextMenu(const AppMenuEnv& env, POINT pt)
{
    std::vector<MenuEntry> entries;
    for (const QuickLink& link : QuickLinks())
        entries.push_back({link.text, link.id, false, false, link.header});
    env.menu->Open(std::move(entries), pt, BarMenuPlace(env.activeBar(), false), [host = env.host](int id) { RunQuickLink(id, host, WM_NOTICE); });
}

void ShowAppMenu(const AppMenuEnv& env, const AppMenuTarget& target, POINT pt, MenuPlace place, std::function<void(bool)> done,
                 std::vector<AppMenuExtra> extras)
{
    const ULONGLONG startedAt = GetTickCount64();
    std::optional<PinnedApp> pinnedEntry;
    if (target.pinned && target.pinIndex >= 0 && env.server)
        if (const auto& pins = env.server->Current().pins; pins && size_t(target.pinIndex) < pins->size())
            pinnedEntry = (*pins)[size_t(target.pinIndex)];
    const std::wstring appPath = target.pinTarget.empty() ? target.pin.path : target.pinTarget;
    const bool windowsEntries = !env.server || env.server->Current().windowsContextMenu;
    std::wstring appId;
    std::vector<JumpItem> listed;
    std::vector<std::pair<std::wstring, std::vector<JumpItem>>> sections;
    if (windowsEntries) {
        appId = ResolveJumpAppId(target.pin, appPath);
        sections = LoadJumpSections(appId, target.pin.path);
    }

    std::vector<MenuEntry> entries = {{target.name, 0, false, false, true}, {L"", 0}};
    entries.push_back({target.windows.empty() || !target.multipleWindows ? Tr(L"Open") : Tr(L"Open New Window"), CmdNewWindow});
    if (env.stage && !env.stage->Excluded(target.key))
        entries.push_back({Tr(L"Open / Add to Current Group"), CmdStageAdd});
    AppendJumpEntries(sections, entries, listed);
    std::wstring shape;
    for (const auto& [title, rows] : sections)
        shape += (shape.empty() ? L"" : L"; ") + title + L"(" + std::to_wstring(rows.size()) + L")";
    Log(L"menu: '%s' path='%s' appid='%s' items=%zu [%s] in %llu ms", target.name.c_str(), appPath.c_str(), appId.c_str(), listed.size(), shape.c_str(),
        GetTickCount64() - startedAt);
    if (windowsEntries && !appPath.empty() && FoldCase(appPath).find(L"\\windowsapps\\") == std::wstring::npos)
        entries.push_back({Tr(L"Run as administrator"), CmdRunAs});
    if (!target.location.empty())
        entries.push_back({Tr(L"Open File Location"), CmdOpenLocation});
    entries.push_back({target.pinned ? Tr(L"Unpin from Atrium") : Tr(L"Pin to Atrium"), int(target.pinned ? CmdUnpin : CmdPin)});
    if (windowsEntries && !appPath.empty())
        entries.push_back({Tr(L"Properties"), CmdProperties});
    for (size_t i = 0; i < extras.size(); ++i) {
        if (extras[i].separatorBefore)
            entries.push_back({});
        entries.push_back({extras[i].label, kCmdExtra + int(i)});
    }
    if (!target.windows.empty())
        entries.push_back({target.windows.size() > 1 ? Tr(L"Close All Windows") : Tr(L"Close Window"), CmdClose});
    entries.push_back({Tr(L"Close Program"), CmdTerminate, false, !target.canTerminate});

    auto picked = std::make_shared<bool>(false);
    auto ctx =
        std::make_shared<AppCommandContext>(AppCommandContext{env, target, std::move(pinnedEntry), appPath, std::move(listed), std::move(extras), pt, place});
    env.menu->Open(
        std::move(entries), pt, place,
        [ctx, picked](int cmd) {
            const bool keepOpen = cmd >= kCmdExtra && size_t(cmd - kCmdExtra) < ctx->extras.size() && ctx->extras[size_t(cmd - kCmdExtra)].keepOpen;
            *picked = cmd != 0 && !keepOpen;
            ExecuteAppMenuCommand(*ctx, cmd);
        },
        [done = std::move(done), picked] {
            if (done)
                done(*picked);
        });
}

void ShowTaskItemMenu(const AppMenuEnv& env, const std::wstring& key, POINT pt)
{
    const std::vector<TaskItem> items = env.activeBar().Items();
    auto it = std::ranges::find_if(items, [&](const TaskItem& t) { return t.key == key; });
    if (it != items.end())
        ShowAppMenu(env, MenuTargetOf(*env.tasks, *it), pt, BarMenuPlace(env.activeBar(), true));
}

void ShowCatalogAppMenu(const AppMenuEnv& env, const CatalogApp& app, POINT pt, std::function<void(bool)> done, std::vector<AppMenuExtra> extras)
{
    const std::wstring aumid = FoldCase(app.aumid), exe = FoldCase(app.target);
    const std::vector<TaskItem> all = env.tasks->Snapshot(true);
    auto it = std::ranges::find_if(all, [&](const TaskItem& t) { return (!aumid.empty() && t.key == aumid) || (!exe.empty() && t.key == exe); });
    if (it != all.end()) {
        ShowAppMenu(env, MenuTargetOf(*env.tasks, *it), pt, MenuPlace::Below, std::move(done), std::move(extras));
        return;
    }
    AppMenuTarget t;
    t.key = !aumid.empty() ? aumid : exe;
    t.name = app.name;
    t.pin = AppCatalog::PinFor(app);
    if (app.id.find(L'\\') != std::wstring::npos)
        t.pinTarget = app.id;
    t.location = AppCatalog::FileLocation(app);
    t.launch = [app] { AppCatalog::Launch(app); };
    if (env.server)
        if (const auto& pins = env.server->Current().pins)
            if (auto p = std::ranges::find_if(*pins, [&](const PinnedApp& q) { return PinsEqual(q, t.pin); }); p != pins->end())
                t.pinned = true, t.pinIndex = int(p - pins->begin());
    ShowAppMenu(env, t, pt, MenuPlace::Below, std::move(done), std::move(extras));
}

void ShowOverflowAppsMenu(const AppMenuEnv& env, const std::vector<std::wstring>& keys, RECT anchor)
{
    const std::vector<TaskItem> items = env.activeBar().Items();
    std::vector<MenuEntry> entries;
    const int px = int(std::lround(16 * PopupScale(anchor)));
    for (size_t i = 0; i < keys.size(); ++i) {
        auto it = std::ranges::find_if(items, [&](const TaskItem& t) { return t.key == keys[i]; });
        std::wstring name = it != items.end() ? it->name : keys[i];
        if (it != items.end() && !it->windows.empty())
            name += it->windows.size() > 1 ? L"  \u00B7 " + std::to_wstring(it->windows.size()) : L"  \u00B7";
        entries.push_back({name, int(1000 + i), false, false, false, env.tasks->Icon(keys[i], px)});
    }
    env.menu->Open(std::move(entries), PopupAnchor(env.activeBar().BarRect(), anchor), BarMenuPlace(env.activeBar(), true), [env, keys](int cmd) {
        if (cmd >= 1000 && cmd < 1000 + int(keys.size()))
            env.tasks->Activate(keys[size_t(cmd - 1000)], env.activeBar().TaskMonitor());
    });
}

void ReorderBarTask(const AppMenuEnv& env, const std::wstring& key, int to)
{
    std::vector<TaskItem> items = env.activeBar().Items();
    auto it = std::ranges::find_if(items, [&](const TaskItem& t) { return t.key == key; });
    if (it == items.end())
        return;
    const bool wasPinned = it->pinned;
    const int oldPinIndex = it->pinIndex;
    items.erase(it);
    to = std::clamp(to, 0, int(items.size()));
    const int pinnedLeft = int(std::count_if(items.begin(), items.end(), [](const TaskItem& t) { return t.pinned; }));
    if (!wasPinned && to >= pinnedLeft) {
        const auto all = env.tasks->Snapshot();
        const std::wstring before = to < int(items.size()) ? items[size_t(to)].key : L"";
        int running = 0;
        for (const TaskItem& item : all) {
            if (item.key == before)
                break;
            if (!item.pinned && item.key != key)
                ++running;
        }
        env.tasks->MoveRunning(key, running);
        return;
    }
    const int at = to < pinnedLeft ? items[size_t(to)].pinIndex : pinnedLeft ? items[size_t(pinnedLeft - 1)].pinIndex + 1 : 0;
    const PinnedApp fresh = wasPinned ? PinnedApp{} : env.tasks->PinFor(key);
    ModifyPins(env.server, [&](std::vector<PinnedApp>& pins) {
        PinnedApp pin = fresh;
        int dest = at;
        if (wasPinned) {
            if (oldPinIndex < 0 || oldPinIndex >= int(pins.size()))
                return false;
            pin = pins[size_t(oldPinIndex)];
            pins.erase(pins.begin() + oldPinIndex);
            if (dest > oldPinIndex)
                --dest;
        }
        pins.insert(pins.begin() + std::clamp(dest, 0, int(pins.size())), pin);
        return true;
    });
    Log(L"bar: %s moved to %d%s", key.c_str(), to, wasPinned ? L"" : L" and pinned");
}

} // namespace atrium
