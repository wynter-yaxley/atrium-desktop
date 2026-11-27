#include "shell/desktop_agent_client.h"
#include "shell/desktop_agent_host.h"
#include "shell/desktop_agent_ipc.h"
#include "shell/tray_model.h"
#include "win32_path.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
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
    return L"\\\\.\\pipe\\Atrium.DesktopAgent.Test." + std::wstring(tag) + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(suffix);
}

template<class Pred> bool WaitUntil(Pred&& pred, DWORD timeoutMs = 3000)
{
    for (const ULONGLONG deadline = GetTickCount64() + timeoutMs; GetTickCount64() < deadline;) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

DesktopAgentClientOptions MakeLoopbackOpts(const std::wstring& cmdPipe, const std::wstring& evtPipe, DWORD pid, uint64_t gen, DWORD pingWaitMs = 2000,
                                           bool unboundEvt = false)
{
    DesktopAgentClientOptions opts;
    opts.corePid = pid, opts.generation = gen, opts.cmdPipeOverride = cmdPipe, opts.evtPipeOverride = evtPipe;
    opts.allowSameProcessPeer = true, opts.allowUnboundTestEvtPeer = unboundEvt, opts.pingWaitMs = pingWaitMs;
    opts.spawnAgentOverride = [](const std::wstring&, DWORD& errOut) -> HANDLE { return errOut = 0, nullptr; };
    return opts;
}

struct MockCmdServer {
    winrt::handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread th;

    template<class Fn> MockCmdServer(const std::wstring& pipeName, DWORD selfPid, uint64_t gen, Fn handler, bool acceptLoop = false)
    {
        th = std::thread([this, pipeName, selfPid, gen, h = std::move(handler), acceptLoop] {
            do {
                const winrt::handle srv{CreateSameUserNamedPipeServer(pipeName)};
                OVERLAPPED ov{};
                ov.hEvent = ev.get();
                if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stop.get()))
                    break;
                while (WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT) {
                    std::string raw;
                    bool disc = false;
                    AgentCommandMessage cmd;
                    if (!ReadPipeMessageOverlapped(srv.get(), ov, stop.get(), raw, disc) || !ValidateAndParseAgentCommand(raw, cmd))
                        break;
                    AgentCommandReply reply;
                    if (cmd.type == AgentCommandType::Ping)
                        reply.ok = true, reply.pid = selfPid, reply.generation = gen;
                    else if (cmd.type == AgentCommandType::Quit)
                        reply.ok = true;
                    else if (!h(cmd, reply))
                        break;
                    if (!WritePipeMessageOverlapped(srv.get(), ov, stop.get(), SerializeAgentCommandReply(reply)) || cmd.type == AgentCommandType::Quit)
                        break;
                }
                DisconnectNamedPipe(srv.get());
            } while (acceptLoop && WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT);
        });
    }
    MockCmdServer(const MockCmdServer&) = delete;
    MockCmdServer& operator=(const MockCmdServer&) = delete;
    ~MockCmdServer() { Stop(); }
    void Stop()
    {
        if (stop)
            SetEvent(stop.get());
        if (th.joinable())
            th.join();
    }
};

void WriteNoReadOverlapped(HANDLE pipe, const std::string& payload)
{
    DWORD written = 0;
    OVERLAPPED ov{};
    const winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ov.hEvent = ev.get();
    WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &written, &ov);
    WaitForSingleObject(ev.get(), 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

void TestPipeNames()
{
    const std::wstring cmd = DesktopAgentCommandPipeName(1, 4242, 7);
    const std::wstring evt = DesktopAgentEventPipeName(1, 4242, 7);
    Check(cmd == L"\\\\.\\pipe\\Atrium.DesktopAgent.Cmd.1.4242.7", "command pipe name format");
    Check(evt == L"\\\\.\\pipe\\Atrium.DesktopAgent.Evt.1.4242.7", "event pipe name format");
}

void TestCommandSerialization()
{
    AgentCommandMessage cmd{AgentCommandType::TraySend, 9001, false, {}, L"guid:{1234}", tray::HostAction::Double, POINT{120, 400}, RECT{100, 380, 140, 420}};
    AgentCommandMessage parsed;
    Check(ValidateAndParseAgentCommand(SerializeAgentCommand(cmd), parsed), "parse valid tray-send");
    Check(parsed.type == AgentCommandType::TraySend && parsed.opId == 9001 && parsed.key == cmd.key && parsed.action == cmd.action && parsed.at.x == 120 &&
              parsed.at.y == 400 && EqualRect(&parsed.cell, &cmd.cell),
          "tray-send fields + opId + geometry match");

    AgentCommandMessage sync{AgentCommandType::SyncDropBars,
                             0,
                             false,
                             {},
                             L"",
                             tray::HostAction::Left,
                             {},
                             {},
                             {{1, RECT{0, 1000, 1920, 1080}, true}, {2, RECT{1920, 1000, 3840, 1080}, false}}};
    AgentCommandMessage syncParsed;
    Check(ValidateAndParseAgentCommand(SerializeAgentCommand(sync), syncParsed) && syncParsed.bars == sync.bars, "sync-drop-bars round-trip matches");

    AgentCommandReply reply{true, 777, (1ULL << 60) + 9, HostSendStatus::Partial, true};
    AgentCommandReply replyParsed;
    Check(ValidateAndParseAgentCommandReply(SerializeAgentCommandReply(reply), replyParsed) && replyParsed.ok && replyParsed.pid == 777 &&
              replyParsed.generation == reply.generation && replyParsed.sendStatus == HostSendStatus::Partial && replyParsed.higherIntegrity,
          "command reply fields round-trip");
    reply.sendStatus = HostSendStatus::Unknown;
    Check(ValidateAndParseAgentCommandReply(SerializeAgentCommandReply(reply), replyParsed) && replyParsed.sendStatus == HostSendStatus::Unknown,
          "command reply HostSendStatus::Unknown round-trips");
}

void TestEventSerializationAndBounds()
{
    AgentEventMessage snap;
    snap.type = AgentEventType::TraySnapshot;
    AgentTrayIconDto icon{L"hwnd:100:1",
                          0x123456789ABCDEFULL,
                          1,
                          WM_APP + 5,
                          4,
                          true,
                          L"App Tip",
                          L"c:\\app\\test.exe",
                          555,
                          99,
                          false,
                          12,
                          true,
                          2,
                          2,
                          std::vector<uint8_t>(16, 0xAB)};
    snap.snapshot.icons.push_back(icon);
    AgentEventMessage snapParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(snap), snapParsed), "parse valid tray-snapshot");
    Check(snapParsed.snapshot.icons.size() == 1 && snapParsed.snapshot.icons[0].hwnd == icon.hwnd && snapParsed.snapshot.icons[0].pixels == icon.pixels &&
              snapParsed.snapshot.icons[0].flashing,
          "tray-snapshot lossless hwnd + pixels + flashing");

    AgentEventMessage dragOver{AgentEventType::DragOver, 0, {}, false, TrayHostYieldReason::None, POINT{50, 900}};
    AgentEventMessage dragParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(dragOver), dragParsed) && dragParsed.pt.x == 50 && dragParsed.pt.y == 900,
          "drag-over point round-trip");

    AgentEventMessage drop{AgentEventType::Drop, 7777, {}, false, TrayHostYieldReason::None, {}, L"app-1", {L"C:\\test\\a.txt", L"C:\\test\\b.txt"}};
    AgentEventMessage dropParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(drop), dropParsed) && dropParsed.opId == 7777 && dropParsed.key == drop.key &&
              dropParsed.paths == drop.paths,
          "drop payload + opId round-trip");

    AgentCommandMessage cmdParsed;
    Check(!ValidateAndParseAgentEvent(std::string(kMaxAgentMessageBytes + 1, 'a'), snapParsed), "reject event > 2MB");
    Check(!ValidateAndParseAgentCommand(std::string(kMaxAgentMessageBytes + 1, 'a'), cmdParsed), "reject cmd > 2MB");
    AgentEventMessage badDrop = drop;
    badDrop.key.clear();
    Check(!ValidateAndParseAgentEvent(SerializeAgentEvent(badDrop), dropParsed), "reject drop with empty key");
    badDrop = drop, badDrop.paths.assign(kMaxAgentDropPaths + 1, L"C:\\x.txt");
    Check(!IsValidAgentDropPayload(badDrop.key, badDrop.paths), "helper rejects drop count > 64");
    std::string overDropJson = R"({"evt":"drop","key":"k","paths":[)";
    for (size_t i = 0; i <= kMaxAgentDropPaths; ++i)
        overDropJson += (i > 0 ? R"(,"C:\\x")" : R"("C:\\x")");
    Check(!ValidateAndParseAgentEvent(overDropJson + "]}", dropParsed), "reject drop json with > 64 paths");
    const std::string longPathJson = std::string(R"({"evt":"drop","key":"k","paths":[")") + std::string(kMaxAgentPathLength + 1, 'a') + R"("]})";
    Check(!ValidateAndParseAgentEvent(longPathJson, dropParsed), "reject drop json with path > 4096");

    AgentTrayIconDto badIcon = icon;
    badIcon.width = kMaxAgentIconDimension + 1, badIcon.height = 1, badIcon.pixels.assign(static_cast<size_t>(badIcon.width) * 4, 0);
    Check(!IsValidAgentTrayIconDto(badIcon), "helper rejects icon > 256px");
    const std::string badIconJson = R"({"evt":"tray-snapshot","yielded":false,"yieldReason":"none","icons":[{"key":"k","hwnd":"1","uid":1,)"
                                    R"("callback":1,"version":4,"showTip":false,"tip":"","exe":"","pid":1,"imageVersion":"1","hidden":false,)"
                                    R"("added":"1","flashing":false,"width":257,"height":1,"pixels":"AAAA"}]})";
    Check(!ValidateAndParseAgentEvent(badIconJson, snapParsed), "reject snapshot json with icon > 256px");
    std::string tooManyIcons = R"({"evt":"tray-snapshot","yielded":false,"yieldReason":"none","icons":[)";
    for (size_t i = 0; i <= kMaxAgentTrayIcons; ++i)
        tooManyIcons += std::string(i > 0 ? "," : "") + R"({"key":"k","hwnd":"1","uid":1,"callback":1,"version":4,"showTip":false,)"
                                                        R"("tip":"","exe":"","pid":1,"imageVersion":"0","hidden":false,"added":"1","flashing":false})";
    Check(!ValidateAndParseAgentEvent(tooManyIcons + "]}", snapParsed), "reject snapshot json with > 128 icons");
}

void TestSupervisorPolicy()
{
    DesktopAgentSupervisorPolicy policy;
    Check(policy.RecordFailure(1000) && policy.NextDelayMs() == 500, "failure 1 schedules 500ms restart");
    Check(!policy.ShouldRestart(1499) && policy.ShouldRestart(1500), "should restart at 500ms");
    policy.MarkRestartDispatched();
    Check(!policy.ShouldRestart(2000), "dispatched restart clears pending flag");
    Check(policy.RecordFailure(2000) && policy.NextDelayMs() == 1500, "failure 2 backoff is 1500ms");
    Check(policy.RecordFailure(4000) && policy.NextDelayMs() == 4000, "failure 3 backoff is 4000ms");
    Check(!policy.RecordFailure(9000) && policy.exhausted && !policy.ShouldRestart(100000), "failure 4 exhausts attempts (degraded mode)");
    policy.RecordHealthy(100000);
    Check(policy.exhausted, "not yet reset before healthy window");
    policy.RecordHealthy(100000 + DesktopAgentSupervisorPolicy::kHealthyResetMs);
    Check(!policy.exhausted && policy.failures == 0 && policy.RecordFailure(200000), "healthy period resets failure window");
}

void TestLoopbackPipeAndClient()
{
    const std::wstring cmdPipe = UniquePipe(L"Cmd", 1), evtPipe = UniquePipe(L"Evt", 1);
    std::atomic_int trayCalls{0}, yieldCalls{0}, dropCalls{0};
    std::atomic_bool acceptDrop{true};
    std::wstring droppedKey;
    std::vector<std::wstring> droppedPaths;

    DesktopAgentCallbacks cb;
    cb.onTrayChanged = [&] { trayCalls.fetch_add(1); };
    cb.onYieldChanged = [&] { yieldCalls.fetch_add(1); };
    cb.onDragOver = [](POINT pt) { return AgentDragOverResult{pt.x >= 10 && pt.x <= 100, (pt.x >= 10 && pt.x <= 100) ? L"app-target" : L""}; };
    cb.onDropFilesAccepted = [&](const std::wstring& key, const std::vector<std::wstring>& paths) {
        return acceptDrop.load() && (droppedKey = key, droppedPaths = paths, dropCalls.fetch_add(1), true);
    };

    DesktopAgentClient client(cb, DesktopAgentClientOptions{0, 0, 1, cmdPipe, evtPipe, false, true});
    Check(WaitUntil([&] { return client.EvtServerReady(); }), "DesktopAgentClient Evt pipe server ready");

    AgentEventMessage snap1;
    snap1.type = AgentEventType::TraySnapshot;
    snap1.snapshot.icons.push_back(
        AgentTrayIconDto{L"hwnd:1:1", 0, 0, 0, 0, false, L"", L"c:\\demo\\qq.exe", 888, 42, false, 0, true, 2, 2, std::vector<uint8_t>(16, 0x7F)});
    std::string replyRaw;
    AgentEventReply replyParsed;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap1), replyRaw, 2000), "send snap1 over same-user pipe");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.ok, "snap1 accepted");
    Check(client.Icons().size() == 1 && client.Icons()[0].image && client.Icons()[0].image->ByteSize() == 16, "cached icon image present");
    Check(client.Flashing().size() == 1 && client.FlashingIcons().size() == 1, "flashing state exposed");

    AgentEventMessage snap2 = snap1;
    snap2.snapshot.icons[0].width = 0, snap2.snapshot.icons[0].height = 0, snap2.snapshot.icons[0].pixels.clear();
    snap2.snapshot.yielded = true, snap2.snapshot.yieldReason = TrayHostYieldReason::OtherProcess;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap2), replyRaw, 2000), "send snap2 with cached imageVersion");
    Check(client.Icons().size() == 1 && client.Icons()[0].image && client.Icons()[0].image->ByteSize() == 16, "reused cached image by version");
    Check(client.Yielded() && client.YieldReason() == TrayHostYieldReason::OtherProcess, "yield state updated");
    Check(trayCalls.load() >= 2 && yieldCalls.load() >= 1, "tray and yield callbacks fired");

    AgentEventMessage drag{AgentEventType::DragOver, 0, {}, false, TrayHostYieldReason::None, POINT{50, 10}};
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drag), replyRaw, 2000), "send drag-over");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.allow && replyParsed.key == L"app-target", "drag-over allowed");

    AgentEventMessage drop{AgentEventType::Drop, 101, {}, false, TrayHostYieldReason::None, {}, L"app-target", {L"C:\\file1.txt"}};
    acceptDrop.store(false);
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drop), replyRaw, 2000), "send rejected drop");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.ok && !replyParsed.allow && dropCalls.load() == 0,
          "rejected drop returns allow=false");

    acceptDrop.store(true), drop.opId = 102;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drop), replyRaw, 2000), "send accepted drop");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.ok && replyParsed.allow && dropCalls.load() == 1 &&
              droppedKey == L"app-target" && droppedPaths.size() == 1,
          "drop callback accepted files");
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drop), replyRaw, 2000), "send duplicate opId drop");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.ok && !replyParsed.allow && dropCalls.load() == 1,
          "duplicate drop opId deduplicated");
    client.Shutdown();
}

void TestEvtPeerBindingAndVerifyHelper()
{
    const std::wstring evtPipe = UniquePipe(L"UnboundEvt", 10);
    std::atomic_int trayCalls{0};
    DesktopAgentCallbacks cb;
    cb.onTrayChanged = [&] { trayCalls.fetch_add(1); };
    DesktopAgentClient client(cb, DesktopAgentClientOptions{0, 0, 1, UniquePipe(L"UnboundCmd", 10), evtPipe, false, false});
    Check(WaitUntil([&] { return client.EvtServerReady(); }), "unbound Evt test server ready");
    AgentEventMessage snap;
    snap.type = AgentEventType::TraySnapshot;
    std::string replyRaw;
    Check(!SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap), replyRaw, 1000) && trayCalls.load() == 0,
          "production Evt pipe rejects unbound peer before HandleEvent");
    client.Shutdown();

    const std::wstring peerPipeName = UniquePipe(L"PeerVerify", 11);
    const winrt::handle srv{CreateSameUserNamedPipeServer(peerPipeName)}, evt{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        quit{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(srv.get() != INVALID_HANDLE_VALUE && evt && quit, "create loopback pipe for VerifyNamedPipePeerProcess");
    std::atomic_bool serverOk{false}, wrongExeRejected{false};
    std::thread serverThread([&] {
        OVERLAPPED ov{};
        ov.hEvent = evt.get();
        if (ConnectNamedPipeOverlapped(srv.get(), ov, quit.get())) {
            AgentPeerIdentity p1, p2;
            winrt::handle proc;
            serverOk.store(VerifyNamedPipePeerProcess(srv.get(), true, ModulePath(), false, p1, &proc) && p1.valid && p1.pid == GetCurrentProcessId() && proc);
            wrongExeRejected.store(!VerifyNamedPipePeerProcess(srv.get(), true, L"C:\\nonexistent\\other.exe", false, p2));
            DisconnectNamedPipe(srv.get());
        }
    });
    const winrt::handle cli{ConnectSameUserPipeClient(peerPipeName, 2000, quit.get())};
    Check(cli.get() != INVALID_HANDLE_VALUE && cli, "connect client for VerifyNamedPipePeerProcess");
    if (cli.get() != INVALID_HANDLE_VALUE) {
        AgentPeerIdentity srvPeer;
        Check(VerifyNamedPipePeerProcess(cli.get(), false, ModulePath(), false, srvPeer) && srvPeer.pid == GetCurrentProcessId(),
              "VerifyNamedPipePeerProcess verified server side");
    }
    if (serverThread.joinable())
        serverThread.join();
    Check(serverOk.load() && wrongExeRejected.load(), "VerifyNamedPipePeerProcess checks exe path and PID");
}

void TestSupervisorExhaustionAndHostRoundTrip()
{
    std::atomic_int spawnAttempts{0};
    DesktopAgentClientOptions failOpts{
        0,
        0,
        1,
        UniquePipe(L"FailCmd", 2),
        UniquePipe(L"FailEvt", 2),
        true,
        false,
        false,
        [&](const std::wstring&, DWORD& errOut) -> HANDLE { return spawnAttempts.fetch_add(1), errOut = ERROR_ACCESS_DENIED, nullptr; },
        50};
    {
        DesktopAgentClient client({}, failOpts);
        Check(WaitUntil([&] { return client.Degraded(); }, 12000), "supervisor enters degraded mode after max restarts");
        Check(spawnAttempts.load() == DesktopAgentSupervisorPolicy::kMaxRestartAttempts + 1, "exact 1 initial + 3 restart attempts");
        client.Shutdown();
    }

    const std::wstring cmdPipe = UniquePipe(L"HostCmd", 3), evtPipe = UniquePipe(L"HostEvt", 3);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 123456;
    std::atomic<int> hostExitCode{-1};
    std::thread hostThread([&] { hostExitCode.store(RunDesktopAgent(selfPid, gen, GetModuleHandleW(nullptr), cmdPipe.c_str(), evtPipe.c_str())); });
    std::atomic<DWORD> notifiedPid{0};
    DesktopAgentCallbacks hostCb;
    hostCb.onAgentPidChanged = [&](DWORD pid) { notifiedPid.store(pid); };
    {
        DesktopAgentClient client(hostCb, MakeLoopbackOpts(cmdPipe, evtPipe, selfPid, gen, 3000));
        Check(WaitUntil([&] { return client.Running(); }, 5000) && client.AgentPid() == selfPid && notifiedPid.load() == selfPid,
              "DesktopAgentClient connected to RunDesktopAgent");
        client.SyncDropBars({{1, RECT{10, 10, 110, 50}, true}});
        client.SyncDropBars({});
        client.Shutdown();
    }
    if (hostThread.joinable())
        hostThread.join();
    Check(hostExitCode.load() == 0, "RunDesktopAgent exited cleanly on quit");
}

void TestSlowStaCommandTimeoutNoUaf()
{
    const std::wstring cmdPipe = UniquePipe(L"SlowCmd", 20);
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int slowStarted{0}, slowFinished{0}, syncCalled{0};
    DesktopAgentHostOptions hostOpts{cmdPipe, UniquePipe(L"SlowEvt", 20), 50, stopEvt.get(), [&](const AgentCommandMessage& cmd) {
                                         if (cmd.type == AgentCommandType::SetTrayPlaces)
                                             slowStarted.fetch_add(1), std::this_thread::sleep_for(std::chrono::milliseconds(180)), slowFinished.fetch_add(1);
                                         else if (cmd.type == AgentCommandType::SyncDropBars)
                                             syncCalled.fetch_add(1);
                                     }};
    std::atomic<int> exitCode{-1};
    std::thread hostThread([&] { exitCode.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 77, GetModuleHandleW(nullptr), hostOpts)); });

    const winrt::handle pipe{ConnectSameUserPipeClient(cmdPipe, 3000)};
    Check(pipe.get() != INVALID_HANDLE_VALUE && pipe, "connected to slow-STA host");
    if (pipe.get() != INVALID_HANDLE_VALUE) {
        std::string rawReply;
        AgentCommandReply reply;
        AgentCommandMessage slowCmd{AgentCommandType::SetTrayPlaces, 0, false, RECT{0, 0, 10, 10}};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(slowCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  !reply.ok && reply.error == L"sta_timeout",
              "slow STA command returned sta_timeout without blocking caller");
        AgentCommandMessage queuedCmd{AgentCommandType::SyncDropBars};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(queuedCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  !reply.ok && reply.error == L"sta_timeout",
              "queued command behind slow STA task timed out and cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        Check(slowStarted.load() == 1 && slowFinished.load() == 1 && syncCalled.load() == 0,
              "running slow STA task completed without UAF and cancelled queued task was skipped");
        AgentCommandMessage pingCmd{AgentCommandType::Ping}, quitCmd{AgentCommandType::Quit};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(pingCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  reply.ok,
              "host remains healthy after STA timeout");
        SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(quitCmd), rawReply, 2000);
    }
    SetEvent(stopEvt.get());
    if (hostThread.joinable())
        hostThread.join();
    Check(exitCode.load() == 0, "slow STA host exited cleanly");
}

void TestStalledReaderShutdownUnder500ms()
{
    const std::wstring evtPipe = UniquePipe(L"StallEvt", 30);
    DesktopAgentClient client({}, DesktopAgentClientOptions{0, 0, 1, UniquePipe(L"StallEvtCmd", 30), evtPipe, false, true});
    Check(WaitUntil([&] { return client.EvtServerReady(); }), "stalled Evt server ready");

    const winrt::handle evtCli{ConnectSameUserPipeClient(evtPipe, 2000)};
    Check(evtCli.get() != INVALID_HANDLE_VALUE && evtCli, "stalled Evt client connected");
    if (evtCli.get() != INVALID_HANDLE_VALUE) {
        AgentEventMessage snap;
        snap.type = AgentEventType::TraySnapshot;
        WriteNoReadOverlapped(evtCli.get(), SerializeAgentEvent(snap));
    }
    const ULONGLONG t0 = GetTickCount64();
    client.Shutdown();
    Check(GetTickCount64() - t0 < 500, "Evt server shuts down in < 500ms despite stalled reader");

    const std::wstring cmdPipe = UniquePipe(L"StallCmd", 31);
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    DesktopAgentHostOptions hostOpts{cmdPipe, UniquePipe(L"StallHostEvt", 31), 5000, stopEvt.get()};
    std::atomic<int> hostExit{-1};
    std::thread hostThread([&] { hostExit.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 88, GetModuleHandleW(nullptr), hostOpts)); });

    const winrt::handle cmdCli{ConnectSameUserPipeClient(cmdPipe, 2000)};
    Check(cmdCli.get() != INVALID_HANDLE_VALUE && cmdCli, "stalled Cmd client connected");
    if (cmdCli.get() != INVALID_HANDLE_VALUE) {
        AgentCommandMessage ping;
        WriteNoReadOverlapped(cmdCli.get(), SerializeAgentCommand(ping));
    }
    const ULONGLONG t1 = GetTickCount64();
    SetEvent(stopEvt.get());
    if (hostThread.joinable())
        hostThread.join();
    Check(hostExit.load() == 0 && GetTickCount64() - t1 < 500, "Cmd server shuts down in < 500ms despite stalled reader");
}

void TestOpTrackerNoReplaySyncBarsAndAckState()
{
    RecentOpTracker tracker;
    for (uint64_t i = 1; i <= RecentOpTracker::kMaxSeenOps; ++i)
        Check(tracker.MarkSeenIfNew(i), "first 64 opIds accepted");
    Check(!tracker.MarkSeenIfNew(RecentOpTracker::kMaxSeenOps), "recent opId deduplicated");
    Check(tracker.MarkSeenIfNew(RecentOpTracker::kMaxSeenOps + 1), "65th opId accepted");
    Check(tracker.MarkSeenIfNew(1), "oldest opId evicted after capacity 64");

    TraySnapshotAckState ack;
    AgentTraySnapshot snap;
    AgentTrayIconDto dto;
    dto.key = L"k1", dto.imageVersion = 42, dto.width = 1, dto.height = 1, dto.pixels = {1, 2, 3, 4};
    snap.icons.push_back(dto);
    ack.RecordAck(1, snap, false);
    Check(!ack.IsConfirmed(1, 42), "failed ack does not mark imageVersion confirmed");
    ack.RecordAck(1, snap, true);
    Check(ack.IsConfirmed(1, 42), "successful ack marks imageVersion confirmed");
    Check(!ack.IsConfirmed(2, 42), "session bump resets confirmed imageVersions");

    const std::wstring cmdPipe = UniquePipe(L"MockCmd", 40), evtPipe = UniquePipe(L"MockEvt", 40);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 999;
    std::atomic_int syncCount{0}, sendCount{0}, uncertainCount{0};
    std::atomic_bool seenOpId{false};

    MockCmdServer mockCmd(
        cmdPipe, selfPid, gen,
        [&](const AgentCommandMessage& cmd, AgentCommandReply& reply) -> bool {
            if (cmd.type == AgentCommandType::SyncDropBars)
                reply.ok = (syncCount.fetch_add(1) + 1 >= 2);
            else if (cmd.type == AgentCommandType::TraySend && cmd.key == L"icon1")
                return sendCount.fetch_add(1), seenOpId.store(cmd.opId != 0), false; // Drop after write without replying.
            else if (cmd.type == AgentCommandType::TraySend)
                uncertainCount.fetch_add(1), reply.ok = false, reply.error = L"sta_timeout";
            else
                reply.ok = true;
            return true;
        },
        true);

    DesktopAgentClient client({}, MakeLoopbackOpts(cmdPipe, evtPipe, selfPid, gen));
    Check(WaitUntil([&] { return client.Running(); }), "mock Cmd server connected");

    const std::vector<AgentBarRect> bars{{1, RECT{0, 0, 100, 40}, true}};
    client.SyncDropBars(bars), client.SyncDropBars(bars), client.SyncDropBars(bars);
    Check(syncCount.load() == 2, "SyncDropBars retried after failure and skipped once confirmed");
    Check(client.Send(L"", tray::HostAction::Left, POINT{5, 5}) == HostSendStatus::NotSent, "empty-key TraySend fails pre-write as NotSent");
    Check(client.Send(L"icon1", tray::HostAction::Left, POINT{5, 5}) == HostSendStatus::Unknown && sendCount.load() == 1 && seenOpId.load(),
          "TraySend sets opId, never auto-replays, and returns Unknown when server drops after write");

    // Verify TrayModel::ClickHosted skips Explorer fallback on HostSendStatus::Unknown.
    TrayModel model(nullptr, 0);
    model.SetDesktopAgent(&client);
    TrayIcon uncertainIcon;
    uncertainIcon.hostKey = L"test:uncertain", uncertainIcon.tip = L"AtriumTestShouldNotFallback";
    const DWORD beforeSynth = TrayModel::LastSyntheticInput();
    model.Click(uncertainIcon, false, RECT{0, 0, 20, 20});
    Check(uncertainCount.load() == 1 && TrayModel::LastSyntheticInput() == beforeSynth, "ClickHosted with Unknown status does not fall back to Explorer");

    mockCmd.Stop();
    client.Shutdown();
    Check(client.Send(L"icon1", tray::HostAction::Left, POINT{5, 5}) == HostSendStatus::NotSent, "TraySend returns NotSent when client is not running");
}

void TestLaunchedProcessMatchAndReject()
{
    // Note: in-process loopback tests often return nullptr from spawnAgentOverride (matching ShellLaunch's ExplorerComMedium route
    // where no direct process handle exists), whereas direct ShellLaunch returns a live process handle that SpawnAndConnect must match.
    const std::wstring peerPipe = UniquePipe(L"LaunchMatch", 60);
    const winrt::handle srv{CreateSameUserNamedPipeServer(peerPipe)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        quit{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    AgentPeerIdentity selfPeer;
    std::thread srvThread([&] {
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (ConnectNamedPipeOverlapped(srv.get(), ov, quit.get()))
            VerifyNamedPipePeerProcess(srv.get(), true, ModulePath(), false, selfPeer), DisconnectNamedPipe(srv.get());
    });
    const winrt::handle cli{ConnectSameUserPipeClient(peerPipe, 2000, quit.get())};
    if (srvThread.joinable())
        srvThread.join();

    HANDLE dupSelfRaw = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &dupSelfRaw, 0, FALSE, DUPLICATE_SAME_ACCESS);
    const winrt::handle dupSelf{dupSelfRaw};
    Check(dupSelf && selfPeer.valid && VerifyLaunchedProcessMatchesPeer(dupSelf.get(), selfPeer),
          "VerifyLaunchedProcessMatchesPeer accepts matching process handle");
    Check(!VerifyLaunchedProcessMatchesPeer(nullptr, selfPeer), "VerifyLaunchedProcessMatchesPeer rejects null handle");
    AgentPeerIdentity wrongPidPeer = selfPeer, wrongTimePeer = selfPeer;
    wrongPidPeer.pid = selfPeer.pid + 1, wrongTimePeer.creationTime.dwLowDateTime ^= 1U;
    Check(!VerifyLaunchedProcessMatchesPeer(dupSelf.get(), wrongPidPeer) && !VerifyLaunchedProcessMatchesPeer(dupSelf.get(), wrongTimePeer),
          "VerifyLaunchedProcessMatchesPeer rejects mismatched PID and creation time");

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring exe = ModulePath(), cmdLine = L"\"" + exe + L"\"";
    const BOOL spawned = CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    Check(spawned != FALSE && pi.hProcess != nullptr, "created suspended child process for PID mismatch test");
    if (spawned) {
        CloseHandle(pi.hThread);
        const std::wstring cmdPipe = UniquePipe(L"MismatchCmd", 61), evtPipe = UniquePipe(L"MismatchEvt", 61);
        std::atomic_int pingCount{0};
        MockCmdServer decoyCmd(
            cmdPipe, GetCurrentProcessId(), 610, [&](const AgentCommandMessage&, AgentCommandReply&) { return pingCount.fetch_add(1), false; }, true);
        auto badOpts = MakeLoopbackOpts(cmdPipe, evtPipe, GetCurrentProcessId(), 610, 400);
        badOpts.spawnAgentOverride = [&](const std::wstring&, DWORD& errOut) -> HANDLE {
            errOut = 0;
            HANDLE h = nullptr;
            DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
            return h;
        };
        {
            DesktopAgentClient client({}, badOpts);
            Check(WaitUntil([&] { return client.Degraded(); }, 10000) && !client.Running() && pingCount.load() == 0,
                  "SpawnAndConnect rejects Cmd pipe peer whose PID mismatches launchedHandle before Ping");
            decoyCmd.Stop();
            client.Shutdown();
        }
        TerminateProcess(pi.hProcess, 0), WaitForSingleObject(pi.hProcess, 2000), CloseHandle(pi.hProcess);
    }

    const std::wstring okCmdPipe = UniquePipe(L"MatchCmd", 62), okEvtPipe = UniquePipe(L"MatchEvt", 62);
    const DWORD selfPid = GetCurrentProcessId();
    MockCmdServer okCmd(okCmdPipe, selfPid, 620, [](const AgentCommandMessage&, AgentCommandReply& r) { return r.ok = true, true; });
    auto okOpts = MakeLoopbackOpts(okCmdPipe, okEvtPipe, selfPid, 620);
    okOpts.spawnAgentOverride = [](const std::wstring&, DWORD& errOut) -> HANDLE {
        errOut = 0;
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return h;
    };
    {
        DesktopAgentClient client({}, okOpts);
        Check(WaitUntil([&] { return client.Running(); }) && client.AgentPid() == selfPid, "SpawnAndConnect accepts Cmd pipe peer matching launchedHandle");
        okCmd.Stop();
        client.Shutdown();
    }
}

void TestHostSyncDropBarsFailurePropagates()
{
    const std::wstring cmdPipe = UniquePipe(L"SyncFailCmd", 70);
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    DesktopAgentHostOptions hostOpts{cmdPipe, UniquePipe(L"SyncFailEvt", 70), 5000, stopEvt.get(), {}, true};
    std::atomic<int> exitCode{-1};
    std::thread hostThread([&] { exitCode.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 700, GetModuleHandleW(nullptr), hostOpts)); });

    const winrt::handle pipe{ConnectSameUserPipeClient(cmdPipe, 3000)};
    Check(pipe.get() != INVALID_HANDLE_VALUE && pipe, "connected to SyncDropBars failure test host");
    if (pipe.get() != INVALID_HANDLE_VALUE) {
        std::string rawReply;
        AgentCommandReply reply;
        AgentCommandMessage syncCmd{AgentCommandType::SyncDropBars, 0, false, {}, L"", tray::HostAction::Left, {}, {}, {{1, RECT{0, 0, 100, 40}, true}}};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(syncCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  !reply.ok && reply.error == L"overlay_sync_failed",
              "SyncDropBars overlay failure propagates reply.ok == false");
        AgentCommandMessage emptySync{AgentCommandType::SyncDropBars}, quitCmd{AgentCommandType::Quit};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(emptySync), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  reply.ok,
              "SyncDropBars with no visible bars returns reply.ok == true");
        SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(quitCmd), rawReply, 2000);
    }
    SetEvent(stopEvt.get());
    if (hostThread.joinable())
        hostThread.join();
    Check(exitCode.load() == 0, "SyncDropBars failure host exited cleanly");
}

void TestHostOversizedIconAndReconnectResync()
{
    TraySnapshotAckState ack;
    HostIcon oversized;
    oversized.key = L"big:1", oversized.imageVersion = 777;
    oversized.image =
        std::make_shared<const IconImage>(kMaxAgentIconDimension + 1, 2, std::vector<uint8_t>(static_cast<size_t>(kMaxAgentIconDimension + 1) * 2 * 4, 0xFF));
    size_t budgetUsed = 0;
    bool deferred = false;
    const AgentTrayIconDto bigDto = BuildAgentTrayIconDto(oversized, 0, 1, 1024 * 1024, budgetUsed, ack, deferred);
    AgentTraySnapshot snap;
    snap.icons.push_back(bigDto);
    Check(bigDto.imageVersion == 0 && bigDto.pixels.empty() && !deferred && !ack.HasUnconfirmedWithEmptyPixels(1, snap),
          "unencodable oversized icon gets imageVersion=0 and does not trigger HasUnconfirmedWithEmptyPixels loop");

    HostIcon normal;
    normal.key = L"ok:1", normal.imageVersion = 888;
    normal.image = std::make_shared<const IconImage>(2, 2, std::vector<uint8_t>(16, 0xAB));
    deferred = false;
    const AgentTrayIconDto deferredDto = BuildAgentTrayIconDto(normal, 0, 1, 8, budgetUsed, ack, deferred);
    Check(deferred && deferredDto.imageVersion == 888 && deferredDto.pixels.empty(), "over-budget valid icon defers bitmap cleanly");
    deferred = false;
    const AgentTrayIconDto fitDto = BuildAgentTrayIconDto(normal, 0, 1, 64, budgetUsed, ack, deferred);
    Check(!deferred && fitDto.imageVersion == 888 && fitDto.pixels.size() == 16, "in-budget valid icon encodes pixels");

    const std::wstring evtPipe = UniquePipe(L"ResyncEvt", 80);
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        phase1Done{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, phase2Ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
        resyncSeen{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, hostReady{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int dropAttempts{0};
    std::atomic_bool sawYield{false}, sawResyncSnapshot{false};
    std::thread mockEvt([&] {
        for (int i = 0; i < 2; ++i) {
            const winrt::handle srv{CreateSameUserNamedPipeServer(evtPipe)};
            OVERLAPPED ov{};
            ov.hEvent = ev.get();
            std::string raw;
            bool disc = false;
            if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stopEvt.get()))
                return;
            if (ReadPipeMessageOverlapped(srv.get(), ov, stopEvt.get(), raw, disc))
                dropAttempts.fetch_add(1);
            DisconnectNamedPipe(srv.get());
        }
        SetEvent(phase1Done.get());
        const winrt::handle srv{CreateSameUserNamedPipeServer(evtPipe)};
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (srv.get() == INVALID_HANDLE_VALUE)
            return;
        SetEvent(phase2Ready.get());
        if (!ConnectNamedPipeOverlapped(srv.get(), ov, stopEvt.get()))
            return;
        for (std::string raw; WaitForSingleObject(stopEvt.get(), 0) == WAIT_TIMEOUT;) {
            bool disc = false;
            AgentEventMessage evt;
            if (!ReadPipeMessageOverlapped(srv.get(), ov, stopEvt.get(), raw, disc) || !ValidateAndParseAgentEvent(raw, evt))
                break;
            if (evt.type == AgentEventType::TrayYield)
                sawYield.store(true);
            else if (evt.type == AgentEventType::TraySnapshot)
                sawResyncSnapshot.store(true);
            if (!WritePipeMessageOverlapped(srv.get(), ov, stopEvt.get(), SerializeAgentEventReply(AgentEventReply{true})))
                break;
            if (sawYield.load() && sawResyncSnapshot.load()) {
                SetEvent(resyncSeen.get());
                break;
            }
        }
        DisconnectNamedPipe(srv.get());
    });

    std::function<void()> pushSnap, pushYield;
    DesktopAgentHostOptions hostOpts{UniquePipe(L"ResyncCmd", 80), evtPipe, 5000, stopEvt.get(), {}, false, [&](auto s, auto y) {
                                         pushSnap = std::move(s), pushYield = std::move(y), SetEvent(hostReady.get());
                                     }};
    std::atomic<int> hostExit{-1};
    std::thread hostThread([&] { hostExit.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 800, GetModuleHandleW(nullptr), hostOpts)); });
    Check(WaitForSingleObject(hostReady.get(), 3000) == WAIT_OBJECT_0, "resync test host ready");
    if (pushSnap)
        pushSnap();
    Check(WaitForSingleObject(phase1Done.get(), 5000) == WAIT_OBJECT_0 && dropAttempts.load() == 2, "initial TraySnapshot exhausted 2 failed Evt attempts");
    Check(WaitForSingleObject(phase2Ready.get(), 3000) == WAIT_OBJECT_0, "phase 2 healthy Evt server ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (pushYield)
        pushYield();
    Check(WaitForSingleObject(resyncSeen.get(), 5000) == WAIT_OBJECT_0 && sawYield.load() && sawResyncSnapshot.load(),
          "Evt reconnect resyncs pending TraySnapshot via resyncNeeded_");
    SetEvent(stopEvt.get());
    if (mockEvt.joinable())
        mockEvt.join();
    if (hostThread.joinable())
        hostThread.join();
    Check(hostExit.load() == 0, "resync test host exited cleanly");
}

void TestTrayModelSendHostedNoUntrustedRelay()
{
    constexpr UINT kCallbackMsg = WM_APP + 77;
    std::atomic_int relayCount{0};
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* counter = reinterpret_cast<std::atomic_int*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == kCallbackMsg && counter)
            counter->fetch_add(1);
        return DefWindowProcW(hwnd, msg, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"AtriumTestNoRelayTarget";
    RegisterClassExW(&wc);
    HWND targetHwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &relayCount);
    Check(targetHwnd != nullptr, "created message-only test window");

    const std::wstring cmdPipe = UniquePipe(L"RelayCmd", 50), evtPipe = UniquePipe(L"RelayEvt", 50);
    const DWORD selfPid = GetCurrentProcessId();
    std::atomic_int traySendObserved{0};
    MockCmdServer mockCmd(cmdPipe, selfPid, 505, [&](const AgentCommandMessage& cmd, AgentCommandReply& reply) {
        reply.ok = true;
        if (cmd.type == AgentCommandType::TraySend)
            traySendObserved.fetch_add(1), reply.sendStatus = HostSendStatus::NotSent, reply.higherIntegrity = true;
        return true;
    });

    DesktopAgentClient client({}, MakeLoopbackOpts(cmdPipe, evtPipe, selfPid, 505, 2000, true));
    Check(WaitUntil([&] { return client.Running() && client.EvtServerReady(); }), "relay test client connected");

    AgentEventMessage snap;
    snap.type = AgentEventType::TraySnapshot;
    AgentTrayIconDto dto;
    dto.key = L"test:higher", dto.hwnd = reinterpret_cast<uintptr_t>(targetHwnd), dto.uid = 1, dto.callback = kCallbackMsg, dto.version = 4;
    dto.pid = selfPid;
    snap.snapshot.icons.push_back(dto);
    std::string evtReply;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap), evtReply, 2000), "seeded snapshot with target hwnd");

    TrayModel model(nullptr, 0);
    model.SetDesktopAgent(&client);
    const HostSendStatus status = model.SendHosted(L"test:higher", tray::HostAction::Middle, POINT{5, 5}, RECT{0, 0, 20, 20});
    for (MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);)
        TranslateMessage(&msg), DispatchMessageW(&msg);
    Check(status == HostSendStatus::NotSent && traySendObserved.load() == 1 && relayCount.load() == 0,
          "Core skipped blind High-token SendNotifyMessageW relay for higherIntegrity target");

    mockCmd.Stop();
    client.Shutdown();
    if (targetHwnd)
        DestroyWindow(targetHwnd);
}

} // namespace

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestPipeNames();
    TestCommandSerialization();
    TestEventSerializationAndBounds();
    TestSupervisorPolicy();
    TestLoopbackPipeAndClient();
    TestEvtPeerBindingAndVerifyHelper();
    TestSupervisorExhaustionAndHostRoundTrip();
    TestSlowStaCommandTimeoutNoUaf();
    TestStalledReaderShutdownUnder500ms();
    TestOpTrackerNoReplaySyncBarsAndAckState();
    TestTrayModelSendHostedNoUntrustedRelay();
    TestLaunchedProcessMatchAndReject();
    TestHostSyncDropBarsFailurePropagates();
    TestHostOversizedIconAndReconnectResync();
    winrt::uninit_apartment();
    return g_failures == 0 ? 0 : 1;
}
