#include "shell/desktop_agent_client.h"
#include "shell/desktop_agent_host.h"
#include "shell/desktop_agent_ipc.h"
#include "shell/tray_model.h"
#include "win32_path.h"

#include <windows.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
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
    return L"\\\\.\\pipe\\Atrium.DesktopAgent.E2E." + std::wstring(tag) + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(suffix);
}

template<class Pred> bool WaitUntil(Pred&& pred, DWORD timeoutMs = 4000)
{
    for (const ULONGLONG deadline = GetTickCount64() + timeoutMs; GetTickCount64() < deadline;) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
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

int CountOverlayWindowsForPid(DWORD pid, const RECT* expectedVisibleRect = nullptr)
{
    struct Query {
        DWORD pid = 0;
        const RECT* expectedRect = nullptr;
        int count = 0;
    } query{pid, expectedVisibleRect, 0};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* q = reinterpret_cast<Query*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid != q->pid)
                return TRUE;
            wchar_t cls[64]{};
            if (GetClassNameW(hwnd, cls, 64) == 0 || std::wstring_view(cls) != L"AtriumDesktopAgentDropOverlay")
                return TRUE;
            if (q->expectedRect) {
                RECT rc{};
                if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &rc) || !EqualRect(&rc, q->expectedRect))
                    return TRUE;
            }
            ++q->count;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&query));
    return query.count;
}

HANDLE SpawnE2EHelperProcess(const std::wstring& args, winrt::handle* trackOut, DWORD& errOut)
{
    errOut = 0;
    const std::wstring exe = ModulePath();
    if (exe.empty())
        return errOut = ERROR_FILE_NOT_FOUND, nullptr;
    std::wstring cmdLine = L"\"" + exe + L"\" " + args;
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

HANDLE SpawnChildAgentProcess(DWORD corePid, uint64_t gen, const std::wstring& cmdPipe, const std::wstring& evtPipe, const std::wstring& nonce,
                              winrt::handle* trackOut, DWORD& errOut)
{
    return SpawnE2EHelperProcess(L"--child-agent " + std::to_wstring(corePid) + L" " + std::to_wstring(gen) + L" \"" + cmdPipe + L"\" \"" + evtPipe + L"\" \"" +
                                     nonce + L"\"",
                                 trackOut, errOut);
}

void CleanupChildProcs(std::vector<winrt::handle>& procs)
{
    for (auto& proc : procs) {
        if (proc && WaitForSingleObject(proc.get(), 1500) != WAIT_OBJECT_0) {
            TerminateProcess(proc.get(), 1);
            WaitForSingleObject(proc.get(), 1500);
        }
    }
    procs.clear();
}

HANDLE SpawnMockCmdChildProcess(uint64_t gen, const std::wstring& cmdPipe, const std::wstring& evtPipe, const std::wstring& nonce,
                                const std::wstring& startedEvt, const std::wstring& gateEvt, const std::wstring& rejEvt, winrt::handle* trackOut, DWORD& errOut)
{
    return SpawnE2EHelperProcess(L"--mock-cmd-child " + std::to_wstring(gen) + L" \"" + cmdPipe + L"\" \"" + evtPipe + L"\" \"" + nonce + L"\" \"" +
                                     startedEvt + L"\" \"" + gateEvt + L"\" \"" + rejEvt + L"\"",
                                 trackOut, errOut);
}

int RunMockCmdChild(int argc, LPWSTR* argv)
{
    if (argc < 9)
        return 1;
    const uint64_t gen = std::wcstoull(argv[2], nullptr, 10);
    const std::wstring cmdPipe = argv[3], evtPipe = argv[4], nonce = argv[5];
    const auto openEvt = [](const wchar_t* n) {
        return std::wstring_view(n) == L"-" ? winrt::handle{} : winrt::handle{OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, n)};
    };
    const winrt::handle started{openEvt(argv[6])}, gate{openEvt(argv[7])}, rej{openEvt(argv[8])};
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
        if (cmd.type == AgentCommandType::Ping && evtPipe != L"-") {
            std::string evtReply;
            AgentEventMessage snap{AgentEventType::TraySnapshot};
            if (!SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap), evtReply, 600) && rej)
                SetEvent(rej.get());
            if (started)
                SetEvent(started.get());
            if (gate)
                WaitForSingleObject(gate.get(), 4000);
        } else if (cmd.type == AgentCommandType::SyncDropBars) {
            if (started)
                SetEvent(started.get());
            if (gate)
                WaitForSingleObject(gate.get(), 4000);
        }
        if (!WritePipeMessageOverlapped(srv.get(), ov, stop.get(), SerializeAgentCommandReply(reply)) || cmd.type == AgentCommandType::Quit)
            break;
    }
    DisconnectNamedPipe(srv.get());
    return 0;
}

void TestRealMultiProcessAgentLifecycle()
{
    const std::wstring n1 = GenerateAgentSpawnNonce(), n2 = GenerateAgentSpawnNonce();
    Check(n1.size() == 32 && n2.size() == 32 && n1 != n2, "GenerateAgentSpawnNonce produces unique 32-char hex nonces");
    Check(std::ranges::all_of(n1, [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'); }), "spawn nonce uses lowercase hex chars");
    AgentCommandReply nonceReply{true, 42, 7, HostSendStatus::Sent, false, L"", n1};
    AgentCommandReply parsedNonceReply;
    Check(ValidateAndParseAgentCommandReply(SerializeAgentCommandReply(nonceReply), parsedNonceReply) && parsedNonceReply.nonce == n1,
          "AgentCommandReply nonce round-trips through JSON");

    constexpr uint64_t kGen = 9000000000000000101ULL;
    const DWORD selfPid = GetCurrentProcessId();
    const std::wstring cmdPipe = UniquePipe(L"ProcCmd", 1), evtPipe = UniquePipe(L"ProcEvt", 1);
    std::mutex mu;
    std::vector<winrt::handle> childProcs;
    std::vector<std::wstring> seenNonces;

    DesktopAgentClientOptions opts;
    opts.corePid = selfPid;
    opts.generation = kGen;
    opts.cmdPipeOverride = cmdPipe;
    opts.evtPipeOverride = evtPipe;
    opts.launchSupervisor = true;
    opts.allowSameProcessPeer = false;
    opts.pingWaitMs = 4000;
    opts.spawnAgentOverride = [&](const std::wstring& args, DWORD& errOut) -> HANDLE {
        const std::wstring nonce = ExtractArgValue(args, L"--agent-nonce");
        winrt::handle tracked;
        HANDLE proc = SpawnChildAgentProcess(selfPid, kGen, cmdPipe, evtPipe, nonce, &tracked, errOut);
        if (proc) {
            std::lock_guard lock(mu);
            seenNonces.push_back(nonce);
            childProcs.push_back(std::move(tracked));
        }
        return proc;
    };

    const RECT barRect{20, 20, 140, 60};
    {
        DesktopAgentClient client({}, opts);
        Check(WaitUntil([&] { return client.Running() && client.EvtServerReady(); }, 5000), "real child agent #1 spawned and EvtServerReady");
        const DWORD pid1 = client.AgentPid();
        Check(pid1 != 0 && pid1 != selfPid, "child agent #1 runs in separate OS process");
        client.SyncDropBars({{1, barRect, true}});
        Check(WaitUntil([&] { return CountOverlayWindowsForPid(pid1, &barRect) == 1; }, 3000), "SyncDropBars created visible overlay in child #1");

        HANDLE proc1 = nullptr;
        {
            std::lock_guard lock(mu);
            if (!childProcs.empty())
                proc1 = childProcs.front().get();
        }
        Check(proc1 != nullptr && TerminateProcess(proc1, 1) != FALSE, "terminated child agent #1");
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0 && client.AgentPid() != pid1; }, 8000),
              "supervisor detected exit and respawned child agent #2");
        const DWORD pid2 = client.AgentPid();
        Check(pid2 != 0 && pid2 != selfPid && pid2 != pid1, "child agent #2 has new OS PID");
        Check(WaitUntil([&] { return CountOverlayWindowsForPid(pid2, &barRect) == 1; }, 4000), "supervisor automatically replayed SyncDropBars to child #2");
        {
            std::lock_guard lock(mu);
            Check(seenNonces.size() >= 2 && seenNonces[0] != seenNonces[1] && seenNonces[1].size() == 32,
                  "respawned child agent #2 received a fresh spawn nonce");
        }
        client.Shutdown();
    }
    CleanupChildProcs(childProcs);

    // Wrong --agent-nonce injection must be rejected and never marked Running().
    const std::wstring badCmdPipe = UniquePipe(L"BadNonceCmd", 2), badEvtPipe = UniquePipe(L"BadNonceEvt", 2);
    std::atomic_bool everRunning{false};
    std::vector<winrt::handle> badProcs;
    DesktopAgentClientOptions badOpts;
    badOpts.corePid = selfPid;
    badOpts.generation = kGen + 1;
    badOpts.cmdPipeOverride = badCmdPipe;
    badOpts.evtPipeOverride = badEvtPipe;
    badOpts.launchSupervisor = true;
    badOpts.allowSameProcessPeer = false;
    badOpts.pingWaitMs = 400;
    badOpts.spawnAgentOverride = [&](const std::wstring&, DWORD& errOut) -> HANDLE {
        winrt::handle tracked;
        HANDLE proc = SpawnChildAgentProcess(selfPid, kGen + 1, badCmdPipe, badEvtPipe, L"wrong-nonce-deadbeef", &tracked, errOut);
        if (proc) {
            std::lock_guard lock(mu);
            badProcs.push_back(std::move(tracked));
        }
        return proc;
    };
    DesktopAgentCallbacks badCb;
    badCb.onAgentPidChanged = [&](DWORD pid) {
        if (pid != 0)
            everRunning.store(true);
    };
    {
        DesktopAgentClient badClient(badCb, badOpts);
        Check(WaitUntil([&] { return badClient.Degraded(); }, 12000), "wrong-nonce agent spawns exhaust retries into degraded mode");
        Check(!badClient.Running() && !everRunning.load(), "wrong-nonce child process never marked Running()");
        badClient.Shutdown();
    }
    for (auto& proc : badProcs) {
        if (proc)
            TerminateProcess(proc.get(), 1), WaitForSingleObject(proc.get(), 1500);
    }
}

void TestWaitOverlappedTimeoutWithCompletedWrite()
{
    const std::wstring pipeName = UniquePipe(L"TimeoutWrite", 10);
    const winrt::handle srv{CreateSameUserNamedPipeServer(pipeName)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        quit{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, readDone{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(srv.get() != INVALID_HANDLE_VALUE && ev && quit && readDone, "created pipe for SendPipeRequestOverlapped timeout test");
    std::string serverGot;
    std::thread srvThread([&] {
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (!ConnectNamedPipeOverlapped(srv.get(), ov, quit.get()))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        bool disc = false;
        ReadPipeMessageOverlapped(srv.get(), ov, quit.get(), serverGot, disc);
        SetEvent(readDone.get());
        WaitForSingleObject(quit.get(), 500);
        DisconnectNamedPipe(srv.get());
    });
    const winrt::handle cli{ConnectSameUserPipeClient(pipeName, 2000, quit.get())};
    Check(cli.get() != INVALID_HANDLE_VALUE && cli, "connected to delayed-reply pipe server");
    if (cli.get() != INVALID_HANDLE_VALUE) {
        std::string replyOut;
        bool writeCompleted = false;
        const bool ok = SendPipeRequestOverlapped(cli.get(), R"({"cmd":"ping"})", replyOut, 120, nullptr, &writeCompleted);
        Check(!ok && writeCompleted && replyOut.empty(), "SendPipeRequestOverlapped times out with writeCompleted=true when reply withheld");
        Check(WaitForSingleObject(readDone.get(), 1000) == WAIT_OBJECT_0 && serverGot == R"({"cmd":"ping"})", "server actually received the completed write");
    }
    SetEvent(quit.get());
    if (srvThread.joinable())
        srvThread.join();

    // Unwritten failure paths -> writeCompleted=false, HostSendStatus::NotSent.
    std::string unusedReply;
    bool invalidWriteCompleted = true;
    Check(!SendPipeRequestOverlapped(INVALID_HANDLE_VALUE, R"({"cmd":"ping"})", unusedReply, 100, nullptr, &invalidWriteCompleted) && !invalidWriteCompleted,
          "invalid handle returns false with writeCompleted=false");

    const std::wstring discPipe = UniquePipe(L"DiscWrite", 11);
    const winrt::handle discSrv{CreateSameUserNamedPipeServer(discPipe)}, discEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        discQuit{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, srvClosed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread discThread([&] {
        OVERLAPPED ov{};
        ov.hEvent = discEv.get();
        if (ConnectNamedPipeOverlapped(discSrv.get(), ov, discQuit.get()))
            DisconnectNamedPipe(discSrv.get());
        SetEvent(srvClosed.get());
    });
    const winrt::handle discCli{ConnectSameUserPipeClient(discPipe, 2000, discQuit.get())};
    if (discThread.joinable())
        discThread.join();
    if (discCli.get() != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(srvClosed.get(), 1000);
        bool discWriteCompleted = true;
        Check(!SendPipeRequestOverlapped(discCli.get(), R"({"cmd":"ping"})", unusedReply, 100, nullptr, &discWriteCompleted) && !discWriteCompleted,
              "disconnected pipe fails pre-write with writeCompleted=false");
    }

    // DesktopAgentClient::Send returns Unknown (skipping Explorer fallback) when write succeeds and reply times out.
    const std::wstring cmdPipe = UniquePipe(L"SlowReplyCmd", 12), evtPipe = UniquePipe(L"SlowReplyEvt", 12);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 222;
    const winrt::handle stopSlow{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, slowEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int sendObserved{0};
    std::thread slowCmdThread([&] {
        const winrt::handle pipe{CreateSameUserNamedPipeServer(cmdPipe)};
        OVERLAPPED ov{};
        ov.hEvent = slowEv.get();
        if (pipe.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(pipe.get(), ov, stopSlow.get()))
            return;
        while (WaitForSingleObject(stopSlow.get(), 0) == WAIT_TIMEOUT) {
            std::string raw;
            bool disc = false;
            AgentCommandMessage cmd;
            if (!ReadPipeMessageOverlapped(pipe.get(), ov, stopSlow.get(), raw, disc) || !ValidateAndParseAgentCommand(raw, cmd))
                break;
            if (cmd.type == AgentCommandType::Ping) {
                AgentCommandReply r{true, selfPid, gen};
                if (!WritePipeMessageOverlapped(pipe.get(), ov, stopSlow.get(), SerializeAgentCommandReply(r)))
                    break;
            } else if (cmd.type == AgentCommandType::TraySend) {
                sendObserved.fetch_add(1);
                WaitForSingleObject(stopSlow.get(), 2500);
                break;
            } else {
                AgentCommandReply r{true};
                if (!WritePipeMessageOverlapped(pipe.get(), ov, stopSlow.get(), SerializeAgentCommandReply(r)))
                    break;
            }
        }
        DisconnectNamedPipe(pipe.get());
    });
    DesktopAgentClientOptions clientOpts{
        0, selfPid, gen, cmdPipe, evtPipe, true, false, true, [](const std::wstring&, DWORD& e) -> HANDLE {
                                             return e = 0, nullptr; }};
    {
        DesktopAgentClient client({}, clientOpts);
        Check(WaitUntil([&] { return client.Running(); }), "slow-reply mock server connected");
        TrayModel model(nullptr, 0);
        model.SetDesktopAgent(&client);
        TrayIcon icon;
        icon.hostKey = L"test:timeout", icon.tip = L"AtriumTestNoExplorerFallback";
        const DWORD beforeSynth = TrayModel::LastSyntheticInput();
        model.Click(icon, false, RECT{0, 0, 20, 20});
        Check(sendObserved.load() == 1 && TrayModel::LastSyntheticInput() == beforeSynth, "TraySend reply timeout returns Unknown and skips Explorer fallback");
        SetEvent(stopSlow.get());
        client.Shutdown();
        Check(client.Send(L"test:timeout", tray::HostAction::Left, POINT{1, 1}) == HostSendStatus::NotSent, "Send returns NotSent after agent shutdown");
    }
    if (slowCmdThread.joinable())
        slowCmdThread.join();
}

void TestOverlayShowFailure()
{
    const RECT barRect{30, 30, 150, 70};
    {
        const std::wstring cmdPipe = UniquePipe(L"ShowFailCmd", 20);
        const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        DesktopAgentHostOptions failOpts;
        failOpts.cmdPipeOverride = cmdPipe;
        failOpts.evtPipeOverride = UniquePipe(L"ShowFailEvt", 20);
        failOpts.stopEvent = stopEvt.get();
        failOpts.failOverlayShowForTest = true;
        std::atomic<int> hostExit{-1};
        std::thread hostThread([&] { hostExit.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 300, GetModuleHandleW(nullptr), failOpts)); });

        const winrt::handle pipe{ConnectSameUserPipeClient(cmdPipe, 3000)};
        Check(pipe.get() != INVALID_HANDLE_VALUE && pipe, "connected to failOverlayShowForTest host");
        if (pipe.get() != INVALID_HANDLE_VALUE) {
            std::string rawReply;
            AgentCommandReply reply;
            AgentCommandMessage syncCmd{AgentCommandType::SyncDropBars, 0, false, {}, L"", tray::HostAction::Left, {}, {}, {{1, barRect, true}}};
            Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(syncCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                      !reply.ok && reply.error == L"overlay_sync_failed",
                  "failOverlayShowForTest returns overlay_sync_failed");
            Check(CountOverlayWindowsForPid(GetCurrentProcessId()) == 0, "failed overlay show leaves zero hidden overlay windows");
            AgentCommandMessage quitCmd{AgentCommandType::Quit};
            SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(quitCmd), rawReply, 2000);
        }
        SetEvent(stopEvt.get());
        if (hostThread.joinable())
            hostThread.join();
        Check(hostExit.load() == 0, "failOverlayShowForTest host exited cleanly");
    }
    {
        const std::wstring cmdPipe = UniquePipe(L"ShowOkCmd", 21);
        const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        DesktopAgentHostOptions okOpts{cmdPipe, UniquePipe(L"ShowOkEvt", 21), 5000, stopEvt.get()};
        std::atomic<int> hostExit{-1};
        std::thread hostThread([&] { hostExit.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 301, GetModuleHandleW(nullptr), okOpts)); });

        const winrt::handle pipe{ConnectSameUserPipeClient(cmdPipe, 3000)};
        if (pipe.get() != INVALID_HANDLE_VALUE) {
            std::string rawReply;
            AgentCommandReply reply;
            AgentCommandMessage syncCmd{AgentCommandType::SyncDropBars, 0, false, {}, L"", tray::HostAction::Left, {}, {}, {{1, barRect, true}}};
            Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(syncCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                      reply.ok,
                  "SyncDropBars succeeds cleanly when failOverlayShowForTest is false");
            Check(CountOverlayWindowsForPid(GetCurrentProcessId(), &barRect) == 1, "healthy host shows visible overlay window");
            AgentCommandMessage quitCmd{AgentCommandType::Quit};
            SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(quitCmd), rawReply, 2000);
        }
        SetEvent(stopEvt.get());
        if (hostThread.joinable())
            hostThread.join();
        Check(hostExit.load() == 0, "healthy overlay host exited cleanly");
    }
}

void TestConcurrentReplayConvergence()
{
    // Concurrent ReplayDesiredState (Layout A delayed) + rapid UI SyncDropBars(Layout B) converges to Layout B.
    const std::vector<AgentBarRect> layoutA{{1, RECT{0, 0, 100, 40}, true}};
    const std::vector<AgentBarRect> layoutB{{2, RECT{0, 50, 200, 90}, true}};
    const std::wstring cmdPipe = UniquePipe(L"RaceCmd", 22), evtPipe = UniquePipe(L"RaceEvt", 22);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 302;
    const winrt::handle srvStop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, srvEv{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        syncAStarted{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, allowSyncAReply{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

    std::mutex layoutsMu;
    std::vector<std::vector<AgentBarRect>> receivedLayouts;

    DesktopAgentClientOptions raceOpts{
        0, selfPid, gen, cmdPipe, evtPipe, true, false, true, [](const std::wstring&, DWORD& e) -> HANDLE {
                                           return e = 0, nullptr; }, 3000};
    DesktopAgentClient client({}, raceOpts);
    client.SyncDropBars(layoutA);

    std::thread mockSrv([&] {
        const winrt::handle srv{CreateSameUserNamedPipeServer(cmdPipe)};
        OVERLAPPED ov{};
        ov.hEvent = srvEv.get();
        if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, srvStop.get()))
            return;
        while (WaitForSingleObject(srvStop.get(), 0) == WAIT_TIMEOUT) {
            std::string raw;
            bool disc = false;
            AgentCommandMessage cmd;
            if (!ReadPipeMessageOverlapped(srv.get(), ov, srvStop.get(), raw, disc) || !ValidateAndParseAgentCommand(raw, cmd))
                break;
            AgentCommandReply reply;
            if (cmd.type == AgentCommandType::Ping) {
                reply.ok = true, reply.pid = selfPid, reply.generation = gen;
            } else if (cmd.type == AgentCommandType::Quit) {
                reply.ok = true;
            } else if (cmd.type == AgentCommandType::SyncDropBars) {
                if (cmd.bars == layoutA) {
                    SetEvent(syncAStarted.get());
                    WaitForSingleObject(allowSyncAReply.get(), 2000);
                }
                {
                    std::lock_guard lock(layoutsMu);
                    receivedLayouts.push_back(cmd.bars);
                }
                reply.ok = true;
            }
            if (!WritePipeMessageOverlapped(srv.get(), ov, srvStop.get(), SerializeAgentCommandReply(reply)) || cmd.type == AgentCommandType::Quit)
                break;
        }
        DisconnectNamedPipe(srv.get());
    });

    Check(WaitForSingleObject(syncAStarted.get(), 4000) == WAIT_OBJECT_0, "ReplayDesiredState started in-flight Layout A SyncDropBars");
    std::thread uiThread([&] { client.SyncDropBars(layoutB); });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    SetEvent(allowSyncAReply.get());
    uiThread.join();

    Check(WaitUntil(
              [&] {
                  std::lock_guard lock(layoutsMu);
                  return receivedLayouts.size() >= 2;
              },
              3000),
          "concurrent ReplayDesiredState + UI SyncDropBars flushed both updates");
    {
        std::lock_guard lock(layoutsMu);
        Check(receivedLayouts.size() == 2 && receivedLayouts[0] == layoutA && receivedLayouts[1] == layoutB,
              "server converged to Layout B without Layout A overwriting it");
    }
    client.SyncDropBars(layoutB);
    {
        std::lock_guard lock(layoutsMu);
        Check(receivedLayouts.size() == 2, "client confirmed Layout B and skipped redundant resend");
    }

    SetEvent(srvStop.get());
    client.Shutdown();
    if (mockSrv.joinable())
        mockSrv.join();
}

void TestEvtBlockedBeforeHandshakeAndTerminatedOnExit()
{
    const std::wstring cmdPipe = UniquePipe(L"EvtGateCmd", 30), evtPipe = UniquePipe(L"EvtGateEvt", 30);
    const std::wstring startedName = L"Local\\Atrium.E2E.PingStarted." + std::to_wstring(GetCurrentProcessId()),
                       gateName = L"Local\\Atrium.E2E.PingGate." + std::to_wstring(GetCurrentProcessId()),
                       rejName = L"Local\\Atrium.E2E.PrePingRej." + std::to_wstring(GetCurrentProcessId());
    const winrt::handle startedEvt{CreateEventW(nullptr, TRUE, FALSE, startedName.c_str())}, gateEvt{CreateEventW(nullptr, TRUE, FALSE, gateName.c_str())},
        rejEvt{CreateEventW(nullptr, TRUE, FALSE, rejName.c_str())};
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 400;
    std::atomic_int trayCalls{0}, spawnNum{0};
    std::vector<winrt::handle> childProcs;
    std::mutex mu;

    DesktopAgentCallbacks cb;
    cb.onTrayChanged = [&] { trayCalls.fetch_add(1); };
    DesktopAgentClientOptions opts{0,
                                   selfPid,
                                   gen,
                                   cmdPipe,
                                   evtPipe,
                                   true,
                                   false,
                                   false,
                                   [&](const std::wstring& args, DWORD& e) -> HANDLE {
                                       winrt::handle tracked;
                                       HANDLE h =
                                           (spawnNum.fetch_add(1) == 0) ?
                                               SpawnMockCmdChildProcess(gen, cmdPipe, evtPipe, L"wrong-nonce", startedName, gateName, rejName, &tracked, e) :
                                               SpawnChildAgentProcess(selfPid, gen, cmdPipe, evtPipe, ExtractArgValue(args, L"--agent-nonce"), &tracked, e);
                                       if (tracked) {
                                           std::lock_guard lock(mu);
                                           childProcs.push_back(std::move(tracked));
                                       }
                                       return h;
                                   },
                                   3000};
    {
        DesktopAgentClient client(cb, opts);
        Check(WaitForSingleObject(startedEvt.get(), 4000) == WAIT_OBJECT_0, "mock child paused inside first Ping handshake");
        Check(WaitForSingleObject(rejEvt.get(), 1000) == WAIT_OBJECT_0 && trayCalls.load() == 0,
              "Evt rejected even genuine child PID before Ping handshake completed");
        SetEvent(gateEvt.get());
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0; }, 5000), "verified child #2 connected after child #1 wrong nonce rejected");
        const DWORD pid2 = client.AgentPid();
        client.SetTrayHosting(true);
        Check(WaitUntil([&] { return trayCalls.load() >= 1; }, 4000), "child #2 Evt dispatched snapshot after verified handshake");

        HANDLE proc2 = nullptr;
        {
            std::lock_guard lock(mu);
            if (childProcs.size() >= 2)
                proc2 = childProcs[1].get();
        }
        const int beforeExit = trayCalls.load();
        Check(proc2 != nullptr && TerminateProcess(proc2, 1) != FALSE, "terminated child #2 to trigger ClearAgentStateOnExit");
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0 && client.AgentPid() != pid2 && trayCalls.load() > beforeExit; }, 8000),
              "child #2 Evt torn down cleanly and child #3 Evt dispatched new snapshot after respawn");
        client.Shutdown();
    }
    CleanupChildProcs(childProcs);
}

void TestStaleConfirmDoesNotSkipRestartReplay()
{
    const std::wstring cmdPipe = UniquePipe(L"EpochCmd", 31), evtPipe = UniquePipe(L"EpochEvt", 31);
    const std::wstring syncStartedName = L"Local\\Atrium.E2E.Sync1Started." + std::to_wstring(GetCurrentProcessId()),
                       syncGateName = L"Local\\Atrium.E2E.Sync1Gate." + std::to_wstring(GetCurrentProcessId());
    const winrt::handle syncStarted{CreateEventW(nullptr, TRUE, FALSE, syncStartedName.c_str())},
        syncGate{CreateEventW(nullptr, TRUE, FALSE, syncGateName.c_str())};
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 401;
    const RECT barRect{10, 10, 110, 50};
    std::atomic_int spawnNum{0};
    std::vector<winrt::handle> childProcs;
    std::mutex mu;

    DesktopAgentClientOptions opts{0,
                                   selfPid,
                                   gen,
                                   cmdPipe,
                                   evtPipe,
                                   true,
                                   false,
                                   false,
                                   [&](const std::wstring& args, DWORD& e) -> HANDLE {
                                       const std::wstring nonce = ExtractArgValue(args, L"--agent-nonce");
                                       winrt::handle tracked;
                                       HANDLE h = (spawnNum.fetch_add(1) == 0) ?
                                                      SpawnMockCmdChildProcess(gen, cmdPipe, L"-", nonce, syncStartedName, syncGateName, L"-", &tracked, e) :
                                                      SpawnChildAgentProcess(selfPid, gen, cmdPipe, evtPipe, nonce, &tracked, e);
                                       if (tracked) {
                                           std::lock_guard lock(mu);
                                           childProcs.push_back(std::move(tracked));
                                       }
                                       return h;
                                   },
                                   4000};
    {
        DesktopAgentClient client({}, opts);
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0; }, 5000), "epoch test child #1 connected");
        const DWORD pid1 = client.AgentPid();
        std::thread syncThread([&] { client.SyncDropBars({{7, barRect, true}}); });
        Check(WaitForSingleObject(syncStarted.get(), 4000) == WAIT_OBJECT_0, "child #1 SyncDropBars paused in-flight");
        SetEvent(syncGate.get());
        HANDLE proc1 = nullptr;
        {
            std::lock_guard lock(mu);
            if (!childProcs.empty())
                proc1 = childProcs[0].get();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        Check(proc1 != nullptr && TerminateProcess(proc1, 1) != FALSE, "terminated child #1 concurrently with SyncDropBars reply");
        syncThread.join();
        Check(WaitUntil([&] { return client.Running() && client.AgentPid() != 0 && client.AgentPid() != pid1; }, 8000),
              "supervisor respawned child #2 after concurrent exit");
        Check(WaitUntil([&] { return CountOverlayWindowsForPid(client.AgentPid(), &barRect) == 1; }, 4000),
              "child #2 replayed SyncDropBars despite concurrent child #1 reply");
        client.Shutdown();
    }
    CleanupChildProcs(childProcs);
}

void TestSnapshotPostFailureRetriesUntilAck()
{
    const std::wstring evtPipe = UniquePipe(L"PostFailEvt", 32);
    const winrt::handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        hostReady{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, delivered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int dropCount{0};
    std::thread mockEvt([&] {
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        for (int i = 0; i < 2; ++i) {
            const winrt::handle deadSrv{CreateSameUserNamedPipeServer(evtPipe)};
            std::string raw;
            bool disc = false;
            if (deadSrv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(deadSrv.get(), ov, stop.get()))
                return;
            if (ReadPipeMessageOverlapped(deadSrv.get(), ov, stop.get(), raw, disc))
                dropCount.fetch_add(1);
            DisconnectNamedPipe(deadSrv.get());
        }
        const winrt::handle srv{CreateSameUserNamedPipeServer(evtPipe)};
        if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stop.get()))
            return;
        std::string raw;
        bool disc = false;
        AgentEventMessage evt;
        if (ReadPipeMessageOverlapped(srv.get(), ov, stop.get(), raw, disc) && ValidateAndParseAgentEvent(raw, evt) &&
            evt.type == AgentEventType::TraySnapshot &&
            WritePipeMessageOverlapped(srv.get(), ov, stop.get(), SerializeAgentEventReply(AgentEventReply{true}))) {
            SetEvent(delivered.get());
        }
        DisconnectNamedPipe(srv.get());
    });

    std::function<void()> pushSnap;
    DesktopAgentHostOptions opts{UniquePipe(L"PostFailCmd", 32),
                                 evtPipe,
                                 5000,
                                 stop.get(),
                                 {},
                                 false,
                                 [&](auto s, const auto&) { pushSnap = std::move(s), SetEvent(hostReady.get()); },
                                 L"",
                                 false,
                                 2};
    std::atomic<int> exitCode{-1};
    std::thread hostThread([&] { exitCode.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 402, GetModuleHandleW(nullptr), opts)); });
    Check(WaitForSingleObject(hostReady.get(), 3000) == WAIT_OBJECT_0, "post-fail test host ready");
    if (pushSnap)
        pushSnap();
    Check(WaitForSingleObject(delivered.get(), 5000) == WAIT_OBJECT_0 && dropCount.load() == 2,
          "resyncNeeded_ persisted past PostMessage failure and delivered TraySnapshot on retry");
    SetEvent(stop.get());
    if (mockEvt.joinable())
        mockEvt.join();
    if (hostThread.joinable())
        hostThread.join();
    Check(exitCode.load() == 0, "post-fail test host exited cleanly");
}

} // namespace

int main()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 9 && std::wstring_view(argv[1]) == L"--mock-cmd-child") {
        const int code = RunMockCmdChild(argc, argv);
        LocalFree(static_cast<HLOCAL>(argv));
        return code;
    }
    if (argv && argc >= 6 && std::wstring_view(argv[1]) == L"--child-agent") {
        const DWORD corePid = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        const uint64_t gen = std::wcstoull(argv[3], nullptr, 10);
        DesktopAgentHostOptions opts;
        opts.cmdPipeOverride = argv[4];
        opts.evtPipeOverride = argv[5];
        opts.nonce = (argc >= 7) ? argv[6] : L"";
        LocalFree(static_cast<HLOCAL>(argv));
        return RunDesktopAgentWithOptions(corePid, gen, GetModuleHandleW(nullptr), opts);
    }
    if (argv)
        LocalFree(static_cast<HLOCAL>(argv));

    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestRealMultiProcessAgentLifecycle();
    TestWaitOverlappedTimeoutWithCompletedWrite();
    TestOverlayShowFailure();
    TestConcurrentReplayConvergence();
    TestEvtBlockedBeforeHandshakeAndTerminatedOnExit();
    TestStaleConfirmDoesNotSkipRestartReplay();
    TestSnapshotPostFailureRetriesUntilAck();
    winrt::uninit_apartment();
    return g_failures == 0 ? 0 : 1;
}
