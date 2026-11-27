// Medium-integrity desktop interaction agent IPC protocol, bounded validation, same-user pipe helpers, and restart policy.
#pragma once

#include "shell/process_integrity.h"
#include "shell/tray_host.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

inline constexpr size_t kMaxAgentMessageBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxAgentTrayIcons = 128;
inline constexpr int kMaxAgentIconDimension = 256;
inline constexpr size_t kMaxAgentDropPaths = 64;
inline constexpr size_t kMaxAgentPathLength = 4096;
inline constexpr size_t kMaxAgentDropBars = 64;

std::wstring DesktopAgentCommandPipeName(DWORD sessionId, DWORD corePid, uint64_t generation);
std::wstring DesktopAgentEventPipeName(DWORD sessionId, DWORD corePid, uint64_t generation);
DWORD CurrentProcessSessionId();

struct AgentBarRect {
    uint64_t id = 0;
    RECT rect{};
    bool visible = false;
};

inline bool operator==(const AgentBarRect& a, const AgentBarRect& b) { return a.id == b.id && a.visible == b.visible && EqualRect(&a.rect, &b.rect) != FALSE; }

struct AgentDragOverResult {
    bool allow = false;
    std::wstring key;
};

struct AgentTrayIconDto {
    std::wstring key;
    uint64_t hwnd = 0;
    UINT uid = 0;
    UINT callback = 0;
    UINT version = 0;
    bool showTip = false;
    std::wstring tip;
    std::wstring exe;
    DWORD pid = 0;
    uint64_t imageVersion = 0;
    bool hidden = false;
    uint64_t added = 0;
    bool flashing = false;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

struct AgentTraySnapshot {
    std::vector<AgentTrayIconDto> icons;
    bool yielded = false;
    TrayHostYieldReason yieldReason = TrayHostYieldReason::None;
};

enum class AgentCommandType { Ping, SetTrayHosting, SetTrayPlaces, TraySend, TrayHover, SyncDropBars, Quit };

struct AgentCommandMessage {
    AgentCommandType type = AgentCommandType::Ping;
    bool hosting = false;
    RECT anchor{};
    std::wstring key;
    tray::HostAction action = tray::HostAction::Left;
    POINT at{};
    RECT cell{};
    std::vector<AgentBarRect> bars;
};

struct AgentCommandReply {
    bool ok = false;
    DWORD pid = 0;
    uint64_t generation = 0;
    HostSendStatus sendStatus = HostSendStatus::NotSent;
    bool higherIntegrity = false;
    std::wstring error;
};

enum class AgentEventType { TraySnapshot, TrayYield, DragOver, DragLeave, Drop };

struct AgentEventMessage {
    AgentEventType type = AgentEventType::TraySnapshot;
    AgentTraySnapshot snapshot;
    bool yielded = false;
    TrayHostYieldReason yieldReason = TrayHostYieldReason::None;
    POINT pt{};
    std::wstring key;
    std::vector<std::wstring> paths;
};

struct AgentEventReply {
    bool ok = false;
    bool allow = false;
    std::wstring key;
    std::wstring error;
};

bool IsValidAgentTrayIconDto(const AgentTrayIconDto& icon);
bool IsValidAgentDropPayload(const std::wstring& key, const std::vector<std::wstring>& paths);

std::string SerializeAgentCommand(const AgentCommandMessage& msg);
bool ValidateAndParseAgentCommand(const std::string& raw, AgentCommandMessage& out);
std::string SerializeAgentCommandReply(const AgentCommandReply& reply);
bool ValidateAndParseAgentCommandReply(const std::string& raw, AgentCommandReply& out);

std::string SerializeAgentEvent(const AgentEventMessage& msg);
bool ValidateAndParseAgentEvent(const std::string& raw, AgentEventMessage& out);
std::string SerializeAgentEventReply(const AgentEventReply& reply);
bool ValidateAndParseAgentEventReply(const std::string& raw, AgentEventReply& out);

// Pure restart & degraded-mode supervisor policy for the Medium desktop agent.
struct DesktopAgentSupervisorPolicy {
    static constexpr int kMaxRestartAttempts = 3;
    static constexpr ULONGLONG kHealthyResetMs = 30000;
    static constexpr std::array<ULONGLONG, kMaxRestartAttempts> kBackoffMs{500, 1500, 4000};

    int failures = 0;
    bool exhausted = false;
    bool restartScheduled = false;
    ULONGLONG lastFailureAt = 0;
    ULONGLONG healthySince = 0;
    ULONGLONG nextRetryAt = 0;

    void Reset()
    {
        failures = 0;
        exhausted = false;
        restartScheduled = false;
        lastFailureAt = 0;
        healthySince = 0;
        nextRetryAt = 0;
    }

    void RecordHealthy(ULONGLONG nowMs)
    {
        if (healthySince == 0)
            healthySince = nowMs;
        if (nowMs >= healthySince && nowMs - healthySince >= kHealthyResetMs) {
            failures = 0;
            exhausted = false;
            restartScheduled = false;
        }
    }

    // Returns true if a restart should be scheduled, false when attempts are exhausted (degraded mode).
    bool RecordFailure(ULONGLONG nowMs)
    {
        healthySince = 0;
        lastFailureAt = nowMs;
        if (exhausted || failures >= kMaxRestartAttempts) {
            exhausted = true;
            restartScheduled = false;
            return false;
        }
        const ULONGLONG delay = kBackoffMs[static_cast<size_t>(failures)];
        ++failures;
        nextRetryAt = nowMs + delay;
        restartScheduled = true;
        return true;
    }

    void MarkRestartDispatched() { restartScheduled = false; }

    bool ShouldRestart(ULONGLONG nowMs) const { return !exhausted && restartScheduled && nowMs >= nextRetryAt; }

    ULONGLONG NextDelayMs() const
    {
        if (exhausted)
            return 0;
        const size_t idx = failures > 0 ? static_cast<size_t>(failures - 1) : 0;
        return kBackoffMs[idx < kMaxRestartAttempts ? idx : static_cast<size_t>(kMaxRestartAttempts - 1)];
    }
};

HANDLE CreateSameUserNamedPipeServer(const std::wstring& pipeName, DWORD bufferSize = 64 * 1024);
bool VerifyNamedPipeClientSameUser(HANDLE pipe);
bool ConnectNamedPipeOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent);
bool ReadPipeMessageTimed(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, DWORD timeoutMs, std::string& messageOut, bool& disconnectedOut);
inline bool ReadPipeMessageOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, std::string& messageOut, bool& disconnectedOut)
{
    return ReadPipeMessageTimed(pipe, ov, quitEvent, INFINITE, messageOut, disconnectedOut);
}
bool WritePipeMessageOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, const std::string& message);
HANDLE ConnectSameUserPipeClient(const std::wstring& pipeName, DWORD timeoutMs, HANDLE quitEvent = nullptr);
bool SendPipeRequestOverlapped(HANDLE pipe, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent = nullptr);
bool SendNamedPipeOneShot(const std::wstring& pipeName, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent = nullptr);

} // namespace atrium
