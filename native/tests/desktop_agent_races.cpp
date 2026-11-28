#include "shell/desktop_agent_client.h"
#include "shell/desktop_agent_ipc.h"
#include "win32_path.h"

#include <windows.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace atrium;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

std::wstring UniquePipe(const wchar_t* tag, uint64_t suffix)
{
    return L"\\\\.\\pipe\\Atrium.DesktopAgent.Races." + std::wstring(tag) + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(suffix);
}

template<class Pred> bool WaitUntil(Pred&& pred, DWORD timeoutMs = 4000)
{
    for (const ULONGLONG deadline = GetTickCount64() + timeoutMs; GetTickCount64() < deadline;) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

std::wstring ExtractArgValue(const std::wstring& args, const std::wstring& flag)
{
    const size_t pos = args.find(flag);
    if (pos == std::wstring::npos)
        return {};
    size_t start = pos + flag.size();
    while (start < args.size() && args[start] == L' ')
        ++start;
    size_t end = start;
    while (end < args.size() && args[end] != L' ')
        ++end;
    return args.substr(start, end - start);
}

HANDLE SpawnRaceChild(uint64_t gen, const std::wstring& cmdPipe, const std::wstring& evtPipe, const std::wstring& nonce, const std::wstring& sentEvt,
                      winrt::handle* trackOut, DWORD& errOut)
{
    errOut = 0;
    const std::wstring exe = ModulePath();
    if (exe.empty())
        return errOut = ERROR_FILE_NOT_FOUND, nullptr;
    std::wstring cmdLine =
        L"\"" + exe + L"\" --race-evt-child " + std::to_wstring(gen) + L" \"" + cmdPipe + L"\" \"" + evtPipe + L"\" \"" + nonce + L"\" \"" + sentEvt + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return errOut = GetLastError(), nullptr;
    CloseHandle(pi.hThread);
    if (trackOut) {
        HANDLE dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
        trackOut->attach(dup);
    }
    return pi.hProcess;
}

int RunRaceChild(int argc, LPWSTR* argv)
{
    if (argc < 7)
        return 1;
    const uint64_t gen = std::wcstoull(argv[2], nullptr, 10);
    const std::wstring cmdPipe = argv[3], evtPipe = argv[4], nonce = argv[5];
    const winrt::handle sentEvt{OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[6])};
    const winrt::handle srv{CreateSameUserNamedPipeServer(cmdPipe)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED ov{};
    ov.hEvent = ev.get();
    if (srv.get() == INVALID_HANDLE_VALUE || !ev || !stop || !ConnectNamedPipeOverlapped(srv.get(), ov, stop.get()))
        return 1;
    const DWORD pid = GetCurrentProcessId();
    for (std::string raw; WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT;) {
        bool disc = false;
        AgentCommandMessage cmd;
        if (!ReadPipeMessageOverlapped(srv.get(), ov, stop.get(), raw, disc) || !ValidateAndParseAgentCommand(raw, cmd))
            break;
        AgentCommandReply reply{true, pid, gen, HostSendStatus::Sent, false, L"", nonce};
        if (!WritePipeMessageOverlapped(srv.get(), ov, stop.get(), SerializeAgentCommandReply(reply)))
            break;
        if (cmd.type == AgentCommandType::SetTrayHosting && cmd.hosting) {
            const winrt::handle evtCli{ConnectSameUserPipeClient(evtPipe, 2000, stop.get())};
            if (evtCli.get() != INVALID_HANDLE_VALUE && evtCli) {
                AgentEventMessage snap;
                snap.type = AgentEventType::TraySnapshot;
                snap.snapshot.yielded = true, snap.snapshot.yieldReason = TrayHostYieldReason::OtherProcess;
                snap.snapshot.icons.push_back(
                    AgentTrayIconDto{L"hwnd:9:1", 9, 1, 1, 4, false, L"stale", L"stale.exe", pid, 1, false, 1, false, 2, 2, std::vector<uint8_t>(16, 0x55)});
                std::string replyOut;
                if (!SendPipeRequestOverlapped(evtCli.get(), SerializeAgentEvent(snap), replyOut, 2000, stop.get()) && sentEvt)
                    SetEvent(sentEvt.get());
            }
        }
        if (cmd.type == AgentCommandType::Quit)
            break;
    }
    DisconnectNamedPipe(srv.get());
    return 0;
}

void TestEvtStaleMessageReadBeforeExitCannotWriteBack()
{
    const std::wstring cmdPipe = UniquePipe(L"StaleEvtCmd", 1), evtPipe = UniquePipe(L"StaleEvt", 1);
    const std::wstring evtAbortedName = L"Local\\Atrium.Race.EvtAborted." + std::to_wstring(GetCurrentProcessId());
    const winrt::handle evtAborted{CreateEventW(nullptr, TRUE, FALSE, evtAbortedName.c_str())}, pausedBeforeHandle{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        releaseHandle{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const DWORD selfPid = GetCurrentProcessId();
    constexpr uint64_t kGen = 501;
    std::atomic_int trayCalls{0}, yieldCalls{0}, gateHits{0}, spawnNum{0};
    std::vector<winrt::handle> childProcs;
    std::mutex mu;

    DesktopAgentCallbacks cb;
    cb.onTrayChanged = [&] { trayCalls.fetch_add(1); };
    cb.onYieldChanged = [&] { yieldCalls.fetch_add(1); };
    DesktopAgentClientOptions opts{0,
                                   selfPid,
                                   kGen,
                                   cmdPipe,
                                   evtPipe,
                                   true,
                                   false,
                                   false,
                                   [&](const std::wstring& args, DWORD& e) -> HANDLE {
                                       winrt::handle tracked;
                                       HANDLE h = (spawnNum.fetch_add(1) >= 2) ? (e = ERROR_NOT_READY, nullptr) :
                                                                                 SpawnRaceChild(kGen, cmdPipe, evtPipe, ExtractArgValue(args, L"--agent-nonce"),
                                                                                                evtAbortedName, &tracked, e);
                                       if (tracked) {
                                           std::lock_guard lock(mu);
                                           childProcs.push_back(std::move(tracked));
                                       }
                                       return h;
                                   },
                                   3000};
    opts.onBeforeHandleEventForTest = [&] {
        if (gateHits.fetch_add(1) == 0) {
            SetEvent(pausedBeforeHandle.get());
            WaitForSingleObject(releaseHandle.get(), 4000);
        }
    };
    {
        DesktopAgentClient client(cb, opts);
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0; }, 4000), "race child spawned and verified");
        const DWORD pid1 = client.AgentPid();
        client.SetTrayHosting(true);
        Check(WaitForSingleObject(pausedBeforeHandle.get(), 4000) == WAIT_OBJECT_0, "paused ServeEvtPipe after read/parse and before HandleEvent");

        HANDLE proc1 = nullptr;
        {
            std::lock_guard lock(mu);
            if (!childProcs.empty())
                proc1 = childProcs.front().get();
        }
        Check(proc1 != nullptr && TerminateProcess(proc1, 1) != FALSE, "terminated child while stale TraySnapshot queued before HandleEvent");
        Check(WaitUntil([&] { return !client.Running(); }, 4000), "ClearAgentStateOnExit completed while HandleEvent paused");
        const int callsAtExit = trayCalls.load();
        SetEvent(releaseHandle.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        Check(client.Icons().empty() && !client.Yielded() && trayCalls.load() == callsAtExit,
              "stale TraySnapshot read before ClearAgentStateOnExit did not write back tray state");
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0 && client.AgentPid() != pid1 && trayCalls.load() > callsAtExit; }, 6000),
              "stale Evt pipe cleared cleanly and child #2 delivered fresh snapshot");
        client.Shutdown();
    }
    for (auto& proc : childProcs) {
        if (proc && WaitForSingleObject(proc.get(), 1500) != WAIT_OBJECT_0)
            TerminateProcess(proc.get(), 1), WaitForSingleObject(proc.get(), 1500);
    }
}

void TestReplayDoesNotOverwriteNewerHostingAndAnchor()
{
    const std::wstring cmdPipe = UniquePipe(L"OrderCmd", 2), evtPipe = UniquePipe(L"OrderEvt", 2);
    const DWORD selfPid = GetCurrentProcessId();
    constexpr uint64_t kGen = 502;
    const RECT anchorA{0, 0, 100, 40}, anchorB{20, 40, 220, 80};
    const winrt::handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        replayHostingStarted{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, allowReplayContinue{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    std::mutex mu;
    std::vector<bool> observedHosting;
    std::vector<RECT> observedAnchors;
    std::atomic_int hostingCount{0};

    std::thread mockSrv([&] {
        const winrt::handle srv{CreateSameUserNamedPipeServer(cmdPipe)};
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stop.get()))
            return;
        while (WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT) {
            std::string raw;
            bool disc = false;
            AgentCommandMessage cmd;
            if (!ReadPipeMessageOverlapped(srv.get(), ov, stop.get(), raw, disc) || !ValidateAndParseAgentCommand(raw, cmd))
                break;
            AgentCommandReply r{true, selfPid, kGen};
            if (cmd.type == AgentCommandType::SetTrayHosting) {
                if (hostingCount.fetch_add(1) == 0) {
                    SetEvent(replayHostingStarted.get());
                    WaitForSingleObject(allowReplayContinue.get(), 3000);
                }
                std::lock_guard lock(mu);
                observedHosting.push_back(cmd.hosting);
            } else if (cmd.type == AgentCommandType::SetTrayPlaces) {
                std::lock_guard lock(mu);
                observedAnchors.push_back(cmd.anchor);
            }
            if (!WritePipeMessageOverlapped(srv.get(), ov, stop.get(), SerializeAgentCommandReply(r)) || cmd.type == AgentCommandType::Quit)
                break;
        }
        DisconnectNamedPipe(srv.get());
    });

    DesktopAgentClientOptions opts{
        0, selfPid, kGen, cmdPipe, evtPipe, true, false, true, [](const std::wstring&, DWORD& e) -> HANDLE {
                                       return e = 0, nullptr; }, 3000};
    {
        DesktopAgentClient client({}, opts);
        client.SetTrayHosting(true);
        client.SetPlaces(anchorA);
        Check(WaitForSingleObject(replayHostingStarted.get(), 4000) == WAIT_OBJECT_0, "ReplayDesiredState started SetTrayHosting(true)");

        std::thread uiThread([&] {
            client.SetTrayHosting(false);
            client.SetPlaces(anchorB);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        SetEvent(allowReplayContinue.get());
        uiThread.join();

        Check(WaitUntil(
                  [&] {
                      std::lock_guard lock(mu);
                      return observedHosting.size() >= 2 && !observedAnchors.empty() && !client.Hosting() && EqualRect(&observedAnchors.back(), &anchorB);
                  },
                  4000),
              "server converged to newer SetTrayHosting(false) and anchorB");
        std::lock_guard lock(mu);
        Check(!observedHosting.back(), "replay hosting=true never overwrote live hosting=false");
        Check(EqualRect(&observedAnchors.back(), &anchorB), "replay anchorA never overwrote live anchorB");
        SetEvent(stop.get());
        client.Shutdown();
    }
    if (mockSrv.joinable())
        mockSrv.join();
}

} // namespace

int main()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 7 && std::wstring_view(argv[1]) == L"--race-evt-child") {
        const int code = RunRaceChild(argc, argv);
        LocalFree(static_cast<HLOCAL>(argv));
        return code;
    }
    if (argv)
        LocalFree(static_cast<HLOCAL>(argv));

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestEvtStaleMessageReadBeforeExitCannotWriteBack();
    TestReplayDoesNotOverwriteNewerHostingAndAnchor();
    winrt::uninit_apartment();
    return g_failures == 0 ? 0 : 1;
}
