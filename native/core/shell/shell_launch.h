// Dedicated STA shell-launch service: privilege routing, Explorer COM de-elevation, and bounded waits.
#pragma once

#include "shell/process_integrity.h"

#include <windows.h>

#include <string>

namespace atrium {

enum class LaunchPrivilege { ShellDefault, RequireMedium, RequestElevation, Inherit };
enum class LaunchTargetKind { Executable, Shortcut, Document, Folder, Uri, AppUserModelId };
enum class LaunchRoute { DirectCurrentToken, ExplorerComMedium, DirectRunAs, BlockedMismatch };
enum class LaunchOutcome { Succeeded, Failed, TimedOutOrUnknown };

struct LaunchRequest {
    std::wstring target;
    std::wstring parameters;
    std::wstring workingDirectory;
    std::wstring verb; // optional, e.g. L"runas", L"open", L"properties"
    LaunchTargetKind kind = LaunchTargetKind::Executable;
    LaunchPrivilege privilege = LaunchPrivilege::ShellDefault;
    int show = SW_SHOWNORMAL;
    bool noAsync = false;
    bool captureProcess = false;
};

struct LaunchResult {
    LaunchOutcome outcome = LaunchOutcome::Failed;
    DWORD error = 0;
    // Populated only when captureProcess is true and direct launch succeeded; caller closes via CloseHandle.
    HANDLE process = nullptr;
};

constexpr DWORD kDefaultShellLaunchTimeoutMs = 2500;

LaunchRoute ResolveLaunchRoute(PrivilegeMode mode, LaunchPrivilege privilege, LaunchTargetKind kind, const std::wstring& verb = {});

void StartShellLaunch();
void StopShellLaunch();

LaunchResult ShellLaunch(const LaunchRequest& request, DWORD timeoutMs = kDefaultShellLaunchTimeoutMs);
void ShellLaunchAsync(LaunchRequest request);

} // namespace atrium
