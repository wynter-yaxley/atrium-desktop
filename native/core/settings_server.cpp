#include "settings_server.h"

#include "localization.h"
#include "log.h"
#include "shell/process_integrity.h"
#include "shell/logon_task.h"
#include "ui/desktop_widgets.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr int kProtocol = 1;
constexpr DWORD kBuffer = 64 * 1024;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Reply(const wchar_t* op, const wchar_t* message = nullptr)
{
    json::JsonObject o;
    o.SetNamedValue(L"op", json::JsonValue::CreateStringValue(op));
    if (message)
        o.SetNamedValue(L"message", json::JsonValue::CreateStringValue(message));
    return Narrow(std::wstring(o.Stringify()));
}

std::wstring MakeInstanceId()
{
    static std::atomic<uint64_t> seq{1};
    return std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
           std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed));
}

bool MatchesExpected(const json::JsonObject& o, const std::wstring& instanceId, uint64_t revision)
{
    if (o.HasKey(L"instanceId")) {
        const auto v = o.GetNamedValue(L"instanceId");
        if (v.ValueType() != json::JsonValueType::String || std::wstring(v.GetString()) != instanceId)
            return false;
    }
    if (o.HasKey(L"expectedRevision")) {
        const auto v = o.GetNamedValue(L"expectedRevision");
        if (v.ValueType() != json::JsonValueType::Number || v.GetNumber() != double(revision))
            return false;
    }
    return true;
}

std::string ConflictReply(const std::wstring& instanceId, uint64_t revision, const Settings& saved, const Settings& current)
{
    json::JsonObject r;
    r.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"conflict"));
    r.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(instanceId));
    r.SetNamedValue(L"revision", json::JsonValue::CreateNumberValue(double(revision)));
    r.SetNamedValue(L"saved", json::JsonObject::Parse(saved.ToJson()));
    r.SetNamedValue(L"current", json::JsonObject::Parse(current.ToJson()));
    r.SetNamedValue(L"message", json::JsonValue::CreateStringValue(L"settings changed since last read"));
    return Narrow(std::wstring(r.Stringify()));
}

// Waits for an overlapped operation or the quit event. False on quit or error.
bool Wait(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, DWORD& bytes, DWORD& error)
{
    HANDLE hs[] = {ov.hEvent, quit};
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        error = ERROR_OPERATION_ABORTED;
        return false;
    }
    error = GetOverlappedResult(pipe, &ov, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    return error == ERROR_SUCCESS || error == ERROR_MORE_DATA;
}

struct ScopedEvent {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ScopedEvent()
    {
        if (handle)
            CloseHandle(handle);
    }
    ScopedEvent() = default;
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
};

struct PipeDisconnectGuard {
    HANDLE pipe;
    explicit PipeDisconnectGuard(HANDLE p) : pipe(p) {}
    ~PipeDisconnectGuard() { DisconnectNamedPipe(pipe); }
    PipeDisconnectGuard(const PipeDisconnectGuard&) = delete;
    PipeDisconnectGuard& operator=(const PipeDisconnectGuard&) = delete;
};

bool ConnectPipe(HANDLE pipe, OVERLAPPED& ov, HANDLE quit)
{
    if (ConnectNamedPipe(pipe, &ov))
        return true;
    DWORD bytes = 0;
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
        return Wait(pipe, ov, quit, bytes, error);
    return error == ERROR_PIPE_CONNECTED || error == ERROR_SUCCESS;
}

bool ReadPipeMessage(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, std::string& buffer, std::string& message)
{
    message.clear();
    DWORD error = 0;
    do {
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (!ReadFile(pipe, buffer.data(), kBuffer, &bytes, &ov)) {
            error = GetLastError();
            if (error == ERROR_IO_PENDING) {
                if (!Wait(pipe, ov, quit, bytes, error))
                    return false;
            } else if (error != ERROR_MORE_DATA) {
                return false; // client gone
            }
        } else {
            error = ERROR_SUCCESS;
        }
        message.append(buffer.data(), bytes);
    } while (error == ERROR_MORE_DATA && message.size() < 1024 * 1024);
    if (error == ERROR_MORE_DATA) {
        Log(L"pipe: request exceeded 1 MB, disconnecting");
        return false;
    }
    return true;
}

} // namespace

std::wstring SettingsPipeName()
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\Atrium.Core." + std::to_wstring(session);
}

SettingsServer::SettingsServer(HWND host, const Settings& saved, std::wstring pipeName, std::wstring settingsPath,
                               std::function<void(bool)> onAutoUpdateCommitted)
    : host_(host),
      pipeName_(pipeName.empty() ? SettingsPipeName() : std::move(pipeName)),
      settingsPath_(std::move(settingsPath)),
      instanceId_(MakeInstanceId()),
      saved_(saved),
      current_(saved),
      onAutoUpdateCommitted_(std::move(onAutoUpdateCommitted)),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    thread_ = std::thread([this] { Run(); });
}

SettingsServer::~SettingsServer()
{
    SetEvent(quit_);
    if (thread_.joinable())
        thread_.join();
    CloseHandle(quit_);
}

Settings SettingsServer::Current()
{
    std::lock_guard g(lock_);
    return current_;
}

Settings SettingsServer::Saved()
{
    std::lock_guard g(lock_);
    return saved_;
}

bool SettingsServer::Save(const Settings& s, std::wstring* warning, std::wstring* error) const
{
    if (warning)
        warning->clear();
    bool startupChanged = false;
    const bool ok = settingsPath_.empty() ? SaveSettings(s, error, &startupChanged) : SaveSettingsTo(settingsPath_, s, error);
    if (ok && settingsPath_.empty() && startupChanged && !UpdateLogonTask(s.startOnLogin, s.startupMode)) {
        Log(L"settings: settings saved, but Windows startup could not be changed");
        if (warning)
            *warning = Tr(L"Settings were saved, but Windows startup could not be changed. Toggle Start with Windows again, then save.");
    }
    return ok;
}

std::wstring SettingsServer::EffectiveSettingsPath() const { return settingsPath_.empty() ? SettingsPath() : settingsPath_; }

bool SettingsServer::Modify(const std::function<void(Settings&)>& change)
{
    std::lock_guard t(txn_);
    Settings saved, current;
    {
        std::lock_guard g(lock_);
        saved = saved_;
        current = current_;
    }
    change(saved);
    saved.Clamp();
    if (!Save(saved)) {
        Log(L"settings: could not save %s", EffectiveSettingsPath().c_str());
        return false;
    }
    change(current);
    current.Clamp();
    std::function<void(bool)> onAutoUpdateCommitted;
    {
        std::lock_guard g(lock_);
        saved_ = saved;
        ++revision_;
        onAutoUpdateCommitted = onAutoUpdateCommitted_;
    }
    if (onAutoUpdateCommitted)
        onAutoUpdateCommitted(saved.autoUpdate);
    SetCurrent(current);
    return true;
}

void SettingsServer::SetCurrent(const Settings& s)
{
    {
        std::lock_guard g(lock_);
        current_ = s;
    }
    if (host_)
        PostMessageW(host_, WM_ATRIUM_SETTINGS, 0, 0);
}

void SettingsServer::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    KernelObjectSecurity security = KernelObjectSecurity::CreateSameUserPipe();
    if (!security.Valid()) {
        Log(L"pipe: could not build pipe security descriptor (%lu); failing closed", GetLastError());
        winrt::uninit_apartment();
        return;
    }
    bool readyNotified = false;
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        // First instance only: another process cannot pre-create our pipe name.
        HANDLE pipe =
            CreateNamedPipeW(pipeName_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBuffer, kBuffer, 0, security.Get());
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"pipe: CreateNamedPipe failed (%lu)", GetLastError());
            if (WaitForSingleObject(quit_, 2000) != WAIT_TIMEOUT)
                break;
            continue;
        }
        ready_ = true;
        if (!readyNotified) {
            readyNotified = true;
            if (host_)
                PostMessageW(host_, WM_ATRIUM_SERVER_READY, 0, 0);
        }
        Serve(pipe);
        CloseHandle(pipe);

        // A client that leaves without committing gets its preview reverted.
        std::lock_guard t(txn_);
        Settings saved, current;
        {
            std::lock_guard g(lock_);
            saved = saved_;
            current = current_;
        }
        if (current.ToJson() != saved.ToJson()) {
            Log(L"pipe: client left with an unsaved preview, reverted");
            SetCurrent(saved);
        }
    }
    winrt::uninit_apartment();
}

void SettingsServer::Serve(HANDLE pipe)
{
    ScopedEvent event;
    if (!event.handle)
        return;
    OVERLAPPED ov{};
    ov.hEvent = event.handle;
    if (!ConnectPipe(pipe, ov, quit_))
        return;
    PipeDisconnectGuard disconnect{pipe};

    std::string buffer(kBuffer, '\0');
    for (;;) {
        std::string message;
        if (!ReadPipeMessage(pipe, ov, quit_, buffer, message))
            return;

        std::string reply = Handle(message);
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (!WriteFile(pipe, reply.data(), DWORD(reply.size()), &bytes, &ov)) {
            DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING || !Wait(pipe, ov, quit_, bytes, error))
                return;
        }
    }
}

std::string SettingsServer::Handle(const std::string& request)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(request), o) || !o.HasKey(L"op"))
        return Reply(L"error", L"malformed request");
    std::wstring op(o.GetNamedString(L"op", L""));

    if (op == L"add-widget") {
        // Not a settings change (no txn_: the UI thread may be waiting for it in Modify).
        if (!host_)
            return Reply(L"error", L"no room for another widget");
        std::wstring kind(o.GetNamedString(L"kind", L"")), size(o.GetNamedString(L"size", L"small"));
        WidgetKind widgetKind;
        WidgetSize widgetSize;
        if (!DesktopWidgets::ParseNames(kind, size, widgetKind, widgetSize))
            return Reply(L"error", L"unknown widget kind");
        DWORD_PTR added = 0;
        SendMessageTimeoutW(host_, WM_ATRIUM_ADD_WIDGET, WPARAM(widgetKind), LPARAM(widgetSize), SMTO_ABORTIFHUNG, 3000, &added);
        return added ? Reply(L"ok") : Reply(L"error", L"no room for another widget");
    }

    std::lock_guard t(txn_);
    Settings saved, current;
    uint64_t revision = 1;
    {
        std::lock_guard g(lock_);
        saved = saved_;
        current = current_;
        revision = revision_;
    }
    if (op == L"get") {
        json::JsonObject r;
        r.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"state"));
        r.SetNamedValue(L"protocol", json::JsonValue::CreateNumberValue(kProtocol));
        r.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(instanceId_));
        r.SetNamedValue(L"revision", json::JsonValue::CreateNumberValue(double(revision)));
        const PrivilegeMode privilegeMode = CurrentPrivilegeMode();
        r.SetNamedValue(L"elevated", json::JsonValue::CreateBooleanValue(IsElevatedProcess()));
        r.SetNamedValue(L"privilegeMode", json::JsonValue::CreateStringValue(PrivilegeModeWireName(privilegeMode)));
        r.SetNamedValue(L"canDeElevate", json::JsonValue::CreateBooleanValue(CanDeElevateToShell(privilegeMode)));
        r.SetNamedValue(L"saved", json::JsonObject::Parse(saved.ToJson()));
        r.SetNamedValue(L"current", json::JsonObject::Parse(current.ToJson()));
        r.SetNamedValue(L"path", json::JsonValue::CreateStringValue(EffectiveSettingsPath()));
        return Narrow(std::wstring(r.Stringify()));
    }
    if (op == L"quit") {
        if (!host_)
            return Reply(L"error", L"no host window");
        PostMessageW(host_, WM_CLOSE, 0, 0);
        return Reply(L"ok");
    }
    if (op == L"cancel") {
        Log(L"pipe: cancel, back to the saved settings");
        SetCurrent(saved);
        return Reply(L"ok");
    }
    if (op == L"preview" || op == L"commit") {
        if (!o.HasKey(L"settings") || o.GetNamedValue(L"settings").ValueType() != json::JsonValueType::Object)
            return Reply(L"error", L"missing settings object");
        Settings next;
        if (!Settings::FromJson(std::wstring(o.GetNamedObject(L"settings").Stringify()), current, next))
            return Reply(L"error", L"invalid settings");
        if (op == L"commit") {
            if (!MatchesExpected(o, instanceId_, revision))
                return ConflictReply(instanceId_, revision, saved, current);
            std::wstring warning, error;
            if (!Save(next, &warning, &error)) {
                Log(L"pipe: could not save settings (%s)", EffectiveSettingsPath().c_str());
                return Reply(L"error", error.empty() ? L"could not save settings" : error.c_str());
            }
            Log(L"pipe: commit saved");
            uint64_t nextRevision = 0;
            std::function<void(bool)> onAutoUpdateCommitted;
            {
                std::lock_guard g(lock_);
                saved_ = next;
                nextRevision = ++revision_;
                onAutoUpdateCommitted = onAutoUpdateCommitted_;
            }
            if (onAutoUpdateCommitted)
                onAutoUpdateCommitted(next.autoUpdate);
            SetCurrent(next);
            json::JsonObject reply;
            reply.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"ok"));
            reply.SetNamedValue(L"instanceId", json::JsonValue::CreateStringValue(instanceId_));
            reply.SetNamedValue(L"revision", json::JsonValue::CreateNumberValue(double(nextRevision)));
            if (!warning.empty())
                reply.SetNamedValue(L"warning", json::JsonValue::CreateStringValue(warning));
            return Narrow(std::wstring(reply.Stringify()));
        } // lock released before SetCurrent takes it again
        SetCurrent(next);
        return Reply(L"ok");
    }
    return Reply(L"error", L"unknown op");
}

} // namespace atrium
