#include "shell/shell_launch.h"

#include <array>
#include <iostream>
#include <string>

using namespace atrium;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

LaunchRoute ExpectedRoute(PrivilegeMode mode, LaunchPrivilege privilege, const std::wstring& verb)
{
    if (privilege == LaunchPrivilege::RequestElevation || _wcsicmp(verb.c_str(), L"runas") == 0)
        return LaunchRoute::DirectRunAs;
    if (privilege == LaunchPrivilege::Inherit)
        return LaunchRoute::DirectCurrentToken;
    switch (mode) {
    case PrivilegeMode::ElevatedSplit: return LaunchRoute::ExplorerComMedium;
    case PrivilegeMode::Standard: return LaunchRoute::DirectCurrentToken;
    case PrivilegeMode::ElevatedUnified:
    case PrivilegeMode::CrossAccountElevated:
    case PrivilegeMode::Unknown: return privilege == LaunchPrivilege::RequireMedium ? LaunchRoute::BlockedMismatch : LaunchRoute::DirectCurrentToken;
    }
    return LaunchRoute::BlockedMismatch;
}

bool CheckExhaustiveRoutingMatrix()
{
    constexpr std::array<PrivilegeMode, 5> kModes{
        PrivilegeMode::Standard, PrivilegeMode::ElevatedSplit, PrivilegeMode::ElevatedUnified, PrivilegeMode::CrossAccountElevated, PrivilegeMode::Unknown,
    };
    constexpr std::array<LaunchPrivilege, 4> kPrivileges{
        LaunchPrivilege::ShellDefault,
        LaunchPrivilege::RequireMedium,
        LaunchPrivilege::RequestElevation,
        LaunchPrivilege::Inherit,
    };
    constexpr std::array<LaunchTargetKind, 6> kKinds{
        LaunchTargetKind::Executable, LaunchTargetKind::Shortcut, LaunchTargetKind::Document,
        LaunchTargetKind::Folder,     LaunchTargetKind::Uri,      LaunchTargetKind::AppUserModelId,
    };
    const std::array<std::wstring, 5> verbs{L"", L"open", L"properties", L"runas", L"RunAs"};

    for (PrivilegeMode mode : kModes) {
        for (LaunchPrivilege priv : kPrivileges) {
            for (LaunchTargetKind kind : kKinds) {
                for (const std::wstring& verb : verbs) {
                    const LaunchRoute actual = ResolveLaunchRoute(mode, priv, kind, verb);
                    const LaunchRoute expected = ExpectedRoute(mode, priv, verb);
                    if (actual != expected) {
                        std::cerr << "Route mismatch: mode=" << int(mode) << " priv=" << int(priv) << " kind=" << int(kind) << " actual=" << int(actual)
                                  << " expected=" << int(expected) << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool CheckKeySecurityInvariants()
{
    return Require(ResolveLaunchRoute(PrivilegeMode::ElevatedSplit, LaunchPrivilege::ShellDefault, LaunchTargetKind::Executable) ==
                       LaunchRoute::ExplorerComMedium,
                   "ElevatedSplit + ShellDefault routes through ExplorerComMedium") &&
           Require(ResolveLaunchRoute(PrivilegeMode::ElevatedSplit, LaunchPrivilege::RequireMedium, LaunchTargetKind::AppUserModelId) ==
                       LaunchRoute::ExplorerComMedium,
                   "ElevatedSplit + RequireMedium + AppUserModelId routes through ExplorerComMedium") &&
           Require(ResolveLaunchRoute(PrivilegeMode::ElevatedUnified, LaunchPrivilege::RequireMedium, LaunchTargetKind::Executable) ==
                       LaunchRoute::BlockedMismatch,
                   "ElevatedUnified + RequireMedium fails closed as BlockedMismatch") &&
           Require(ResolveLaunchRoute(PrivilegeMode::CrossAccountElevated, LaunchPrivilege::RequireMedium, LaunchTargetKind::Uri) ==
                       LaunchRoute::BlockedMismatch,
                   "CrossAccountElevated + RequireMedium fails closed as BlockedMismatch") &&
           Require(ResolveLaunchRoute(PrivilegeMode::Unknown, LaunchPrivilege::RequireMedium, LaunchTargetKind::Document) == LaunchRoute::BlockedMismatch,
                   "Unknown + RequireMedium fails closed as BlockedMismatch") &&
           Require(ResolveLaunchRoute(PrivilegeMode::Standard, LaunchPrivilege::RequireMedium, LaunchTargetKind::Shortcut) == LaunchRoute::DirectCurrentToken,
                   "Standard + RequireMedium launches with DirectCurrentToken") &&
           Require(ResolveLaunchRoute(PrivilegeMode::ElevatedSplit, LaunchPrivilege::Inherit, LaunchTargetKind::Executable) == LaunchRoute::DirectCurrentToken,
                   "Inherit explicitly stays on DirectCurrentToken") &&
           Require(ResolveLaunchRoute(PrivilegeMode::ElevatedSplit, LaunchPrivilege::ShellDefault, LaunchTargetKind::Executable, L"runas") ==
                       LaunchRoute::DirectRunAs,
                   "Explicit runas verb routes to DirectRunAs");
}

bool CheckServiceLifecycle()
{
    StartShellLaunch();
    StartShellLaunch();
    const LaunchResult emptyResult = ShellLaunch(LaunchRequest{});
    const bool emptyFailed = Require(emptyResult.outcome == LaunchOutcome::Failed && emptyResult.error == ERROR_INVALID_PARAMETER,
                                     "Empty target fails fast with ERROR_INVALID_PARAMETER");
    StopShellLaunch();
    StopShellLaunch();
    return emptyFailed;
}

} // namespace

int main()
{
    if (!CheckExhaustiveRoutingMatrix() || !CheckKeySecurityInvariants() || !CheckServiceLifecycle())
        return 1;
    std::cout << "shell launch: all checks passed\n";
    return 0;
}
