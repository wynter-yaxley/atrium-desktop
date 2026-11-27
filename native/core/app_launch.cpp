#include "app_launch.h"

#include "guard_process.h"
#include "host_messages.h"
#include "lifecycle_server.h"
#include "localization.h"
#include "log.h"
#include "settings.h"
#include "shell/app_registration.h"
#include "shell/desktop_agent_host.h"
#include "shell/logon_task.h"
#include "shell/process_integrity.h"
#include "shell/shell_launch.h"
#include "win32_path.h"

#include <shellapi.h>

#include <cerrno>
#include <chrono>
#include <cwchar>
#include <thread>
#include <vector>

namespace atrium {

namespace {

uint64_t ParseGuardGenerationFromCommandLine()
{
    int count = 0;
    auto* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
        return 0;
    uint64_t generation = 0;
    for (int i = 1; i + 1 < count; ++i) {
        if (wcscmp(argv[i], L"--guard-gen") != 0)
            continue;
        const wchar_t* s = argv[i + 1];
        if (s[0] >= L'0' && s[0] <= L'9') {
            wchar_t* end = nullptr;
            errno = 0;
            const unsigned long long val = std::wcstoull(s, &end, 10);
            if (errno == 0 && end != s && *end == L'\0')
                generation = static_cast<uint64_t>(val);
        }
        break;
    }
    LocalFree(static_cast<HLOCAL>(argv));
    return generation;
}

bool SignalOtherOpenSettings(HWND other)
{
    if (other && PostMessageW(other, WM_OPEN_SETTINGS, 0, 0) != 0)
        return true;
    const DWORD err = other ? GetLastError() : ERROR_ACCESS_DENIED;
    if (other && err != ERROR_ACCESS_DENIED)
        return false;
    LifecycleReply reply;
    return SendLifecycleCommand(L"open-settings", {}, &reply) && reply.ok;
}

bool QuitOtherInstance(HWND other, DWORD timeoutMs)
{
    DWORD pid = 0;
    if (other)
        GetWindowThreadProcessId(other, &pid);
    if (!pid) {
        LifecycleReply ping;
        if (SendLifecycleCommand(L"ping", {}, &ping) && ping.ok)
            pid = ping.pid;
    }
    HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    bool sent = other && PostMessageW(other, WM_CLOSE, 0, 0) != 0;
    if (!sent) {
        const DWORD err = other ? GetLastError() : ERROR_ACCESS_DENIED;
        if (!other || err == ERROR_ACCESS_DENIED) {
            LifecycleReply reply;
            sent = SendLifecycleCommand(L"quit", {}, &reply) && reply.ok;
        }
    }
    if (!sent) {
        if (process)
            CloseHandle(process);
        return false;
    }
    bool exited = false;
    if (process) {
        exited = WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(process);
    } else {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            LifecycleReply ping;
            if (!SendLifecycleCommand(L"ping", {}, &ping, {}, 250) || !ping.ok) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    WaitForGuard();
    return exited;
}

void OpenSettingsImpl(const wchar_t* page, bool showErrorUi)
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
        // The settings app keeps a single instance itself and activates it; fail closed if delegation fails.
        LaunchRequest req;
        req.target = exe;
        req.parameters = args;
        req.kind = LaunchTargetKind::Executable;
        req.privilege = LaunchPrivilege::ShellDefault;
        req.show = SW_SHOWNORMAL;
        const LaunchResult result = ShellLaunch(req);
        if (result.outcome == LaunchOutcome::Succeeded)
            return;
        Log(L"settings: could not launch %s (outcome=%d, err=%lu)", exe.c_str(), int(result.outcome), result.error);
        return;
    }
    Log(L"settings: Atrium.Settings.exe not found next to %s", dir.c_str());
    if (showErrorUi) {
        MessageBoxW(nullptr, Tr(L"Atrium.Settings.exe was not found. Make sure the Settings folder in the installation directory is complete."), L"Atrium",
                    MB_ICONWARNING);
    }
}

} // namespace

std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L'\\'));
}

// page: the settings app opens on it ("widgets"); an open window just comes to the front.
void OpenSettings(const wchar_t* page) { OpenSettingsImpl(page, true); }

void OpenSettingsQuiet(const wchar_t* page) { OpenSettingsImpl(page, false); }

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

std::optional<int> HandleLaunchCommand(const CoreLaunchOptions& launch, HINSTANCE instance)
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
    if (launch.desktopAgent)
        return launch.desktopAgentCorePid ?
                   RunDesktopAgent(launch.desktopAgentCorePid, launch.desktopAgentGeneration, instance ? instance : GetModuleHandleW(nullptr)) :
                   1;
    if (launch.guard)
        return launch.guardPid ? RunGuard(launch.guardPid, kHostClass, ParseGuardGenerationFromCommandLine()) : 1;
    if (!launch.exitRequested)
        return std::nullopt;
    HWND other = FindWindowForExecutable(kHostClass);
    if (!other)
        return 0;
    return QuitOtherInstance(other, 10000) ? 0 : 1;
}

HANDLE AcquireInstanceMutex(int& exitCode, bool replace)
{
    KernelObjectSecurity security = KernelObjectSecurity::CreateSameUserMutex();
    if (!security.Valid()) {
        Log(L"core: could not build security descriptor for instance mutex (%lu)", GetLastError());
        exitCode = 1;
        return nullptr;
    }
    HANDLE mutex = CreateMutexExW(security.Get(), kInstanceMutex, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    const DWORD err = GetLastError();
    if (!mutex && err != ERROR_ACCESS_DENIED) {
        exitCode = 1;
        return nullptr;
    }
    if (mutex && err != ERROR_ALREADY_EXISTS) {
        AdvanceInstanceGeneration();
        return mutex;
    }
    HWND other = FindWindowW(kHostClass, nullptr);
    const bool same = other && SameExecutableWindow(other);
    if ((same || !other) && !replace) {
        SignalOtherOpenSettings(other);
        if (mutex)
            CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    bool replaced = false;
    if ((other || replace || err == ERROR_ACCESS_DENIED) &&
        (same || !other ||
         MessageBoxW(nullptr, Tr(L"Atrium is running from another location. Quit it and use the version in this folder?"), L"Atrium",
                     MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES))
        replaced = QuitOtherInstance(other, 15000);
    if (!replaced && replace && mutex && err == ERROR_ALREADY_EXISTS) {
        const DWORD wait = WaitForSingleObject(mutex, 15000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            WaitForGuard();
            AdvanceInstanceGeneration();
            Log(L"core: took over from an exiting instance");
            return mutex;
        }
    }
    if (mutex)
        CloseHandle(mutex);
    if (!replaced) {
        exitCode = 0;
        return nullptr;
    }
    mutex = CreateMutexExW(security.Get(), kInstanceMutex, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);
        exitCode = 0;
        return nullptr;
    }
    AdvanceInstanceGeneration();
    Log(L"core: took over from %s", same ? L"an existing instance" : L"the copy at another location");
    return mutex;
}

} // namespace atrium
