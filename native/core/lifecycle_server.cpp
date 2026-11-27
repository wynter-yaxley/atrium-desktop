#include "lifecycle_server.h"

#include "host_messages.h"
#include "log.h"
#include "win32_path.h"

#include <sddl.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr DWORD kBuffer = 16 * 1024;
constexpr DWORD kMaxMessageBytes = 1024 * 1024;
constexpr DWORD kDrainTimeoutMs = 200;

std::wstring Widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string JsonStringify(const json::JsonObject& obj) { return Narrow(std::wstring(obj.Stringify())); }

std::string ErrorReply(const wchar_t* error)
{
    json::JsonObject o;
    o.SetNamedValue(L"ok", json::JsonValue::CreateBooleanValue(false));
    o.SetNamedValue(L"error", json::JsonValue::CreateStringValue(error));
    return JsonStringify(o);
}

std::string AcceptedReply()
{
    json::JsonObject o;
    o.SetNamedValue(L"ok", json::JsonValue::CreateBooleanValue(true));
    o.SetNamedValue(L"status", json::JsonValue::CreateStringValue(L"accepted"));
    return JsonStringify(o);
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

bool WaitOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, DWORD& bytes, DWORD& error)
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

bool ConnectPipe(HANDLE pipe, OVERLAPPED& ov, HANDLE quit)
{
    if (ConnectNamedPipe(pipe, &ov))
        return true;
    DWORD bytes = 0;
    DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING)
        return WaitOverlapped(pipe, ov, quit, bytes, error);
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
                if (!WaitOverlapped(pipe, ov, quit, bytes, error))
                    return false;
            } else if (error != ERROR_MORE_DATA) {
                return false;
            }
        } else {
            error = ERROR_SUCCESS;
        }
        message.append(buffer.data(), bytes);
    } while (error == ERROR_MORE_DATA && message.size() < kMaxMessageBytes);
    if (error == ERROR_MORE_DATA) {
        Log(L"lifecycle: request exceeded 1 MB, disconnecting");
        return false;
    }
    return !message.empty();
}

bool WritePipeMessage(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, const std::string& reply)
{
    ResetEvent(ov.hEvent);
    DWORD bytes = 0;
    if (!WriteFile(pipe, reply.data(), DWORD(reply.size()), &bytes, &ov)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING || !WaitOverlapped(pipe, ov, quit, bytes, error))
            return false;
    }
    return bytes == reply.size();
}

DWORD RemainingMs(ULONGLONG deadlineMs)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadlineMs)
        return 0;
    const ULONGLONG diff = deadlineMs - now;
    return diff > MAXDWORD ? MAXDWORD : static_cast<DWORD>(diff);
}

void DrainPipeClient(HANDLE pipe, OVERLAPPED& ov, HANDLE quit, DWORD timeoutMs)
{
    const ULONGLONG deadlineMs = GetTickCount64() + timeoutMs;
    char drain[64]{};
    for (;;) {
        const DWORD remaining = RemainingMs(deadlineMs);
        if (remaining == 0)
            return;
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (ReadFile(pipe, drain, sizeof(drain), &bytes, &ov))
            continue;
        const DWORD error = GetLastError();
        if (error == ERROR_MORE_DATA)
            continue;
        if (error != ERROR_IO_PENDING)
            return;
        HANDLE hs[] = {ov.hEvent, quit};
        if (WaitForMultipleObjects(2, hs, FALSE, remaining) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &bytes, TRUE);
            return;
        }
        if (!GetOverlappedResult(pipe, &ov, &bytes, FALSE)) {
            if (GetLastError() == ERROR_MORE_DATA)
                continue;
            return;
        }
    }
}

bool WaitClientOverlapped(HANDLE pipe, OVERLAPPED& ov, ULONGLONG deadlineMs, DWORD& bytes, DWORD& error)
{
    const DWORD remaining = RemainingMs(deadlineMs);
    if (WaitForSingleObject(ov.hEvent, remaining) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        error = ERROR_TIMEOUT;
        return false;
    }
    error = GetOverlappedResult(pipe, &ov, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    return error == ERROR_SUCCESS || error == ERROR_MORE_DATA;
}

std::wstring FileBaseName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool MatchesExpectedExecutable(HANDLE process, const std::wstring& expectedExePath)
{
    const std::wstring actual = ProcessImagePath(process);
    if (actual.empty())
        return false;
    if (expectedExePath.empty())
        return FoldCase(FileBaseName(actual)) == L"atrium.exe";
    if (expectedExePath.find_first_of(L"\\/") != std::wstring::npos)
        return FoldCase(actual) == FoldCase(expectedExePath);
    return FoldCase(FileBaseName(actual)) == FoldCase(expectedExePath);
}

bool VerifyPipeServerIdentity(HANDLE pipe, DWORD expectedPid, const std::wstring& expectedExePath)
{
    const auto& self = SelfSecurityContext();
    if (!self.valid)
        return false;
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid == 0)
        return false;
    if (expectedPid != 0 && serverPid != expectedPid)
        return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!proc)
        return false;
    const ProcessSecurityContext server = QueryProcessSecurityContext(proc);
    const bool exeOk = MatchesExpectedExecutable(proc, expectedExePath);
    CloseHandle(proc);
    if (!server.valid || server.sessionId != self.sessionId)
        return false;
    if (_wcsicmp(server.userSid.c_str(), self.userSid.c_str()) != 0)
        return false;
    if (!expectedExePath.empty() && !exeOk)
        return false;
    return expectedPid != 0 || exeOk;
}

bool VerifyImpersonatedClientSid(HANDLE pipe, const std::wstring& expectedSid)
{
    if (!ImpersonateNamedPipeClient(pipe))
        return false;
    HANDLE token = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != FALSE;
    RevertToSelf();
    if (!opened)
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    bool match = false;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        wchar_t* sidText = nullptr;
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        if (ConvertSidToStringSidW(user->User.Sid, &sidText) && sidText) {
            match = _wcsicmp(sidText, expectedSid.c_str()) == 0;
            LocalFree(sidText);
        }
    }
    CloseHandle(token);
    return match;
}

bool ParseLifecycleReply(const std::string& raw, LifecycleReply& out)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(raw), o))
        return false;
    out = {};
    if (o.HasKey(L"ok") && o.GetNamedValue(L"ok").ValueType() == json::JsonValueType::Boolean)
        out.ok = o.GetNamedBoolean(L"ok");
    if (o.HasKey(L"pong") && o.GetNamedValue(L"pong").ValueType() == json::JsonValueType::Boolean)
        out.pong = o.GetNamedBoolean(L"pong");
    if (o.HasKey(L"pid") && o.GetNamedValue(L"pid").ValueType() == json::JsonValueType::Number)
        out.pid = static_cast<DWORD>(o.GetNamedNumber(L"pid"));
    if (o.HasKey(L"canDeElevate") && o.GetNamedValue(L"canDeElevate").ValueType() == json::JsonValueType::Boolean)
        out.canDeElevate = o.GetNamedBoolean(L"canDeElevate");
    out.privilegeMode = std::wstring(o.GetNamedString(L"privilegeMode", L""));
    out.status = std::wstring(o.GetNamedString(L"status", L""));
    out.error = std::wstring(o.GetNamedString(L"error", L""));
    return true;
}

HANDLE OpenLifecyclePipe(const std::wstring& pipeName, ULONGLONG deadlineMs, DWORD expectedPid, const std::wstring& expectedExePath)
{
    for (;;) {
        HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            if (!VerifyPipeServerIdentity(pipe, expectedPid, expectedExePath)) {
                CloseHandle(pipe);
                return INVALID_HANDLE_VALUE;
            }
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
                CloseHandle(pipe);
                return INVALID_HANDLE_VALUE;
            }
            return pipe;
        }
        const DWORD err = GetLastError();
        const DWORD remaining = RemainingMs(deadlineMs);
        if (remaining == 0)
            return INVALID_HANDLE_VALUE;
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), remaining > 100 ? 100 : remaining);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            std::this_thread::sleep_for(std::chrono::milliseconds(remaining > 25 ? 25 : remaining));
        } else {
            return INVALID_HANDLE_VALUE;
        }
    }
}

} // namespace

std::wstring LifecyclePipeName()
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\Atrium.Lifecycle." + std::to_wstring(session);
}

LifecycleServer::LifecycleServer(HWND host, std::wstring pipeName, LifecycleCallbacks callbacks)
    : host_(host),
      pipeName_(pipeName.empty() ? LifecyclePipeName() : std::move(pipeName)),
      callbacks_(std::move(callbacks)),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (quit_)
        thread_ = std::thread([this] { Run(); });
}

LifecycleServer::~LifecycleServer()
{
    if (quit_)
        SetEvent(quit_);
    if (thread_.joinable())
        thread_.join();
    if (quit_)
        CloseHandle(quit_);
}

void LifecycleServer::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    KernelObjectSecurity security = KernelObjectSecurity::CreateSameUserPipe();
    if (!security.Valid()) {
        Log(L"lifecycle: could not build pipe security descriptor (%lu); failing closed", GetLastError());
        winrt::uninit_apartment();
        return;
    }
    while (WaitForSingleObject(quit_, 0) == WAIT_TIMEOUT) {
        // First instance only: another process cannot pre-create our pipe name.
        HANDLE pipe =
            CreateNamedPipeW(pipeName_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBuffer, kBuffer, 0, security.Get());
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"lifecycle: CreateNamedPipeW failed (%lu)", GetLastError());
            if (WaitForSingleObject(quit_, 2000) != WAIT_TIMEOUT)
                break;
            continue;
        }
        ready_ = true;
        Serve(pipe);
        CloseHandle(pipe);
    }
    winrt::uninit_apartment();
}

bool LifecycleServer::VerifyClientUser(HANDLE pipe) const
{
    const auto& self = SelfSecurityContext();
    if (!self.valid)
        return false;
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || !clientPid)
        return false;
    DWORD err = 0;
    const ProcessSecurityContext client = QueryPidSecurityContext(clientPid, &err);
    if (client.valid)
        return _wcsicmp(client.userSid.c_str(), self.userSid.c_str()) == 0;
    if (err == ERROR_ACCESS_DENIED)
        return VerifyImpersonatedClientSid(pipe, self.userSid);
    return false;
}

void LifecycleServer::Serve(HANDLE pipe)
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
    std::string message;
    if (!ReadPipeMessage(pipe, ov, quit_, buffer, message))
        return;
    if (!VerifyClientUser(pipe)) {
        Log(L"lifecycle: rejected client with mismatched or unknown user SID");
        return;
    }
    const std::string reply = Handle(message);
    if (WritePipeMessage(pipe, ov, quit_, reply))
        DrainPipeClient(pipe, ov, quit_, kDrainTimeoutMs);
}

std::string LifecycleServer::Handle(const std::string& request)
{
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(request), o) || !o.HasKey(L"cmd"))
        return ErrorReply(L"malformed_request");
    const std::wstring cmd(o.GetNamedString(L"cmd", L""));
    const PrivilegeMode mode = callbacks_.privilegeModeProvider ? callbacks_.privilegeModeProvider() : CurrentPrivilegeMode();

    if (cmd == L"ping") {
        json::JsonObject r;
        r.SetNamedValue(L"ok", json::JsonValue::CreateBooleanValue(true));
        r.SetNamedValue(L"pong", json::JsonValue::CreateBooleanValue(true));
        r.SetNamedValue(L"pid", json::JsonValue::CreateNumberValue(double(GetCurrentProcessId())));
        r.SetNamedValue(L"privilegeMode", json::JsonValue::CreateStringValue(PrivilegeModeWireName(mode)));
        r.SetNamedValue(L"canDeElevate", json::JsonValue::CreateBooleanValue(CanDeElevateToShell(mode)));
        return JsonStringify(r);
    }
    if (cmd == L"open-settings") {
        const std::wstring page(o.GetNamedString(L"page", L""));
        if (callbacks_.onOpenSettings)
            callbacks_.onOpenSettings(page);
        else if (host_)
            PostMessageW(host_, WM_OPEN_SETTINGS, 0, 0);
        return AcceptedReply();
    }
    if (cmd == L"quit") {
        // Post asynchronously and reply immediately so ShutdownApp can join our worker thread cleanly.
        if (callbacks_.onQuit)
            callbacks_.onQuit();
        else if (host_)
            PostMessageW(host_, WM_CLOSE, 0, 0);
        return AcceptedReply();
    }
    if (cmd == L"notify-update-ready") {
        if (callbacks_.onNotifyUpdateReady)
            callbacks_.onNotifyUpdateReady();
        else if (host_)
            PostMessageW(host_, WM_UPDATE_READY, 0, 0);
        return AcceptedReply();
    }
    if (cmd == L"restart-standard") {
        if (!CanDeElevateToShell(mode))
            return ErrorReply(L"cannot_deelevate");
        if (!callbacks_.onRestartStandard)
            return ErrorReply(L"not_configured");
        if (!callbacks_.onRestartStandard())
            return ErrorReply(L"restart_failed");
        if (callbacks_.onQuit)
            callbacks_.onQuit();
        else if (host_)
            PostMessageW(host_, WM_CLOSE, 0, 0);
        return AcceptedReply();
    }
    return ErrorReply(L"unknown_command");
}

bool SendLifecycleCommand(const std::wstring& cmd, const std::wstring& page, LifecycleReply* replyOut, const std::wstring& pipeName, DWORD timeoutMs,
                          DWORD expectedPid, const std::wstring& expectedExePath)
{
    if (replyOut)
        *replyOut = {};
    const ULONGLONG deadlineMs = GetTickCount64() + timeoutMs;
    const std::wstring target = pipeName.empty() ? LifecyclePipeName() : pipeName;
    HANDLE pipe = OpenLifecyclePipe(target, deadlineMs, expectedPid, expectedExePath);
    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    ScopedEvent event;
    if (!event.handle) {
        CloseHandle(pipe);
        return false;
    }

    json::JsonObject req;
    req.SetNamedValue(L"cmd", json::JsonValue::CreateStringValue(cmd));
    if (!page.empty())
        req.SetNamedValue(L"page", json::JsonValue::CreateStringValue(page));
    const std::string payload = JsonStringify(req);

    OVERLAPPED ov{};
    ov.hEvent = event.handle;
    DWORD written = 0;
    if (!WriteFile(pipe, payload.data(), DWORD(payload.size()), &written, &ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING || !WaitClientOverlapped(pipe, ov, deadlineMs, written, err)) {
            CloseHandle(pipe);
            return false;
        }
    }
    if (written != payload.size()) {
        CloseHandle(pipe);
        return false;
    }

    std::string reply;
    std::string chunk(kBuffer, '\0');
    for (;;) {
        ResetEvent(ov.hEvent);
        DWORD read = 0;
        DWORD err = ERROR_SUCCESS;
        if (!ReadFile(pipe, chunk.data(), DWORD(chunk.size()), &read, &ov)) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (!WaitClientOverlapped(pipe, ov, deadlineMs, read, err)) {
                    CloseHandle(pipe);
                    return false;
                }
            } else if (err != ERROR_MORE_DATA) {
                CloseHandle(pipe);
                return false;
            }
        }
        reply.append(chunk.data(), read);
        if (reply.size() > kMaxMessageBytes) {
            CloseHandle(pipe);
            return false;
        }
        if (err != ERROR_MORE_DATA)
            break;
    }
    CloseHandle(pipe);

    LifecycleReply parsed;
    if (!ParseLifecycleReply(reply, parsed))
        return false;
    if (replyOut)
        *replyOut = std::move(parsed);
    return true;
}

} // namespace atrium
