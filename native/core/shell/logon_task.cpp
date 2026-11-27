#include "shell/logon_task.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>

#include <winrt/base.h>

#include <iterator>
#include <mutex>

namespace atrium {

namespace {

struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), UINT(s.size()))) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
};

std::wstring UserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    std::wstring result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    wchar_t* text = nullptr;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}

// One task per user, as OneDrive names its own.
std::wstring TaskName(const std::wstring& sid) { return L"Atrium Startup-" + sid; }

bool ConnectTaskService(winrt::com_ptr<ITaskService>& service, winrt::com_ptr<ITaskFolder>& folder)
{
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.put()))))
        return false;
    VARIANT none{};
    VariantInit(&none);
    if (FAILED(service->Connect(none, none, none, none)))
        return false;
    return SUCCEEDED(service->GetFolder(Bstr(L"\\").b, folder.put())) && folder;
}

winrt::com_ptr<ITaskFolder> RootFolder()
{
    winrt::com_ptr<ITaskService> service;
    winrt::com_ptr<ITaskFolder> folder;
    return ConnectTaskService(service, folder) ? folder : nullptr;
}

bool ConfigureLogonTask(ITaskDefinition* def, const std::wstring& sid, const std::wstring& exe)
{
    winrt::com_ptr<IRegistrationInfo> reg;
    winrt::com_ptr<IPrincipal> principal;
    winrt::com_ptr<ITriggerCollection> triggers;
    winrt::com_ptr<ITrigger> trigger;
    winrt::com_ptr<ITaskSettings> settings;
    winrt::com_ptr<IActionCollection> actions;
    winrt::com_ptr<IAction> action;
    if (FAILED(def->get_RegistrationInfo(reg.put())) || FAILED(reg->put_Description(Bstr(L"Starts Atrium when you sign in.").b)) ||
        FAILED(def->get_Principal(principal.put())) || FAILED(principal->put_UserId(Bstr(sid).b)) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) || FAILED(principal->put_RunLevel(TASK_RUNLEVEL_LUA)) ||
        FAILED(def->get_Triggers(triggers.put())) || FAILED(triggers->Create(TASK_TRIGGER_LOGON, trigger.put())))
        return false;
    auto logon = trigger.try_as<ILogonTrigger>();
    if (!logon || FAILED(logon->put_Enabled(VARIANT_TRUE)) || FAILED(logon->put_UserId(Bstr(sid).b)))
        return false;
    if (FAILED(def->get_Settings(settings.put())) || FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) || FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_ExecutionTimeLimit(Bstr(L"PT0S").b)) || FAILED(settings->put_Priority(4)))
        return false;
    winrt::com_ptr<IIdleSettings> idle;
    if (SUCCEEDED(settings->get_IdleSettings(idle.put())) && idle) {
        idle->put_StopOnIdleEnd(VARIANT_FALSE);
        idle->put_RestartOnIdle(VARIANT_FALSE);
    }
    if (FAILED(def->get_Actions(actions.put())) || FAILED(actions->Create(TASK_ACTION_EXEC, action.put())))
        return false;
    auto exec = action.try_as<IExecAction>();
    return exec && SUCCEEDED(exec->put_Path(Bstr(exe).b));
}

bool RegisterLogonTask(const std::wstring& exe)
{
    ComScope com;
    const std::wstring sid = UserSid();
    if (sid.empty()) {
        Log(L"startup: could not resolve user SID for logon task");
        return false;
    }
    winrt::com_ptr<ITaskService> service;
    winrt::com_ptr<ITaskFolder> folder;
    winrt::com_ptr<ITaskDefinition> def;
    if (!ConnectTaskService(service, folder) || FAILED(service->NewTask(0, def.put()))) {
        Log(L"startup: could not connect to Task Scheduler for logon task");
        return false;
    }
    if (!ConfigureLogonTask(def.get(), sid, exe)) {
        Log(L"startup: could not configure logon task definition");
        return false;
    }
    VARIANT none{};
    VariantInit(&none);
    winrt::com_ptr<IRegisteredTask> registered;
    const HRESULT hr = folder->RegisterTaskDefinition(Bstr(TaskName(sid)).b, def.get(), TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, none,
                                                      registered.put());
    if (FAILED(hr)) {
        Log(L"startup: the logon task could not be registered (0x%08X)", unsigned(hr));
        return false;
    }
    return true;
}

} // namespace

StartupMode ParseStartupMode(const std::wstring& mode) { return mode == L"task" ? StartupMode::Task : StartupMode::Run; }

bool IsStartupApprovedDisabled(const uint8_t* data, size_t size)
{
    // The first byte is odd when the switch is off (2 on, 3 off).
    return data && size > 0 && (data[0] & 1) != 0;
}

bool ShouldSyncStartupOnLaunch(bool development, bool registeredNow, bool startOnLogin, bool startupTurnedOff)
{
    return !development && registeredNow && startOnLogin && !startupTurnedOff;
}

StartupPlan PlanStartupChange(const StartupState& state, bool enabled, StartupMode mode)
{
    if (state.exe.empty() || !state.taskReadOk)
        return {};
    const std::wstring command = L"\"" + state.exe + L"\"";
    const bool ownTask = !state.taskProgram.empty() && _wcsicmp(state.taskProgram.c_str(), state.exe.c_str()) == 0;
    const bool otherTask = !state.taskProgram.empty() && !ownTask;
    const bool ownRun = state.hasRunValue && _wcsicmp(state.runValue.c_str(), command.c_str()) == 0;
    const bool otherRun = state.hasRunValue && !ownRun;
    if (!enabled) {
        // Another distribution's entries are not ours to remove.
        if (!state.runReadOk || otherRun || otherTask)
            return {};
        return {.ok = true, .removeTask = ownTask, .writeRun = false, .deleteRun = state.hasRunValue, .createTask = false};
    }
    if (mode == StartupMode::Task) {
        if (!state.runReadOk)
            return {};
        return {.ok = true, .removeTask = false, .writeRun = false, .deleteRun = state.hasRunValue, .createTask = !ownTask};
    }
    return {.ok = true,
            .removeTask = !state.taskProgram.empty(),
            .writeRun = !state.hasRunValue || state.runValue != command,
            .deleteRun = false,
            .createTask = false};
}

namespace {
struct TaskQuery {
    std::wstring program;
    bool readOk = false;
};

TaskQuery QueryLogonTask()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    if (!folder)
        return {};
    winrt::com_ptr<IRegisteredTask> task;
    const HRESULT hr = folder->GetTask(Bstr(TaskName(sid)).b, task.put());
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return {{}, true};
    winrt::com_ptr<ITaskDefinition> definition;
    winrt::com_ptr<IActionCollection> actions;
    winrt::com_ptr<IAction> action;
    if (FAILED(hr) || !task || FAILED(task->get_Definition(definition.put())) || FAILED(definition->get_Actions(actions.put())) ||
        FAILED(actions->get_Item(1, action.put())))
        return {};
    auto exec = action.try_as<IExecAction>();
    BSTR path = nullptr;
    if (!exec || FAILED(exec->get_Path(&path)) || !path)
        return {};
    std::wstring result(path, SysStringLen(path));
    SysFreeString(path);
    if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"')
        result = result.substr(1, result.size() - 2);
    return {std::move(result), true};
}
} // namespace

std::wstring LogonTaskProgram() { return QueryLogonTask().program; }

bool RemoveLogonTask()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    if (!folder)
        return false;
    HRESULT hr = folder->DeleteTask(Bstr(TaskName(sid)).b, 0);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return true;
    Log(L"startup: could not remove logon task (0x%08X)", unsigned(hr));
    return false;
}

bool UpdateLogonTask(bool startOnLogin, StartupMode mode)
{
    if (IsDevelopmentBuild())
        return true;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    constexpr auto kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    wchar_t exe[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!length || length == std::size(exe))
        return false;
    wchar_t current[32771]{};
    DWORD size = sizeof(current);
    LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium", RRF_RT_REG_SZ, nullptr, current, &size);
    const TaskQuery taskQuery = QueryLogonTask();
    const StartupState state{
        .exe = exe,
        .taskProgram = taskQuery.program,
        .runValue = read == ERROR_SUCCESS ? current : std::wstring{},
        .hasRunValue = read == ERROR_SUCCESS,
        .runReadOk = read == ERROR_SUCCESS || read == ERROR_FILE_NOT_FOUND || read == ERROR_PATH_NOT_FOUND,
        .taskReadOk = taskQuery.readOk,
    };
    const StartupPlan plan = PlanStartupChange(state, startOnLogin, mode);
    if (!plan.ok) {
        Log(L"startup: skipped logon update because existing startup entries belong to another install or could not be read");
        return false;
    }
    if (plan.createTask && !RegisterLogonTask(state.exe))
        return false;
    if (plan.writeRun) {
        const std::wstring command = L"\"" + state.exe + L"\"";
        const LSTATUS result = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium", REG_SZ, command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t)));
        if (result != ERROR_SUCCESS) {
            Log(L"startup: could not write Run entry (%lu)", result);
            return false;
        }
    }
    if (plan.removeTask && !RemoveLogonTask())
        return false;
    if (plan.deleteRun) {
        const LSTATUS result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium");
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
            Log(L"startup: could not remove Run entry (%lu)", result);
            return false;
        }
    }
    return true;
}

bool UpdateLogonTask(bool startOnLogin, const std::wstring& mode) { return UpdateLogonTask(startOnLogin, ParseStartupMode(mode)); }

bool SetStartOnLogin(bool enabled, StartupMode mode) { return UpdateLogonTask(enabled, mode); }

bool StartupTurnedOff()
{
    BYTE state[12]{};
    DWORD size = sizeof(state);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", L"Atrium", RRF_RT_REG_BINARY,
                        nullptr, state, &size) == ERROR_SUCCESS &&
           IsStartupApprovedDisabled(state, size);
}

} // namespace atrium
