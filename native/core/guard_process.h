// Guard process (`--guard`) that restores Windows' taskbar if Atrium hangs or exits unexpectedly.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace atrium {

std::wstring InstanceGenerationPath();
uint64_t ReadInstanceGeneration(const std::wstring& path = InstanceGenerationPath());
std::optional<uint64_t> AdvanceInstanceGeneration(const std::wstring& path = InstanceGenerationPath());
void SetActiveInstanceGeneration(uint64_t generation);
uint64_t ActiveInstanceGeneration();
bool ShouldGuardUndoLeftover(uint64_t guardGeneration, uint64_t activeGeneration);

std::wstring AgentPidPath();
bool WriteAgentPid(DWORD pid, const std::wstring& path = AgentPidPath());
DWORD ReadAgentPid(const std::wstring& path = AgentPidPath());
void ClearAgentPid(const std::wstring& path = AgentPidPath());

void EnsureGuard(HANDLE& guard);
int RunGuard(DWORD pid, const wchar_t* hostClass, uint64_t generation = 0);
void WaitForGuard();

} // namespace atrium
