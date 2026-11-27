// Windows Win+X quick link menu items and their launch actions (quick_links.h).
#include "shell/quick_links.h"

#include "localization.h"
#include "shell/shell_launch.h"
#include "shell/system_status.h"

#include <windows.h>

#include <utility>

namespace atrium {

namespace {

enum QuickLinkId {
    InstalledApps = 1,
    MobilityCenter,
    PowerOptions,
    EventViewer,
    System,
    DeviceManager,
    NetworkConnections,
    DiskManagement,
    ComputerManagement,
    Terminal,
    TerminalAdmin,
    TaskManager,
    Settings,
    FileExplorer,
    Search,
    Run,
    SignOut,
    Sleep,
    Hibernate,
    ShutDown,
    Restart,
    Desktop,
};

bool HasMobilityCenter()
{
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return false;
    const std::wstring path = std::wstring(sys, n) + L"\\mblctr.exe";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

const wchar_t* TerminalTarget() { return SearchPathW(nullptr, L"wt.exe", nullptr, 0, nullptr, nullptr) ? L"wt.exe" : L"powershell.exe"; }

void DispatchShellOpen(const wchar_t* target, LaunchTargetKind kind, bool elevated)
{
    if (!target)
        return;
    LaunchRequest req;
    req.target = target;
    req.kind = kind;
    if (elevated) {
        req.verb = L"runas";
        req.privilege = LaunchPrivilege::RequestElevation;
    } else {
        req.privilege = LaunchPrivilege::ShellDefault;
    }
    req.noAsync = true;
    ShellLaunchAsync(std::move(req));
}

void SendWinKey(WORD vk)
{
    INPUT in[4]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

} // namespace

std::vector<QuickLink> QuickLinks()
{
    std::vector<QuickLink> items;
    items.push_back({InstalledApps, Tr(L"Installed apps")});
    if (HasMobilityCenter())
        items.push_back({MobilityCenter, Tr(L"Mobility Center")});
    items.push_back({PowerOptions, Tr(L"Power Options")});
    items.push_back({EventViewer, Tr(L"Event Viewer")});
    items.push_back({System, Tr(L"System")});
    items.push_back({DeviceManager, Tr(L"Device Manager")});
    items.push_back({NetworkConnections, Tr(L"Network Connections")});
    items.push_back({DiskManagement, Tr(L"Disk Management")});
    items.push_back({ComputerManagement, Tr(L"Computer Management")});
    items.push_back({Terminal, Tr(L"Terminal")});
    items.push_back({TerminalAdmin, Tr(L"Terminal (Admin)")});
    items.push_back({});
    items.push_back({TaskManager, Tr(L"Task Manager")});
    items.push_back({Settings, Tr(L"Settings")});
    items.push_back({FileExplorer, Tr(L"File Explorer")});
    items.push_back({Search, Tr(L"Search")});
    items.push_back({Run, Tr(L"Run")});
    items.push_back({});
    items.push_back({0, Tr(L"Shut down or sign out"), true});
    items.push_back({SignOut, Tr(L"Sign out")});
    items.push_back({Sleep, Tr(L"Sleep")});
    if (const HibernateMenuOption hiber = DescribeHibernateMenu(QueryHibernateCapability()); hiber.visible)
        items.push_back({Hibernate, hiber.label});
    items.push_back({ShutDown, Tr(L"Shut down")});
    items.push_back({Restart, Tr(L"Restart")});
    items.push_back({});
    items.push_back({Desktop, Tr(L"Desktop")});
    return items;
}

void RunQuickLink(int id, HWND notifyHost, UINT noticeMsg)
{
    if (id == Terminal || id == TerminalAdmin) {
        DispatchShellOpen(TerminalTarget(), LaunchTargetKind::Executable, id == TerminalAdmin);
        return;
    }
    struct LaunchSpec {
        int id;
        const wchar_t* target;
        LaunchTargetKind kind;
    };
    static constexpr LaunchSpec kLaunches[] = {
        {InstalledApps, L"ms-settings:appsfeatures", LaunchTargetKind::Uri},
        {MobilityCenter, L"mblctr.exe", LaunchTargetKind::Executable},
        {PowerOptions, L"ms-settings:powersleep", LaunchTargetKind::Uri},
        {EventViewer, L"eventvwr.msc", LaunchTargetKind::Document},
        {System, L"ms-settings:about", LaunchTargetKind::Uri},
        {DeviceManager, L"devmgmt.msc", LaunchTargetKind::Document},
        {NetworkConnections, L"ms-settings:network", LaunchTargetKind::Uri},
        {DiskManagement, L"diskmgmt.msc", LaunchTargetKind::Document},
        {ComputerManagement, L"compmgmt.msc", LaunchTargetKind::Document},
        {TaskManager, L"taskmgr.exe", LaunchTargetKind::Executable},
        {Settings, L"ms-settings:", LaunchTargetKind::Uri},
        {FileExplorer, L"explorer.exe", LaunchTargetKind::Executable},
        {Run, L"shell:::{2559a1f3-21d7-11d4-bdaf-00c04f60b9f0}", LaunchTargetKind::Uri},
    };
    for (const LaunchSpec& spec : kLaunches) {
        if (spec.id == id) {
            DispatchShellOpen(spec.target, spec.kind, false);
            return;
        }
    }
    switch (id) {
    case Search: SendWinKey('S'); break;
    case Desktop: SendWinKey('D'); break;
    case SignOut: ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER); break;
    case Sleep: RunPowerAction(PowerAction::Sleep, notifyHost, noticeMsg); break;
    case Hibernate: RunPowerAction(PowerAction::Hibernate, notifyHost, noticeMsg); break;
    case ShutDown: RunPowerAction(PowerAction::Shutdown, notifyHost, noticeMsg); break;
    case Restart: RunPowerAction(PowerAction::Restart, notifyHost, noticeMsg); break;
    default: break;
    }
}

} // namespace atrium
