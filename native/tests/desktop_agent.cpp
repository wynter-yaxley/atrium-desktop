#include "shell/desktop_agent_client.h"
#include "shell/desktop_agent_host.h"
#include "shell/desktop_agent_ipc.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>

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
    cmd.key = L"guid:{1234}";
    cmd.action = tray::HostAction::Double;
    cmd.at = POINT{120, 400};
    cmd.cell = RECT{100, 380, 140, 420};

    AgentCommandMessage parsed;
    Check(ValidateAndParseAgentCommand(SerializeAgentCommand(cmd), parsed), "parse valid tray-send");
    Check(parsed.type == AgentCommandType::TraySend && parsed.key == cmd.key && parsed.action == cmd.action, "tray-send fields match");
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
    drop.key = L"app-1";
    drop.paths = {L"C:\\test\\a.txt", L"C:\\test\\b.txt"};
    AgentEventMessage dropParsed;
    Check(ValidateAndParseAgentEvent(SerializeAgentEvent(drop), dropParsed), "parse valid drop");
    Check(dropParsed.key == drop.key && dropParsed.paths == drop.paths, "drop payload round-trip");

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
    cb.onDropFiles = [&](const std::wstring& key, const std::vector<std::wstring>& paths) {
        droppedKey = key;
        droppedPaths = paths;
        dropCalls.fetch_add(1);
    };

    DesktopAgentClientOptions opts;
    opts.cmdPipeOverride = cmdPipe;
    opts.evtPipeOverride = evtPipe;
    opts.launchSupervisor = false;
    DesktopAgentClient client(cb, opts);

    for (int i = 0; i < 200 && !client.EvtServerReady(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(client.EvtServerReady(), "DesktopAgentClient Evt pipe server ready");

    // Push TraySnapshot with bitmap, then second TraySnapshot omitting cached bitmap by imageVersion.
    AgentEventMessage snap1;
    snap1.type = AgentEventType::TraySnapshot;
    snap1.snapshot.yielded = false;
    snap1.snapshot.yieldReason = TrayHostYieldReason::None;
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
    snap2.snapshot.icons[0].width = 0;
    snap2.snapshot.icons[0].height = 0;
    snap2.snapshot.icons[0].pixels.clear();
    snap2.snapshot.yielded = true;
    snap2.snapshot.yieldReason = TrayHostYieldReason::OtherProcess;
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(snap2), replyRaw, 2000), "send snap2 with cached imageVersion");
    Check(client.Icons().size() == 1 && client.Icons()[0].image && client.Icons()[0].image->ByteSize() == 16, "reused cached image by version");
    Check(client.Yielded() && client.YieldReason() == TrayHostYieldReason::OtherProcess, "yield state updated");
    Check(trayCalls.load() >= 2 && yieldCalls.load() >= 1, "tray and yield callbacks fired");

    // Drag-over & drop round-trip.
    AgentEventMessage drag;
    drag.type = AgentEventType::DragOver;
    drag.pt = POINT{50, 10};
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drag), replyRaw, 2000), "send drag-over");
    Check(ValidateAndParseAgentEventReply(replyRaw, replyParsed) && replyParsed.allow && replyParsed.key == L"app-target", "drag-over allowed");

    AgentEventMessage drop;
    drop.type = AgentEventType::Drop;
    drop.key = L"app-target";
    drop.paths = {L"C:\\file1.txt"};
    Check(SendNamedPipeOneShot(evtPipe, SerializeAgentEvent(drop), replyRaw, 2000), "send drop");
    Check(dropCalls.load() == 1 && droppedKey == L"app-target" && droppedPaths.size() == 1, "drop callback received files");

    client.Shutdown();
}

void TestSupervisorExhaustionAndHostRoundTrip()
{
    // 1. Supervisor exhaustion using failing spawn override (never touches Atrium.exe).
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

    // 2. Real RunDesktopAgent host + DesktopAgentClient Cmd/Evt round-trip in-process.
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

} // namespace

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestPipeNames();
    TestCommandSerialization();
    TestEventSerializationAndBounds();
    TestSupervisorPolicy();
    TestLoopbackPipeAndClient();
    TestSupervisorExhaustionAndHostRoundTrip();
    winrt::uninit_apartment();
    return g_failures == 0 ? 0 : 1;
}
