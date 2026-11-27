// Launch CLI commands, single-instance mutex acquisition, settings launch, and logon task sync.
#pragma once

#include "launch_args.h"

#include <windows.h>

#include <optional>
#include <string>
#include <thread>

namespace atrium {

struct Settings;

inline constexpr wchar_t kHostClass[] = L"AtriumCoreHost";
inline constexpr wchar_t kInstanceMutex[] = L"Local\\Atrium.Core";

std::wstring ExeDir();
void OpenSettings(const wchar_t* page = nullptr);
void OpenSettingsQuiet(const wchar_t* page = nullptr);
std::thread StartLogonUpdate(const Settings& settings);
std::optional<int> HandleLaunchCommand(const CoreLaunchOptions& launch, HINSTANCE instance = nullptr);
HANDLE AcquireInstanceMutex(int& exitCode, bool replace);

} // namespace atrium
