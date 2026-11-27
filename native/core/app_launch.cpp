#include "app_launch.h"

#include "guard_process.h"
#include "host_messages.h"
#include "localization.h"
#include "log.h"
#include "settings.h"
#include "shell/app_registration.h"
#include "shell/logon_task.h"
#include "win32_path.h"

#include <shellapi.h>

#include <vector>

namespace atrium {

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\'));
}

// page: the settings app opens on it ("widgets"); an open window just comes to the front.
void OpenSettings(const wchar_t* page)
{
    const std::wstring dir = ExeDir();
    std::vector<std::wstring> candidates = {dir + L"\\Settings\\Atrium.Settings.exe", dir + L"\\Atrium.Settings.exe"};
    // A development build (native\build\win-x64\Release): the packaged settings app of the same repository.
    for (std::wstring up = dir; IsDevelopmentBuild(); candidates.push_back(up + L"\\dist\\Development\\Atrium\\Settings\\Atrium.Settings.exe")) {
        const size_t pos = up.find_last_of(L'\\');
        if (pos == std::wstring::npos || pos <= 2)
            break;
        up.resize(pos);
    }
    const std::wstring args = FormatSettingsLaunchArgs(page, DataDirectoryArgs());
    for (const std::wstring& exe : candidates) {
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        // The settings app keeps a single instance itself and activates it.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
            return;
    }
    Log(L"settings: Atrium.Settings.exe not found next to %s", dir.c_str());
    MessageBoxW(nullptr, Tr(L"Atrium.Settings.exe was not found. Make sure the Settings folder in the installation directory is complete."), L"Atrium",
                MB_ICONWARNING);
}

std::thread StartLogonUpdate(const Settings& settings)
{
    if (!ShouldSyncStartupOnLaunch(IsDevelopmentBuild(), IsRegisteredApplication(ExeDir()), settings.startOnLogin,
                                   ParseStartupMode(settings.startupMode) == StartupMode::Run && StartupTurnedOff()))
        return {};
    return std::thread([mode = settings.startupMode] {
        if (!UpdateLogonTask(true, mode))
            Log(L"startup: could not update the Windows logon entry");
    });
}

std::optional<int> HandleLaunchCommand(const CoreLaunchOptions& launch)
{
    if (launch.uninstall) {
        if (MessageBoxW(nullptr, Tr(L"Uninstall Atrium? Your personal settings and desktop widget layout will be kept."), L"Atrium",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
            return 0;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ExeDir() + L"\\Uninstall.ps1\" -Confirmed";
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", powershell.c_str(), arguments.c_str(), nullptr, SW_HIDE)) > 32 ? 0 : 1;
    }
    if (launch.guard)
        return launch.guardPid ? RunGuard(launch.guardPid, kHostClass) : 1;
    if (!launch.exitRequested)
        return std::nullopt;
    HWND other = FindWindowForExecutable(kHostClass);
    if (!other)
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(other, WM_CLOSE, 0, 0);
    const DWORD wait = process ? WaitForSingleObject(process, 10000) : WAIT_FAILED;
    if (process)
        CloseHandle(process);
    WaitForGuard();
    return wait == WAIT_OBJECT_0 ? 0 : 1;
}

HANDLE AcquireInstanceMutex(int& exitCode, bool replace)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex) {
        exitCode = 1;
        return nullptr;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return mutex;
    HWND other = FindWindowW(kHostClass, nullptr);
    const bool same = other && SameExecutableWindow(other);
    if (same && !replace) {
        PostMessageW(other, WM_OPEN_SETTINGS, 0, 0);
        CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    bool replaced = false;
    if (other && (same || MessageBoxW(nullptr, Tr(L"Atrium is running from another location. Quit it and use the version in this folder?"), L"Atrium",
                                      MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            PostMessageW(other, WM_CLOSE, 0, 0);
            replaced = WaitForSingleObject(process, 15000) == WAIT_OBJECT_0;
            CloseHandle(process);
        }
        WaitForGuard();
    }
    CloseHandle(mutex);
    if (!replaced) {
        exitCode = 0;
        return nullptr;
    }
    mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    Log(L"core: took over from %s", same ? L"an existing instance" : L"the copy at another location");
    return mutex;
}

} // namespace atrium
