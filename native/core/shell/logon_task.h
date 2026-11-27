// Current user's HKCU Run startup entry and Task Scheduler logon task.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace atrium {

enum class StartupMode { Run, Task };

struct StartupState {
    std::wstring exe;
    std::wstring taskProgram;
    std::wstring runValue;
    bool hasRunValue = false;
    bool runReadOk = true;
    bool taskReadOk = true;
};

struct StartupPlan {
    bool ok = false;
    bool removeTask = false;
    bool writeRun = false;
    bool deleteRun = false;
    bool createTask = false;
};

StartupMode ParseStartupMode(const std::wstring& mode);
// The program the Task Scheduler task starts, empty when there is no task.
std::wstring LogonTaskProgram();
bool RemoveLogonTask();
// Updates Windows logon registration (HKCU Run or Task Scheduler logon task).
bool UpdateLogonTask(bool startOnLogin, StartupMode mode);
bool UpdateLogonTask(bool startOnLogin, const std::wstring& mode);
// Compatibility wrapper (defaults to Run mode when mode is omitted).
bool SetStartOnLogin(bool enabled, StartupMode mode = StartupMode::Run);
// The Run entry turned off in Task Manager's startup apps.
bool StartupTurnedOff();
bool IsStartupApprovedDisabled(const uint8_t* data, size_t size);
bool ShouldSyncStartupOnLaunch(bool development, bool registeredNow, bool startOnLogin, bool startupTurnedOff);
StartupPlan PlanStartupChange(const StartupState& state, bool enabled, StartupMode mode = StartupMode::Run);

} // namespace atrium
