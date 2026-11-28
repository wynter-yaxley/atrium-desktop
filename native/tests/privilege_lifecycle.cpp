#include "guard_process.h"
#include "launch_args.h"
#include "lifecycle_server.h"
#include "settings.h"
#include "settings_server.h"
#include "shell/process_integrity.h"
#include "win32_path.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace json = winrt::Windows::Data::Json;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct TempDir {
    fs::path path;
    explicit TempDir(const wchar_t* prefix)
    {
        wchar_t base[MAX_PATH]{};
        GetTempPathW(MAX_PATH, base);
        path = fs::path(base) / (std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

struct HandleCloser {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~HandleCloser()
    {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr)
            CloseHandle(handle);
    }
    HandleCloser() = default;
    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;
};

std::wstring Widen(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

template<class Server> bool WaitReady(const Server& server)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!server.Ready()) {
        if (GetTickCount64() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

HANDLE ConnectMessagePipe(const std::wstring& pipeName)
{
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
                CloseHandle(h);
                return INVALID_HANDLE_VALUE;
            }
            return h;
        }
        const DWORD err = GetLastError();
        if (GetTickCount64() >= deadline)
            return INVALID_HANDLE_VALUE;
        if (err == ERROR_PIPE_BUSY || err == ERROR_FILE_NOT_FOUND)
            WaitNamedPipeW(pipeName.c_str(), 100);
        else
            return INVALID_HANDLE_VALUE;
    }
}

bool TransactJson(HANDLE pipe, const json::JsonObject& req, json::JsonObject& out)
{
    const std::string payload = Narrow(std::wstring(req.Stringify()));
    DWORD written = 0;
    if (!WriteFile(pipe, payload.data(), DWORD(payload.size()), &written, nullptr) || written != payload.size())
        return false;
    std::string reply;
    std::string chunk(16 * 1024, '\0');
    for (;;) {
        DWORD read = 0;
        const bool ok = ReadFile(pipe, chunk.data(), DWORD(chunk.size()), &read, nullptr) != FALSE;
        const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err != ERROR_MORE_DATA)
            return false;
        reply.append(chunk.data(), read);
        if (err != ERROR_MORE_DATA)
            break;
    }
    return json::JsonObject::TryParse(Widen(reply), out);
}

void CheckIntegrityAndPrivilegeClassification()
{
    using namespace atrium;
    Check(CompareIntegrity(0, SECURITY_MANDATORY_MEDIUM_RID) == IntegrityRelation::Unknown, "CompareIntegrity(0, medium) is Unknown");
    Check(CompareIntegrity(SECURITY_MANDATORY_MEDIUM_RID, 0) == IntegrityRelation::Unknown, "CompareIntegrity(medium, 0) is Unknown");
    Check(CompareIntegrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID) == IntegrityRelation::Lower, "CompareIntegrity(medium, high) is Lower");
    Check(CompareIntegrity(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_HIGH_RID) == IntegrityRelation::Equal, "CompareIntegrity(high, high) is Equal");
    Check(CompareIntegrity(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID) == IntegrityRelation::Higher,
          "CompareIntegrity(high, medium) is Higher");

    ProcessSecurityContext stdUser;
    stdUser.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
    stdUser.userSid = L"S-1-5-21-1000-1";
    stdUser.isElevated = false;
    stdUser.valid = true;

    ProcessSecurityContext elevSameUser;
    elevSameUser.integrityRid = SECURITY_MANDATORY_HIGH_RID;
    elevSameUser.userSid = L"S-1-5-21-1000-1";
    elevSameUser.isElevated = true;
    elevSameUser.valid = true;

    ProcessSecurityContext elevOtherUser;
    elevOtherUser.integrityRid = SECURITY_MANDATORY_HIGH_RID;
    elevOtherUser.userSid = L"S-1-5-21-1000-2";
    elevOtherUser.isElevated = true;
    elevOtherUser.valid = true;

    ProcessSecurityContext invalid;

    Check(ClassifyPrivilegeMode(stdUser, stdUser) == PrivilegeMode::Standard, "Standard privilege mode classified");
    Check(!CanDeElevateToShell(PrivilegeMode::Standard), "Standard cannot de-elevate");
    Check(std::wstring(PrivilegeModeWireName(PrivilegeMode::Standard)) == L"standard", "Standard wire name");

    Check(ClassifyPrivilegeMode(elevSameUser, stdUser) == PrivilegeMode::ElevatedSplit, "ElevatedSplit privilege mode classified");
    Check(CanDeElevateToShell(PrivilegeMode::ElevatedSplit), "ElevatedSplit can de-elevate");
    Check(std::wstring(PrivilegeModeWireName(PrivilegeMode::ElevatedSplit)) == L"elevated_split", "ElevatedSplit wire name");

    Check(ClassifyPrivilegeMode(elevSameUser, elevSameUser) == PrivilegeMode::ElevatedUnified, "ElevatedUnified privilege mode classified");
    Check(!CanDeElevateToShell(PrivilegeMode::ElevatedUnified), "ElevatedUnified cannot de-elevate");
    Check(std::wstring(PrivilegeModeWireName(PrivilegeMode::ElevatedUnified)) == L"elevated_unified", "ElevatedUnified wire name");

    Check(ClassifyPrivilegeMode(elevOtherUser, stdUser) == PrivilegeMode::CrossAccountElevated, "CrossAccountElevated privilege mode classified");
    Check(!CanDeElevateToShell(PrivilegeMode::CrossAccountElevated), "CrossAccountElevated cannot de-elevate");
    Check(std::wstring(PrivilegeModeWireName(PrivilegeMode::CrossAccountElevated)) == L"cross_account_elevated", "CrossAccountElevated wire name");

    Check(ClassifyPrivilegeMode(invalid, stdUser) == PrivilegeMode::Unknown, "Invalid self yields Unknown");
    Check(ClassifyPrivilegeMode(elevSameUser, invalid) == PrivilegeMode::Unknown, "Invalid shell yields Unknown");
    Check(!CanDeElevateToShell(PrivilegeMode::Unknown), "Unknown cannot de-elevate");
    Check(std::wstring(PrivilegeModeWireName(PrivilegeMode::Unknown)) == L"unknown", "Unknown wire name");

    Check(SelfSecurityContext().valid, "SelfSecurityContext is valid");
    if (GetShellWindow() != nullptr) {
        const ShellProcessIdentity shell = ResolveShellProcessIdentity();
        Check(shell.valid && shell.pid != 0, "ResolveShellProcessIdentity succeeds when shell window exists");
        Check(CurrentPrivilegeMode() != PrivilegeMode::Unknown, "CurrentPrivilegeMode is known when shell exists");
    }
}

void CheckPipeAndMutexAcls()
{
    using namespace atrium;
    KernelObjectSecurity invalid = KernelObjectSecurity::FromSddl(L"not-valid-sddl");
    Check(!invalid.Valid() && invalid.Get() == nullptr, "invalid SDDL fails closed without returning a SECURITY_ATTRIBUTES");

    const std::wstring emptyPipeSddl = BuildSameUserPipeSddl(L"", true);
    Check(emptyPipeSddl.empty(), "empty user SID produces empty pipe SDDL");

    KernelObjectSecurity pipeSec = KernelObjectSecurity::CreateSameUserPipe();
    Check(pipeSec.Valid() && pipeSec.Get() != nullptr, "CreateSameUserPipe succeeds");
    if (!pipeSec.Valid())
        return;

    const std::wstring pipeName = L"\\\\.\\pipe\\Atrium.AclTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HandleCloser server;
    server.handle = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, pipeSec.Get());
    Check(server.handle != INVALID_HANDLE_VALUE, "CreateNamedPipeW succeeds with same-user pipe ACL");
    if (server.handle == INVALID_HANDLE_VALUE)
        return;

    PSID ownerSid = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD secInfoErr =
        GetSecurityInfo(server.handle, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &ownerSid, nullptr, &dacl, nullptr, &sd);
    wchar_t* ownerSidText = nullptr;
    const bool ownerMatchesSelf = secInfoErr == ERROR_SUCCESS && ownerSid != nullptr && ConvertSidToStringSidW(ownerSid, &ownerSidText) != FALSE &&
                                  ownerSidText != nullptr && _wcsicmp(ownerSidText, SelfSecurityContext().userSid.c_str()) == 0;
    if (ownerSidText)
        LocalFree(ownerSidText);
    Check(ownerMatchesSelf, "pipe owner SID matches current user TokenUser SID");
    Check(secInfoErr == ERROR_SUCCESS && dacl != nullptr, "pipe has explicit DACL");
    if (dacl) {
        ACL_SIZE_INFORMATION info{};
        Check(GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) != FALSE && info.AceCount == 1, "pipe DACL contains exactly one same-user ACE");
        LPVOID rawAce = nullptr;
        if (info.AceCount >= 1 && GetAce(dacl, 0, &rawAce)) {
            const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
            Check(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE, "pipe ACE is ACCESS_ALLOWED");
            Check((ace->Mask & GENERIC_ALL) == 0, "pipe ACE does not grant GENERIC_ALL");
            Check((ace->Mask & FILE_CREATE_PIPE_INSTANCE) == 0, "pipe ACE does not grant FILE_CREATE_PIPE_INSTANCE");
            const DWORD required = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;
            Check((ace->Mask & required) == required, "pipe ACE grants required duplex client rights");
        }
    }
    if (sd)
        LocalFree(sd);

    HandleCloser hijacker;
    hijacker.handle = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 4096, 4096, 0, pipeSec.Get());
    Check(hijacker.handle == INVALID_HANDLE_VALUE, "second CreateNamedPipeW(FILE_FLAG_FIRST_PIPE_INSTANCE) is rejected");

    const std::wstring negMultiPipe = pipeName + L".NegMulti";
    constexpr DWORD kMultiOpenMode = PIPE_ACCESS_DUPLEX;
    constexpr DWORD kMultiPipeMode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    HandleCloser negFirst;
    negFirst.handle = CreateNamedPipeW(negMultiPipe.c_str(), kMultiOpenMode, kMultiPipeMode, 2, 4096, 4096, 0, pipeSec.Get());
    Check(negFirst.handle != INVALID_HANDLE_VALUE, "first multi-instance CreateNamedPipeW with tightened ACL succeeds");
    HandleCloser negSecond;
    negSecond.handle = CreateNamedPipeW(negMultiPipe.c_str(), kMultiOpenMode, kMultiPipeMode, 2, 4096, 4096, 0, pipeSec.Get());
    Check(negSecond.handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
          "second identical CreateNamedPipeW(nMaxInstances=2) without FILE_CREATE_PIPE_INSTANCE is denied");

    const auto& selfCtx = SelfSecurityContext();
    std::wstring allowCreateSddl = L"D:P(A;;0x0012019F;;;" + selfCtx.userSid + L")";
    if (selfCtx.integrityRid >= SECURITY_MANDATORY_MEDIUM_RID)
        allowCreateSddl += L"S:(ML;;NW;;;ME)";
    KernelObjectSecurity allowCreateSec = KernelObjectSecurity::FromSddl(allowCreateSddl);
    Check(allowCreateSec.Valid(), "FromSddl with FILE_CREATE_PIPE_INSTANCE (0x0012019F) succeeds");
    if (allowCreateSec.Valid()) {
        const std::wstring posMultiPipe = pipeName + L".PosMulti";
        HandleCloser posFirst;
        posFirst.handle = CreateNamedPipeW(posMultiPipe.c_str(), kMultiOpenMode, kMultiPipeMode, 2, 4096, 4096, 0, allowCreateSec.Get());
        Check(posFirst.handle != INVALID_HANDLE_VALUE, "first multi-instance CreateNamedPipeW with FILE_CREATE_PIPE_INSTANCE succeeds");
        HandleCloser posSecond;
        posSecond.handle = CreateNamedPipeW(posMultiPipe.c_str(), kMultiOpenMode, kMultiPipeMode, 2, 4096, 4096, 0, allowCreateSec.Get());
        Check(posSecond.handle != INVALID_HANDLE_VALUE,
              "second identical CreateNamedPipeW(nMaxInstances=2) with FILE_CREATE_PIPE_INSTANCE (0x0012019F) succeeds");
    }

    std::thread serverThread([srv = server.handle] {
        if (ConnectNamedPipe(srv, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[64]{};
            DWORD read = 0;
            if (ReadFile(srv, buf, sizeof(buf), &read, nullptr) && read > 0) {
                DWORD written = 0;
                WriteFile(srv, buf, read, &written, nullptr);
                FlushFileBuffers(srv);
            }
            DisconnectNamedPipe(srv);
        }
    });

    HandleCloser client;
    client.handle = ConnectMessagePipe(pipeName);
    Check(client.handle != INVALID_HANDLE_VALUE, "duplex client connects to hardened pipe");
    if (client.handle != INVALID_HANDLE_VALUE) {
        const char msg[] = "hello";
        DWORD written = 0;
        Check(WriteFile(client.handle, msg, 5, &written, nullptr) != FALSE && written == 5, "duplex client writes to hardened pipe");
        char reply[16]{};
        DWORD read = 0;
        Check(ReadFile(client.handle, reply, sizeof(reply), &read, nullptr) != FALSE && read == 5 && std::string(reply, read) == "hello",
              "duplex client reads from hardened pipe");
    }
    serverThread.join();
}

void CheckMutexAcl()
{
    using namespace atrium;
    KernelObjectSecurity mutexSec = KernelObjectSecurity::CreateSameUserMutex();
    Check(mutexSec.Valid() && mutexSec.Get() != nullptr, "CreateSameUserMutex succeeds");
    const std::wstring mutexName = L"Local\\Atrium.MutexAclTest." + std::to_wstring(GetCurrentProcessId());
    HandleCloser mutex;
    mutex.handle = CreateMutexExW(mutexSec.Get(), mutexName.c_str(), CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    Check(mutex.handle != nullptr, "CreateMutexExW succeeds with same-user mutex ACL");
    if (mutex.handle) {
        PACL mDacl = nullptr;
        PSECURITY_DESCRIPTOR mSd = nullptr;
        if (GetSecurityInfo(mutex.handle, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &mDacl, nullptr, &mSd) == ERROR_SUCCESS &&
            mDacl != nullptr) {
            LPVOID rawAce = nullptr;
            if (GetAce(mDacl, 0, &rawAce)) {
                const auto* ace = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
                Check((ace->Mask & GENERIC_ALL) == 0, "mutex ACE does not grant GENERIC_ALL");
                Check((ace->Mask & MUTEX_ALL_ACCESS) == MUTEX_ALL_ACCESS, "mutex ACE grants MUTEX_ALL_ACCESS");
            }
        }
        if (mSd)
            LocalFree(mSd);
    }
}

void CheckLifecycleAndSettingsServers()
{
    using namespace atrium;
    TempDir temp(L"atrium-lifecycle-test-");
    const fs::path cfgPath = temp.path / L"settings.json";
    Settings initial;
    Check(SaveSettingsTo(cfgPath.wstring(), initial), "seed temp settings.json");

    const std::wstring settingsPipe =
        L"\\\\.\\pipe\\Atrium.Settings.LifecycleTest." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const std::wstring lifecyclePipe =
        L"\\\\.\\pipe\\Atrium.Lifecycle.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());

    SettingsServer settingsServer(nullptr, initial, settingsPipe, cfgPath.wstring());
    Check(WaitReady(settingsServer), "SettingsServer becomes ready");

    // Hold a SettingsServer client connection open across all LifecycleServer checks.
    HandleCloser settingsClient;
    settingsClient.handle = ConnectMessagePipe(settingsPipe);
    Check(settingsClient.handle != INVALID_HANDLE_VALUE, "SettingsServer client connects");
    if (settingsClient.handle != INVALID_HANDLE_VALUE) {
        json::JsonObject getReq;
        getReq.SetNamedValue(L"op", json::JsonValue::CreateStringValue(L"get"));
        json::JsonObject state{nullptr};
        Check(TransactJson(settingsClient.handle, getReq, state), "SettingsServer get succeeds");
        const PrivilegeMode expectedMode = CurrentPrivilegeMode();
        Check(state.GetNamedString(L"privilegeMode", L"") == PrivilegeModeWireName(expectedMode), "SettingsServer get reports expected privilegeMode");
        Check(state.HasKey(L"canDeElevate") && state.GetNamedBoolean(L"canDeElevate") == CanDeElevateToShell(expectedMode),
              "SettingsServer get reports expected canDeElevate");
    }

    std::atomic<PrivilegeMode> simulatedMode{PrivilegeMode::Standard};
    std::wstring openedPage;
    std::atomic<int> quitCount{0};
    std::atomic<int> restartCount{0};
    std::atomic<int> notifyUpdateCount{0};

    LifecycleServer lifecycleServer(nullptr, lifecyclePipe,
                                    LifecycleCallbacks{
                                        .onOpenSettings = [&](const std::wstring& page) { openedPage = page; },
                                        .onQuit = [&] { quitCount.fetch_add(1); },
                                        .onRestartStandard =
                                            [&] {
                                                restartCount.fetch_add(1);
                                                return true;
                                            },
                                        .onNotifyUpdateReady = [&] { notifyUpdateCount.fetch_add(1); },
                                        .privilegeModeProvider = [&] { return simulatedMode.load(); },
                                    });
    Check(WaitReady(lifecycleServer), "LifecycleServer becomes ready");

    const std::wstring selfExe = ModulePath();
    const DWORD selfPid = GetCurrentProcessId();

    LifecycleReply pingReply;
    Check(SendLifecycleCommand(L"ping", {}, &pingReply, lifecyclePipe, 2000, selfPid, selfExe) && pingReply.ok && pingReply.pong && pingReply.pid == selfPid &&
              pingReply.privilegeMode == L"standard" && !pingReply.canDeElevate,
          "LifecycleServer ping succeeds with SQOS + server identity verification while SettingsServer client is open");
    LifecycleReply exeOnlyReply;
    Check(SendLifecycleCommand(L"ping", {}, &exeOnlyReply, lifecyclePipe, 2000, 0, selfExe) && exeOnlyReply.ok && exeOnlyReply.pong,
          "LifecycleServer ping succeeds when verifying expectedExePath without explicit PID");
    const std::wstring otherDirSameExe = L"C:\\OtherDir\\" + fs::path(selfExe).filename().wstring();
    for (const auto& [expPid, expPath, msg] : std::vector<std::tuple<DWORD, std::wstring, const char*>>{
             {selfPid + 1, selfExe, "SendLifecycleCommand rejects mismatched expectedPid before sending request"},
             {0, L"C:\\invalid\\NotAtrium.exe", "SendLifecycleCommand rejects mismatched expectedExePath before sending request"},
             {0, L"", "SendLifecycleCommand rejects non-Atrium.exe server when expectedExePath defaults to Atrium.exe"},
             {selfPid, L"C:\\invalid\\NotAtrium.exe", "SendLifecycleCommand rejects matching expectedPid when expectedExePath mismatches"},
             {selfPid, L"", "SendLifecycleCommand rejects matching expectedPid on non-Atrium.exe server when expectedExePath is empty"},
             {0, otherDirSameExe, "SendLifecycleCommand rejects different-directory path with matching basename when expectedPid=0"},
             {selfPid, otherDirSameExe, "SendLifecycleCommand rejects different-directory path with matching basename even when expectedPid == selfPid"},
         }) {
        LifecycleReply rejected;
        Check(!SendLifecycleCommand(L"ping", {}, &rejected, lifecyclePipe, 500, expPid, expPath), msg);
    }

    LifecycleReply openReply;
    Check(SendLifecycleCommand(L"open-settings", L"widgets", &openReply, lifecyclePipe, 2000, selfPid, selfExe) && openReply.ok &&
              openReply.status == L"accepted" && openedPage == L"widgets",
          "LifecycleServer open-settings dispatches page");

    LifecycleReply zeroTimeoutReply;
    const int notifyBeforeZeroTimeout = notifyUpdateCount.load();
    Check(!SendLifecycleCommand(L"notify-update-ready", {}, &zeroTimeoutReply, lifecyclePipe, 0, selfPid, selfExe) &&
              notifyUpdateCount.load() == notifyBeforeZeroTimeout,
          "SendLifecycleCommand with timeoutMs=0 fails cleanly without sending request to ready server");

    LifecycleReply notifyReply;
    Check(SendLifecycleCommand(L"notify-update-ready", {}, &notifyReply, lifecyclePipe, 2000, selfPid, selfExe) && notifyReply.ok &&
              notifyReply.status == L"accepted" && notifyUpdateCount.load() == 1,
          "LifecycleServer notify-update-ready dispatches callback");

    LifecycleReply denyRestart;
    Check(SendLifecycleCommand(L"restart-standard", {}, &denyRestart, lifecyclePipe, 2000, selfPid, selfExe) && !denyRestart.ok &&
              denyRestart.error == L"cannot_deelevate" && restartCount.load() == 0,
          "LifecycleServer rejects restart-standard when not ElevatedSplit");

    simulatedMode.store(PrivilegeMode::ElevatedSplit);
    LifecycleReply splitPing;
    Check(SendLifecycleCommand(L"ping", {}, &splitPing, lifecyclePipe, 2000, selfPid, selfExe) && splitPing.ok &&
              splitPing.privilegeMode == L"elevated_split" && splitPing.canDeElevate,
          "LifecycleServer ping reflects ElevatedSplit canDeElevate=true");

    LifecycleReply allowRestart;
    Check(SendLifecycleCommand(L"restart-standard", {}, &allowRestart, lifecyclePipe, 2000, selfPid, selfExe) && allowRestart.ok &&
              allowRestart.status == L"accepted" && restartCount.load() == 1 && quitCount.load() == 1,
          "LifecycleServer allows restart-standard in ElevatedSplit and signals quit");

    LifecycleReply quitReply;
    Check(SendLifecycleCommand(L"quit", {}, &quitReply, lifecyclePipe, 2000, selfPid, selfExe) && quitReply.ok && quitReply.status == L"accepted" &&
              quitCount.load() == 2,
          "LifecycleServer quit replies accepted immediately while SettingsServer client remains open");
}

void CheckLifecyclePausedAndStalledCases()
{
    using namespace atrium;
    const std::wstring selfExe = ModulePath();
    const DWORD selfPid = GetCurrentProcessId();

    // P1-4: paused client that writes ping and never reads reply must not stall LifecycleServer teardown.
    {
        const std::wstring pausedPipe = L"\\\\.\\pipe\\Atrium.Lifecycle.PausedTest." + std::to_wstring(selfPid) + L"." + std::to_wstring(GetTickCount64());
        auto pausedServer = std::make_unique<LifecycleServer>(nullptr, pausedPipe);
        Check(WaitReady(*pausedServer), "Paused-client LifecycleServer becomes ready");
        HandleCloser pausedClient;
        pausedClient.handle = ConnectMessagePipe(pausedPipe);
        Check(pausedClient.handle != INVALID_HANDLE_VALUE, "Paused client connects to LifecycleServer");
        if (pausedClient.handle != INVALID_HANDLE_VALUE) {
            const char pingPayload[] = R"({"cmd":"ping"})";
            DWORD written = 0;
            Check(WriteFile(pausedClient.handle, pingPayload, DWORD(sizeof(pingPayload) - 1), &written, nullptr) != FALSE,
                  "Paused client writes ping without reading reply");
            HandleCloser pauseTimer;
            pauseTimer.handle = CreateWaitableTimerW(nullptr, TRUE, nullptr);
            if (pauseTimer.handle) {
                LARGE_INTEGER due{};
                due.QuadPart = -30LL * 10000LL; // 30 ms relative
                if (SetWaitableTimer(pauseTimer.handle, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(pauseTimer.handle, 30);
            }
        }
        const ULONGLONG stopStart = GetTickCount64();
        pausedServer.reset();
        const ULONGLONG stopElapsed = GetTickCount64() - stopStart;
        Check(stopElapsed < 500, "LifecycleServer teardown completes in < 500ms even with paused unread client");
    }

    // P1-5: stalled mock pipe server must be aborted inside bounded overlapped timeoutMs.
    KernelObjectSecurity pipeSec = KernelObjectSecurity::CreateSameUserPipe();
    Check(pipeSec.Valid(), "CreateSameUserPipe succeeds for stalled mock server");
    if (!pipeSec.Valid())
        return;
    const std::wstring stalledPipe = L"\\\\.\\pipe\\Atrium.Lifecycle.StalledTest." + std::to_wstring(selfPid) + L"." + std::to_wstring(GetTickCount64());
    HandleCloser mockSrv;
    mockSrv.handle = CreateNamedPipeW(stalledPipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                      PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, pipeSec.Get());
    Check(mockSrv.handle != INVALID_HANDLE_VALUE, "Stalled mock pipe created");
    HandleCloser stopEvent;
    stopEvent.handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (mockSrv.handle == INVALID_HANDLE_VALUE || !stopEvent.handle)
        return;
    std::thread mockThread([srv = mockSrv.handle, stop = stopEvent.handle] {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
            return;
        bool connected = ConnectNamedPipe(srv, &ov) != FALSE;
        if (!connected && GetLastError() == ERROR_PIPE_CONNECTED)
            connected = true;
        else if (!connected && GetLastError() == ERROR_IO_PENDING) {
            HANDLE hs[] = {ov.hEvent, stop};
            DWORD unused = 0;
            if (WaitForMultipleObjects(2, hs, FALSE, 2000) == WAIT_OBJECT_0)
                connected = GetOverlappedResult(srv, &ov, &unused, FALSE) != FALSE;
            else
                CancelIoEx(srv, &ov);
        }
        if (connected) {
            ResetEvent(ov.hEvent);
            char buf[256]{};
            DWORD read = 0;
            if (!ReadFile(srv, buf, sizeof(buf), &read, &ov) && GetLastError() == ERROR_IO_PENDING) {
                HANDLE hs[] = {ov.hEvent, stop};
                if (WaitForMultipleObjects(2, hs, FALSE, 2000) != WAIT_OBJECT_0)
                    CancelIoEx(srv, &ov);
            }
            // Intentionally stall without writing a reply until test signals stop.
            WaitForSingleObject(stop, 2000);
            DisconnectNamedPipe(srv);
        }
        CloseHandle(ov.hEvent);
    });

    const ULONGLONG reqStart = GetTickCount64();
    LifecycleReply stalledReply;
    const bool sent = SendLifecycleCommand(L"ping", {}, &stalledReply, stalledPipe, 150, selfPid, selfExe);
    const ULONGLONG reqElapsed = GetTickCount64() - reqStart;
    Check(!sent && reqElapsed < 400, "SendLifecycleCommand aborts stalled mock pipe server within < 400ms");

    SetEvent(stopEvent.handle);
    mockThread.join();
}

void CheckGuardInstanceGeneration()
{
    using namespace atrium;
    TempDir temp(L"atrium-guard-gen-test-");
    const std::wstring genPath = (temp.path / L"instance.gen").wstring();

    Check(ReadInstanceGeneration(genPath) == 0, "missing generation file reads as 0");
    const std::optional<uint64_t> g1 = AdvanceInstanceGeneration(genPath);
    Check(g1.has_value() && *g1 == 1 && ReadInstanceGeneration(genPath) == 1 && ActiveInstanceGeneration() == 1, "first AdvanceInstanceGeneration writes 1");
    const std::optional<uint64_t> g2 = AdvanceInstanceGeneration(genPath);
    Check(g2.has_value() && *g2 == 2 && ReadInstanceGeneration(genPath) == 2 && ActiveInstanceGeneration() == 2, "second AdvanceInstanceGeneration writes 2");

    // Persisting to a directory path fails cleanly and leaves ActiveInstanceGeneration unchanged.
    const std::optional<uint64_t> failed = AdvanceInstanceGeneration(temp.path.wstring());
    Check(!failed.has_value() && ActiveInstanceGeneration() == 2 && ReadInstanceGeneration(genPath) == 2,
          "AdvanceInstanceGeneration fails cleanly without advancing in-memory generation on disk failure");

    Check(ShouldGuardUndoLeftover(*g2, ReadInstanceGeneration(genPath)), "matching generation allows guard undo");
    Check(!ShouldGuardUndoLeftover(*g1, ReadInstanceGeneration(genPath)), "stale generation skips guard undo");
    Check(ShouldGuardUndoLeftover(0, ReadInstanceGeneration(genPath)), "legacy zero generation allows guard undo");
    Check(ShouldGuardUndoLeftover(*g2, 0), "missing active generation allows guard undo");
}

void CheckDesktopAgentLaunchArgs()
{
    using namespace atrium;
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--desktop-agent", L"4242", L"--agent-gen", L"99"});
        Check(opt.desktopAgent && opt.desktopAgentCorePid == 4242 && opt.desktopAgentGeneration == 99 && !opt.guard && opt.guardPid == 0,
              "--desktop-agent 4242 --agent-gen 99 parses cleanly");
    }
    {
        const CoreLaunchOptions opt =
            ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"D:\\tests\\--desktop-agent 111", L"--desktop-agent", L"4242", L"--agent-gen", L"99"});
        Check(opt.desktopAgent && opt.desktopAgentCorePid == 4242 && opt.desktopAgentGeneration == 99,
              "--desktop-agent and --agent-gen parse cleanly when combined with --data-dir");
    }
    {
        const CoreLaunchOptions inDataDir = ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"D:\\tests\\--desktop-agent 111 --agent-gen 7"});
        Check(!inDataDir.desktopAgent && inDataDir.desktopAgentCorePid == 0 && inDataDir.desktopAgentGeneration == 0,
              "--data-dir value containing --desktop-agent must not trigger desktopAgent");
    }
    {
        const CoreLaunchOptions missing = ParseCoreLaunchOptions({L"Atrium.exe", L"--desktop-agent"});
        const CoreLaunchOptions zero = ParseCoreLaunchOptions({L"Atrium.exe", L"--desktop-agent", L"0"});
        const CoreLaunchOptions badDigits = ParseCoreLaunchOptions({L"Atrium.exe", L"--desktop-agent", L"12abc"});
        const CoreLaunchOptions nextFlag = ParseCoreLaunchOptions({L"Atrium.exe", L"--desktop-agent", L"--exit"});
        Check(missing.desktopAgent && missing.desktopAgentCorePid == 0, "bare --desktop-agent leaves desktopAgentCorePid=0");
        Check(zero.desktopAgent && zero.desktopAgentCorePid == 0, "--desktop-agent 0 leaves desktopAgentCorePid=0");
        Check(badDigits.desktopAgent && badDigits.desktopAgentCorePid == 0, "--desktop-agent 12abc leaves desktopAgentCorePid=0");
        Check(nextFlag.desktopAgent && nextFlag.desktopAgentCorePid == 0 && nextFlag.exitRequested,
              "--desktop-agent followed by --exit leaves desktopAgentCorePid=0 and parses --exit");
    }
    {
        const CoreLaunchOptions zeroGen = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-gen", L"0"});
        const CoreLaunchOptions negGen = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-gen", L"-1"});
        const CoreLaunchOptions badGen = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-gen", L"99xyz"});
        const CoreLaunchOptions trailingGen = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-gen"});
        const CoreLaunchOptions guardGen = ParseCoreLaunchOptions({L"Atrium.exe", L"--guard", L"4242", L"--guard-gen", L"77"});
        Check(zeroGen.desktopAgentGeneration == 0, "--agent-gen 0 parses as 0");
        Check(negGen.desktopAgentGeneration == 0, "--agent-gen -1 leaves desktopAgentGeneration=0");
        Check(badGen.desktopAgentGeneration == 0, "--agent-gen 99xyz leaves desktopAgentGeneration=0");
        Check(trailingGen.desktopAgentGeneration == 0, "trailing --agent-gen leaves desktopAgentGeneration=0");
        Check(guardGen.guard && guardGen.guardPid == 4242 && guardGen.guardGeneration == 77, "--guard-gen 77 parses guardGeneration");
    }
    {
        const CoreLaunchOptions validNonce = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-nonce", L"0123456789abcdefABCDEF"});
        const CoreLaunchOptions badHexNonce = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-nonce", L"01234567zzzz"});
        const CoreLaunchOptions tooLongNonce = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-nonce", std::wstring(65, L'a')});
        const CoreLaunchOptions emptyNonce = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-nonce", L""});
        const CoreLaunchOptions trailingNonce = ParseCoreLaunchOptions({L"Atrium.exe", L"--agent-nonce"});
        Check(validNonce.desktopAgentNonce == L"0123456789abcdefABCDEF", "--agent-nonce accepts valid ASCII hex token");
        Check(badHexNonce.desktopAgentNonce.empty(), "--agent-nonce rejects non-hex characters");
        Check(tooLongNonce.desktopAgentNonce.empty(), "--agent-nonce rejects tokens longer than 64 chars");
        Check(emptyNonce.desktopAgentNonce.empty(), "--agent-nonce rejects empty token");
        Check(trailingNonce.desktopAgentNonce.empty(), "trailing --agent-nonce leaves desktopAgentNonce empty");
    }
}

void CheckGuardAgentPidFile()
{
    using namespace atrium;
    TempDir temp(L"atrium-agent-pid-test-");
    const std::wstring pidPath = (temp.path / L"agent.pid").wstring();

    Check(ReadAgentPid(pidPath) == 0, "missing agent.pid reads as 0");
    Check(!WriteAgentPid(0, pidPath), "WriteAgentPid(0) is rejected");
    Check(!WriteAgentPid(4242, L""), "WriteAgentPid with empty path is rejected");
    Check(ReadAgentPid(L"") == 0, "ReadAgentPid with empty path reads as 0");

    Check(WriteAgentPid(4242, pidPath), "WriteAgentPid(4242) succeeds in temp path");
    Check(ReadAgentPid(pidPath) == 4242, "ReadAgentPid returns written PID 4242");

    Check(WriteAgentPid(9876, pidPath), "WriteAgentPid(9876) overwrites existing agent.pid");
    Check(ReadAgentPid(pidPath) == 9876, "ReadAgentPid returns overwritten PID 9876");

    Check(WriteTextFile(pidPath, L"not-a-pid"), "seed invalid agent.pid contents");
    Check(ReadAgentPid(pidPath) == 0, "invalid agent.pid contents read as 0");

    Check(WriteTextFile(pidPath, L"99999999999"), "seed out-of-range DWORD agent.pid contents");
    Check(ReadAgentPid(pidPath) == 0, "out-of-range DWORD agent.pid contents read as 0");

    ClearAgentPid(pidPath);
    Check(ReadAgentPid(pidPath) == 0 && !fs::exists(pidPath), "ClearAgentPid removes agent.pid");
    ClearAgentPid(pidPath);
    Check(ReadAgentPid(pidPath) == 0, "ClearAgentPid on missing file stays 0");
}

} // namespace

int main()
{
    winrt::init_apartment();
    CheckIntegrityAndPrivilegeClassification();
    CheckPipeAndMutexAcls();
    CheckMutexAcl();
    CheckLifecycleAndSettingsServers();
    CheckLifecyclePausedAndStalledCases();
    CheckGuardInstanceGeneration();
    CheckDesktopAgentLaunchArgs();
    CheckGuardAgentPidFile();
    if (g_failures) {
        std::fprintf(stderr, "%d privilege/lifecycle check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("privilege and lifecycle checks passed");
    return 0;
}
