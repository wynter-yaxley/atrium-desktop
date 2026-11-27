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

void TestPipeNames()
{
    const std::wstring cmd = DesktopAgentCommandPipeName(1, 4242, 7);
    const std::wstring evt = DesktopAgentEventPipeName(1, 4242, 7);
    Check(cmd == L"\\\\.\\pipe\\Atrium.DesktopAgent.Cmd.1.4242.7", "command pipe name format");
    Check(evt == L"\\\\.\\pipe\\Atrium.DesktopAgent.Evt.1.4242.7", "event pipe name format");
}

void TestCommandSerialization()
{
    AgentCommandMessage cmd;
    cmd.type = AgentCommandType::TraySend;
    cmd.opId = 9001;
    cmd.key = L"guid:{1234}";
    cmd.action = tray::HostAction::Double;
    cmd.at = POINT{120, 400};
    cmd.cell = RECT{100, 380, 140, 420};

    AgentCommandMessage parsed;
    Check(ValidateAndParseAgentCommand(SerializeAgentCommand(cmd), parsed), "parse valid tray-send");
    Check(parsed.type == AgentCommandType::TraySend && parsed.opId == 9001 && parsed.key == cmd.key && parsed.action == cmd.action,
          "tray-send fields + opId match");
    Check(parsed.at.x == 120 && parsed.at.y == 400 && EqualRect(&parsed.cell, &cmd.cell), "tray-send geometry matches");

    AgentCommandMessage sync;
    sync.type = AgentCommandType::SyncDropBars;
    sync.bars = {
        {1, RECT{0, 1000, 1920, 1080}, true},
        {2, RECT{1920, 1000, 3840, 1080}, false},
    };
    AgentCommandMessage syncParsed;
    Check(ValidateAndParseAgentCommand(SerializeAgentCommand(sync), syncParsed), "parse valid sync-drop-bars");
    Check(syncParsed.bars == sync.bars, "sync-drop-bars round-trip matches");

    AgentCommandReply reply;
    reply.ok = true;
    reply.pid = 777;
    reply.generation = (1ULL << 60) + 9;
    reply.sendStatus = HostSendStatus::Partial;
    reply.higherIntegrity = true;
    AgentCommandReply replyParsed;
    Check(ValidateAndParseAgentCommandReply(SerializeAgentCommandReply(reply), replyParsed), "parse command reply");
    Check(replyParsed.ok && replyParsed.pid == 777 && replyParsed.generation == reply.generation && replyParsed.sendStatus == HostSendStatus::Partial &&
              replyParsed.higherIntegrity,
          "command reply fields round-trip");
}

void TestEventSerializationAndBounds()
{
    AgentEventMessage snap;
    snap.type = AgentEventType::TraySnapshot;
    snap.snapshot.yielded = false;
    snap.snapshot.yieldReason = TrayHostYieldReason::None;
    AgentTrayIconDto icon;
    icon.key = L"hwnd:100:1";
    icon.hwnd = 0x123456789ABCDEFULL;
    icon.uid = 1;
    icon.callback = WM_APP + 5;
    icon.version = 4;
    icon.showTip = true;
    icon.tip = L"App Tip";
    icon.exe = L"c:\\app\\test.exe";
    icon.pid = 555;
    icon.imageVersion = 99;
    icon.hidden = false;
    icon.added = 12;
    icon.flashing = true;
    icon.width = 2;
    icon.height = 2;
    icon.pixels = std::vector<uint8_t>(16, 0xAB);
    snap.snapshot.icons.push_back(icon);

    AgentEventMessage snapParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(snap), snapParsed), "parse valid tray-snapshot");
    Check(snapParsed.snapshot.icons.size() == 1 && snapParsed.snapshot.icons[0].hwnd == icon.hwnd && snapParsed.snapshot.icons[0].pixels == icon.pixels &&
              snapParsed.snapshot.icons[0].flashing,
          "tray-snapshot lossless hwnd + pixels + flashing");

    AgentEventMessage dragOver;
    dragOver.type = AgentEventType::DragOver;
    dragOver.pt = POINT{50, 900};
    AgentEventMessage dragParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(dragOver), dragParsed), "parse drag-over");
    Check(dragParsed.pt.x == 50 && dragParsed.pt.y == 900, "drag-over point round-trip");

    AgentEventMessage drop;
    drop.type = AgentEventType::Drop;
    drop.opId = 7777;
    drop.key = L"app-1";
    drop.paths = {L"C:\\test\\a.txt", L"C:\\test\\b.txt"};
    AgentEventMessage dropParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(drop), dropParsed), "parse valid drop");
    Check(dropParsed.opId == 7777 && dropParsed.key == drop.key && dropParsed.paths == drop.paths, "drop payload + opId round-trip");

    // Reject oversized / invalid payloads.
    AgentCommandMessage cmdParsed;
    Check(!ValidateAndParseAgentEvent(std::string(kMaxAgentMessageBytes + 1, 'a'), snapParsed), "reject event > 2MB");
    Check(!ValidateAndParseAgentCommand(std::string(kMaxAgentMessageBytes + 1, 'a'), cmdParsed), "reject cmd > 2MB");

    AgentEventMessage badDrop = drop;
    badDrop.key.clear();
    Check(!ValidateAndParseAgentEvent(SerializeAgentEvent(badDrop), dropParsed), "reject drop with empty key");

    badDrop = drop;
    badDrop.paths.assign(kMaxAgentDropPaths + 1, L"C:\\x.txt");
    Check(!IsValidAgentDropPayload(badDrop.key, badDrop.paths), "helper rejects drop count > 64");
    const std::string overDropJson = []() {
        std::string s = R"({"evt":"drop","key":"k","paths":[)";
        for (size_t i = 0; i <= kMaxAgentDropPaths; ++i) {
            if (i > 0)
                s += ',';
            s += R"("C:\\x")";
        }
        s += "]}";
        return s;
    }();
    Check(!ValidateAndParseAgentEvent(overDropJson, dropParsed), "reject drop json with > 64 paths");

    const std::string longPathJson = std::string(R"({"evt":"drop","key":"k","paths":[")") + std::string(kMaxAgentPathLength + 1, 'a') + R"("]})";
    Check(!ValidateAndParseAgentEvent(longPathJson, dropParsed), "reject drop json with path > 4096");

    AgentTrayIconDto badIcon = icon;
    badIcon.width = kMaxAgentIconDimension + 1;
    badIcon.height = 1;
    badIcon.pixels.assign(static_cast<size_t>(badIcon.width) * 4, 0);
    Check(!IsValidAgentTrayIconDto(badIcon), "helper rejects icon > 256px");

    const std::string badIconJson = R"({"evt":"tray-snapshot","yielded":false,"yieldReason":"none","icons":[{"key":"k","hwnd":"1","uid":1,)"
                                    R"("callback":1,"version":4,"showTip":false,"tip":"","exe":"","pid":1,"imageVersion":"1","hidden":false,)"
                                    R"("added":"1","flashing":false,"width":257,"height":1,"pixels":"AAAA"}]})";
    Check(!ValidateAndParseAgentEvent(badIconJson, snapParsed), "reject snapshot json with icon > 256px");

    const std::string tooManyIcons = []() {
        std::string s = R"({"evt":"tray-snapshot","yielded":false,"yieldReason":"none","icons":[)";
        for (size_t i = 0; i <= kMaxAgentTrayIcons; ++i) {
            if (i > 0)
                s += ',';
            s += R"({"key":"k","hwnd":"1","uid":1,"callback":1,"version":4,"showTip":false,"tip":"","exe":"",)"
                 R"("pid":1,"imageVersion":"0","hidden":false,"added":"1","flashing":false})";
        }
        s += "]}";
        return s;
    }();
    Check(!ValidateAndParseAgentEvent(tooManyIcons, snapParsed), "reject snapshot json with > 128 icons");
}

void TestSupervisorPolicy()
{
    DesktopAgentSupervisorPolicy policy;
    Check(policy.RecordFailure(1000), "failure 1 schedules restart");
    Check(policy.NextDelayMs() == 500, "failure 1 backoff is 500ms");
    Check(!policy.ShouldRestart(1499), "should not restart before 500ms");
    Check(policy.ShouldRestart(1500), "should restart at 500ms");
    policy.MarkRestartDispatched();
    Check(!policy.ShouldRestart(2000), "dispatched restart clears pending flag");

    Check(policy.RecordFailure(2000), "failure 2 schedules restart");
    Check(policy.NextDelayMs() == 1500, "failure 2 backoff is 1500ms");
    Check(policy.RecordFailure(4000), "failure 3 schedules restart");
    Check(policy.NextDelayMs() == 4000, "failure 3 backoff is 4000ms");

    Check(!policy.RecordFailure(9000), "failure 4 exhausts attempts (degraded mode)");
    Check(policy.exhausted && !policy.ShouldRestart(100000), "exhausted policy does not restart");

    policy.RecordHealthy(100000);
    Check(policy.exhausted, "not yet reset before healthy window");
    policy.RecordHealthy(100000 + DesktopAgentSupervisorPolicy::kHealthyResetMs);
    Check(!policy.exhausted && policy.failures == 0, "healthy period resets failure window");
    Check(policy.RecordFailure(200000), "restart allowed again after healthy reset");
}

void TestLoopbackPipeAndClient()
{
    const std::wstring cmdPipe = UniquePipe(L"Cmd", 1);
    const std::wstring evtPipe = UniquePipe(L"Evt", 1);

    std::atomic_int trayCalls{0};
    std::atomic_int yieldCalls{0};
    std::atomic_int dropCalls{0};
    std::atomic_bool acceptDrop{true};
    std::wstring droppedKey;
    std::vector<std::wstring> droppedPaths;

    DesktopAgentCallbacks cb;
    cb.onTrayChanged = [&] { trayCalls.fetch_add(1); };
    cb.onYieldChanged = [&] { yieldCalls.fetch_add(1); };
    cb.onDragOver = [](POINT pt) {
        AgentDragOverResult res;
        res.allow = (pt.x >= 10 && pt.x <= 100);
        res.key = res.allow ? L"app-target" : L"";
        return res;
    };
    cb.onDropFilesAccepted = [&](const std::wstring& key, const std::vector<std::wstring>& paths) {
        if (!acceptDrop.load())
            return false;
        droppedKey = key;
        droppedPaths = paths;
        dropCalls.fetch_add(1);
        return true;
    };

    DesktopAgentClientOptions opts;
    opts.cmdPipeOverride = cmdPipe;
    opts.evtPipeOverride = evtPipe;
    opts.launchSupervisor = false;
    opts.allowUnboundTestEvtPeer = true;
    DesktopAgentClient client(cb, opts);

    for (int i = 0; i < 200 && !client.EvtServerReady(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(client.EvtServerReady(), "DesktopAgentClient Evt pipe server ready");

    AgentEventMessage snap1;
    snap1.type = AgentEventType::TraySnapshot;
    AgentTrayIconDto icon1;
    icon1.key = L"hwnd:1:1";
    icon1.exe = L"c:\\demo\\qq.exe";
    icon1.pid = 888;
    icon1.imageVersion = 42;
    icon1.flashing = true;
    icon1.width = 2;
    icon1.height = 2;
    icon1.pixels = std::vector<uint8_t>(16, 0x7F);
    snap1.snapshot.icons.push_back(icon1);

    std::string replyRaw;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap1), replyRaw, 2000), "send snap1 over same-user pipe");
    AgentEventReply replyParsed;
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

    AgentEventMessage drag;
    drag.type = AgentEventType::DragOver, drag.pt = POINT{50, 10};
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drag), replyRaw, 2000), "send drag-over");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.allow && replyParsed.key == L"app-target", "drag-over allowed");

    AgentEventMessage drop;
    drop.type = AgentEventType::Drop, drop.opId = 101, drop.key = L"app-target", drop.paths = {L"C:\\file1.txt"};
    acceptDrop.store(false);
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drop), replyRaw, 2000), "send rejected drop");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.ok && !replyParsed.allow && dropCalls.load() == 0,
          "rejected drop returns allow=false");

    acceptDrop.store(true);
    drop.opId = 102;
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

    DesktopAgentClientOptions opts;
    opts.cmdPipeOverride = UniquePipe(L"UnboundCmd", 10);
    opts.evtPipeOverride = evtPipe;
    opts.launchSupervisor = false;
    opts.allowUnboundTestEvtPeer = false;
    DesktopAgentClient client(cb, opts);
    for (int i = 0; i < 200 && !client.EvtServerReady(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(client.EvtServerReady(), "unbound Evt test server ready");

    AgentEventMessage snap;
    snap.type = AgentEventType::TraySnapshot;
    std::string replyRaw;
    Check(!SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap), replyRaw, 1000) && trayCalls.load() == 0,
          "production Evt pipe rejects unbound peer before HandleEvent");
    client.Shutdown();

    const std::wstring peerPipeName = UniquePipe(L"PeerVerify", 11);
    const winrt::handle srv{CreateSameUserNamedPipeServer(peerPipeName)};
    const winrt::handle evt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const winrt::handle quit{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(srv.get() != INVALID_HANDLE_VALUE && evt && quit, "create loopback pipe for VerifyNamedPipePeerProcess");
    std::atomic_bool serverOk{false};
    std::atomic_bool wrongExeRejected{false};
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
    DesktopAgentClientOptions failOpts;
    failOpts.cmdPipeOverride = UniquePipe(L"FailCmd", 2);
    failOpts.evtPipeOverride = UniquePipe(L"FailEvt", 2);
    failOpts.pingWaitMs = 50;
    failOpts.spawnAgentOverride = [&](const std::wstring&, DWORD& errOut) -> HANDLE {
        spawnAttempts.fetch_add(1);
        errOut = ERROR_ACCESS_DENIED;
        return nullptr;
    };
    {
        DesktopAgentClient client({}, failOpts);
        const ULONGLONG deadline = GetTickCount64() + 12000;
        while (!client.Degraded() && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Check(client.Degraded(), "supervisor enters degraded mode after max restarts");
        Check(spawnAttempts.load() == DesktopAgentSupervisorPolicy::kMaxRestartAttempts + 1, "exact 1 initial + 3 restart attempts");
        client.Shutdown();
    }

    const std::wstring cmdPipe = UniquePipe(L"HostCmd", 3);
    const std::wstring evtPipe = UniquePipe(L"HostEvt", 3);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 123456;

    std::atomic<int> hostExitCode{-1};
    std::thread hostThread([&] { hostExitCode.store(RunDesktopAgent(selfPid, gen, GetModuleHandleW(nullptr), cmdPipe.c_str(), evtPipe.c_str())); });

    std::atomic<DWORD> notifiedPid{0};
    DesktopAgentCallbacks hostCb;
    hostCb.onAgentPidChanged = [&](DWORD pid) { notifiedPid.store(pid); };

    DesktopAgentClientOptions liveOpts;
    liveOpts.corePid = selfPid;
    liveOpts.generation = gen;
    liveOpts.cmdPipeOverride = cmdPipe;
    liveOpts.evtPipeOverride = evtPipe;
    liveOpts.pingWaitMs = 3000;
    liveOpts.allowSameProcessPeer = true;
    liveOpts.spawnAgentOverride = [](const std::wstring&, DWORD& errOut) -> HANDLE {
        errOut = 0;
        return nullptr;
    };
    {
        DesktopAgentClient client(hostCb, liveOpts);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!client.Running() && GetTickCount64() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(client.Running() && client.AgentPid() == selfPid && notifiedPid.load() == selfPid, "DesktopAgentClient connected to RunDesktopAgent");

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
    const std::wstring evtPipe = UniquePipe(L"SlowEvt", 20);
    const DWORD selfPid = GetCurrentProcessId();
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int slowStarted{0};
    std::atomic_int slowFinished{0};
    std::atomic_int syncCalled{0};

    DesktopAgentHostOptions hostOpts;
    hostOpts.cmdPipeOverride = cmdPipe;
    hostOpts.evtPipeOverride = evtPipe;
    hostOpts.staTaskTimeoutMs = 50;
    hostOpts.stopEvent = stopEvt.get();
    hostOpts.onBeforeStaCommandForTest = [&](const AgentCommandMessage& cmd) {
        if (cmd.type == AgentCommandType::SetTrayPlaces) {
            slowStarted.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            slowFinished.fetch_add(1);
        } else if (cmd.type == AgentCommandType::SyncDropBars) {
            syncCalled.fetch_add(1);
        }
    };

    std::atomic<int> exitCode{-1};
    std::thread hostThread([&] { exitCode.store(RunDesktopAgentWithOptions(selfPid, 77, GetModuleHandleW(nullptr), hostOpts)); });

    const winrt::handle pipe{ConnectSameUserPipeClient(cmdPipe, 3000)};
    Check(pipe.get() != INVALID_HANDLE_VALUE && pipe, "connected to slow-STA host");
    if (pipe.get() != INVALID_HANDLE_VALUE) {
        std::string rawReply;
        AgentCommandReply reply;
        AgentCommandMessage slowCmd;
        slowCmd.type = AgentCommandType::SetTrayPlaces, slowCmd.anchor = RECT{0, 0, 10, 10};
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(slowCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  !reply.ok && reply.error == L"sta_timeout",
              "slow STA command returned sta_timeout without blocking caller");

        AgentCommandMessage queuedCmd;
        queuedCmd.type = AgentCommandType::SyncDropBars;
        Check(SendPipeRequestOverlapped(pipe.get(), SerializeAgentCommand(queuedCmd), rawReply, 2000) && ValidateAndParseAgentCommandReply(rawReply, reply) &&
                  !reply.ok && reply.error == L"sta_timeout",
              "queued command behind slow STA task timed out and cancelled");

        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        Check(slowStarted.load() == 1 && slowFinished.load() == 1 && syncCalled.load() == 0,
              "running slow STA task completed without UAF and cancelled queued task was skipped");

        AgentCommandMessage pingCmd, quitCmd;
        pingCmd.type = AgentCommandType::Ping, quitCmd.type = AgentCommandType::Quit;
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
    DesktopAgentClientOptions opts;
    opts.cmdPipeOverride = UniquePipe(L"StallEvtCmd", 30);
    opts.evtPipeOverride = evtPipe;
    opts.launchSupervisor = false;
    opts.allowUnboundTestEvtPeer = true;
    DesktopAgentClient client({}, opts);
    for (int i = 0; i < 200 && !client.EvtServerReady(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const winrt::handle evtCli{ConnectSameUserPipeClient(evtPipe, 2000)};
    Check(evtCli.get() != INVALID_HANDLE_VALUE && evtCli, "stalled Evt client connected");
    if (evtCli.get() != INVALID_HANDLE_VALUE) {
        AgentEventMessage snap;
        snap.type = AgentEventType::TraySnapshot;
        const std::string payload = SerializeAgentEvent(snap);
        DWORD written = 0;
        OVERLAPPED ov{};
        const winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        ov.hEvent = ev.get();
        WriteFile(evtCli.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, &ov);
        WaitForSingleObject(ev.get(), 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const ULONGLONG t0 = GetTickCount64();
    client.Shutdown();
    const ULONGLONG evtShutdownMs = GetTickCount64() - t0;
    Check(evtShutdownMs < 500, "Evt server shuts down in < 500ms despite stalled reader");

    const std::wstring cmdPipe = UniquePipe(L"StallCmd", 31);
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    DesktopAgentHostOptions hostOpts;
    hostOpts.cmdPipeOverride = cmdPipe;
    hostOpts.evtPipeOverride = UniquePipe(L"StallHostEvt", 31);
    hostOpts.stopEvent = stopEvt.get();
    std::atomic<int> hostExit{-1};
    std::thread hostThread([&] { hostExit.store(RunDesktopAgentWithOptions(GetCurrentProcessId(), 88, GetModuleHandleW(nullptr), hostOpts)); });

    const winrt::handle cmdCli{ConnectSameUserPipeClient(cmdPipe, 2000)};
    Check(cmdCli.get() != INVALID_HANDLE_VALUE && cmdCli, "stalled Cmd client connected");
    if (cmdCli.get() != INVALID_HANDLE_VALUE) {
        AgentCommandMessage ping;
        ping.type = AgentCommandType::Ping;
        const std::string payload = SerializeAgentCommand(ping);
        DWORD written = 0;
        OVERLAPPED ov{};
        const winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        ov.hEvent = ev.get();
        WriteFile(cmdCli.get(), payload.data(), static_cast<DWORD>(payload.size()), &written, &ov);
        WaitForSingleObject(ev.get(), 1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const ULONGLONG t1 = GetTickCount64();
    SetEvent(stopEvt.get());
    if (hostThread.joinable())
        hostThread.join();
    const ULONGLONG cmdShutdownMs = GetTickCount64() - t1;
    Check(hostExit.load() == 0 && cmdShutdownMs < 500, "Cmd server shuts down in < 500ms despite stalled reader");
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

    const std::wstring cmdPipe = UniquePipe(L"MockCmd", 40);
    const std::wstring evtPipe = UniquePipe(L"MockEvt", 40);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 999;
    const winrt::handle stopMock{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int syncCount{0};
    std::atomic_int sendCount{0};
    std::atomic_bool seenOpId{false};

    std::thread mockCmd([&] {
        const winrt::handle srv{CreateSameUserNamedPipeServer(cmdPipe)};
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stopMock.get()))
            return;
        while (WaitForSingleObject(stopMock.get(), 0) == WAIT_TIMEOUT) {
            std::string raw;
            bool disc = false;
            if (!ReadPipeMessageOverlapped(srv.get(), ov, stopMock.get(), raw, disc))
                break;
            AgentCommandMessage cmd;
            if (!ValidateAndParseAgentCommand(raw, cmd))
                break;
            AgentCommandReply reply;
            if (cmd.type == AgentCommandType::Ping) {
                reply.ok = true, reply.pid = selfPid, reply.generation = gen;
            } else if (cmd.type == AgentCommandType::SyncDropBars) {
                const int n = syncCount.fetch_add(1) + 1;
                reply.ok = (n >= 2);
            } else if (cmd.type == AgentCommandType::TraySend) {
                sendCount.fetch_add(1);
                seenOpId.store(cmd.opId != 0);
                break; // Drop without replying: client must NOT replay TraySend.
            } else if (cmd.type == AgentCommandType::Quit) {
                reply.ok = true;
                WritePipeMessageOverlapped(srv.get(), ov, stopMock.get(), SerializeAgentCommandReply(reply));
                break;
            } else {
                reply.ok = true;
            }
            if (!WritePipeMessageOverlapped(srv.get(), ov, stopMock.get(), SerializeAgentCommandReply(reply)))
                break;
        }
        DisconnectNamedPipe(srv.get());
    });

    DesktopAgentClientOptions opts;
    opts.corePid = selfPid, opts.generation = gen, opts.cmdPipeOverride = cmdPipe, opts.evtPipeOverride = evtPipe;
    opts.allowSameProcessPeer = true, opts.pingWaitMs = 2000;
    opts.spawnAgentOverride = [](const std::wstring&, DWORD& errOut) -> HANDLE { return errOut = 0, nullptr; };
    DesktopAgentClient client({}, opts);
    for (int i = 0; i < 200 && !client.Running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(client.Running(), "mock Cmd server connected");

    const std::vector<AgentBarRect> bars{{1, RECT{0, 0, 100, 40}, true}};
    client.SyncDropBars(bars); // fails (reply.ok = false)
    client.SyncDropBars(bars); // must retry because confirmedDropBars_ != bars
    client.SyncDropBars(bars); // now confirmed, must skip
    Check(syncCount.load() == 2, "SyncDropBars retried after failure and skipped once confirmed");

    const HostSendStatus sendStatus = client.Send(L"icon1", tray::HostAction::Left, POINT{5, 5});
    Check(sendStatus == HostSendStatus::NotSent && sendCount.load() == 1 && seenOpId.load(), "TraySend sets opId and never auto-replays");

    SetEvent(stopMock.get());
    client.Shutdown();
    if (mockCmd.joinable())
        mockCmd.join();
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
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AtriumTestNoRelayTarget";
    RegisterClassExW(&wc);
    HWND targetHwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, &relayCount);
    Check(targetHwnd != nullptr, "created message-only test window");

    const std::wstring cmdPipe = UniquePipe(L"RelayCmd", 50);
    const std::wstring evtPipe = UniquePipe(L"RelayEvt", 50);
    const DWORD selfPid = GetCurrentProcessId();
    const uint64_t gen = 505;
    const winrt::handle stopEvt{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    const winrt::handle ev{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_int traySendObserved{0};

    std::thread mockCmd([&] {
        const winrt::handle srv{CreateSameUserNamedPipeServer(cmdPipe)};
        OVERLAPPED ov{};
        ov.hEvent = ev.get();
        if (srv.get() == INVALID_HANDLE_VALUE || !ConnectNamedPipeOverlapped(srv.get(), ov, stopEvt.get()))
            return;
        while (WaitForSingleObject(stopEvt.get(), 0) == WAIT_TIMEOUT) {
            std::string raw;
            bool disc = false;
            if (!ReadPipeMessageOverlapped(srv.get(), ov, stopEvt.get(), raw, disc))
                break;
            AgentCommandMessage cmd;
            if (!ValidateAndParseAgentCommand(raw, cmd))
                break;
            AgentCommandReply reply;
            reply.ok = true;
            if (cmd.type == AgentCommandType::Ping)
                reply.pid = selfPid, reply.generation = gen;
            else if (cmd.type == AgentCommandType::TraySend) {
                traySendObserved.fetch_add(1);
                reply.sendStatus = HostSendStatus::NotSent;
                reply.higherIntegrity = true;
            }
            WritePipeMessageOverlapped(srv.get(), ov, stopEvt.get(), SerializeAgentCommandReply(reply));
            if (cmd.type == AgentCommandType::Quit)
                break;
        }
        DisconnectNamedPipe(srv.get());
    });

    DesktopAgentClientOptions opts;
    opts.corePid = selfPid, opts.generation = gen, opts.cmdPipeOverride = cmdPipe, opts.evtPipeOverride = evtPipe;
    opts.allowSameProcessPeer = true, opts.allowUnboundTestEvtPeer = true, opts.pingWaitMs = 2000;
    opts.spawnAgentOverride = [](const std::wstring&, DWORD& errOut) -> HANDLE { return errOut = 0, nullptr; };
    DesktopAgentClient client({}, opts);
    for (int i = 0; i < 200 && (!client.Running() || !client.EvtServerReady()); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(client.Running() && client.EvtServerReady(), "relay test client connected");

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
    TrayIcon icon;
    icon.hostKey = L"test:higher";
    const HostSendStatus status = model.SendHosted(icon.hostKey, tray::HostAction::Middle, POINT{5, 5}, RECT{0, 0, 20, 20});

    for (MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);)
        TranslateMessage(&msg), DispatchMessageW(&msg);
    Check(status == HostSendStatus::NotSent && traySendObserved.load() == 1 && relayCount.load() == 0,
          "Core skipped blind High-token SendNotifyMessageW relay for higherIntegrity target");

    SetEvent(stopEvt.get());
    client.Shutdown();
    if (mockCmd.joinable())
        mockCmd.join();
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
    winrt::uninit_apartment();
    return g_failures == 0 ? 0 : 1;
}
