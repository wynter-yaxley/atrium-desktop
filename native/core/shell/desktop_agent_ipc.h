// Medium-integrity desktop interaction agent IPC protocol, bounded validation, same-user pipe helpers, and restart policy.
#pragma once

#include "shell/process_integrity.h"
#include "shell/tray_host.h"

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

namespace atrium {

inline constexpr size_t kMaxAgentMessageBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxAgentTrayIcons = 128;
inline constexpr int kMaxAgentIconDimension = 256;
inline constexpr size_t kMaxAgentDropPaths = 64;
inline constexpr size_t kMaxAgentPathLength = 4096;
inline constexpr size_t kMaxAgentDropBars = 64;

inline std::wstring DesktopAgentCommandPipeName(DWORD sessionId, DWORD corePid, uint64_t generation)
{
    return L"\\\\.\\pipe\\Atrium.DesktopAgent.Cmd." + std::to_wstring(sessionId) + L"." + std::to_wstring(corePid) + L"." + std::to_wstring(generation);
}

inline std::wstring DesktopAgentEventPipeName(DWORD sessionId, DWORD corePid, uint64_t generation)
{
    return L"\\\\.\\pipe\\Atrium.DesktopAgent.Evt." + std::to_wstring(sessionId) + L"." + std::to_wstring(corePid) + L"." + std::to_wstring(generation);
}

inline DWORD CurrentProcessSessionId()
{
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

inline std::wstring GenerateAgentSpawnNonce()
{
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::array<uint8_t, 16> bytes{};
    GUID g{};
    if (SUCCEEDED(CoCreateGuid(&g))) {
        std::memcpy(bytes.data(), &g, bytes.size());
    } else {
        static std::atomic<uint64_t> fallbackCounter{1};
        const uint64_t hi = GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
        const uint64_t lo = fallbackCounter.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL;
        std::memcpy(bytes.data(), &hi, 8);
        std::memcpy(bytes.data() + 8, &lo, 8);
    }
    std::wstring out;
    out.reserve(32);
    for (uint8_t b : bytes) {
        out.push_back(kHex[(b >> 4) & 0xF]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

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
    uint64_t opId = 0;
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
    std::wstring nonce;
};

enum class AgentEventType { TraySnapshot, TrayYield, DragOver, DragLeave, Drop };

struct AgentEventMessage {
    AgentEventType type = AgentEventType::TraySnapshot;
    uint64_t opId = 0;
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

inline bool IsValidAgentTrayIconDto(const AgentTrayIconDto& icon)
{
    if (icon.key.empty() || icon.key.size() > kMaxAgentPathLength || icon.tip.size() > kMaxAgentPathLength || icon.exe.size() > kMaxAgentPathLength)
        return false;
    if (icon.width == 0 && icon.height == 0)
        return icon.pixels.empty();
    if (icon.width <= 0 || icon.height <= 0 || icon.width > kMaxAgentIconDimension || icon.height > kMaxAgentIconDimension)
        return false;
    return icon.pixels.size() == static_cast<size_t>(icon.width) * static_cast<size_t>(icon.height) * 4;
}

inline bool IsValidAgentDropPayload(const std::wstring& key, const std::vector<std::wstring>& paths)
{
    if (key.empty() || key.size() > kMaxAgentPathLength || paths.empty() || paths.size() > kMaxAgentDropPaths)
        return false;
    return std::ranges::all_of(paths, [](const std::wstring& p) { return !p.empty() && p.size() <= kMaxAgentPathLength; });
}

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

struct AgentPeerIdentity {
    DWORD pid = 0;
    FILETIME creationTime{};
    std::wstring imagePath;
    ProcessSecurityContext security;
    bool valid = false;
};

class RecentOpTracker {
public:
    static constexpr size_t kMaxSeenOps = 64;

    bool MarkSeenIfNew(uint64_t opId)
    {
        if (opId == 0)
            return true;
        if (seen_.contains(opId))
            return false;
        if (order_.size() >= kMaxSeenOps) {
            seen_.erase(order_.front());
            order_.pop_front();
        }
        order_.push_back(opId);
        seen_.insert(opId);
        return true;
    }

    void Clear()
    {
        order_.clear();
        seen_.clear();
    }

private:
    std::deque<uint64_t> order_;
    std::unordered_set<uint64_t> seen_;
};

class TraySnapshotAckState {
public:
    void Reset()
    {
        confirmedSession_ = 0;
        confirmedVersions_.clear();
    }

    bool IsConfirmed(uint64_t evtSession, uint64_t imageVersion)
    {
        if (confirmedSession_ != evtSession) {
            confirmedSession_ = evtSession;
            confirmedVersions_.clear();
        }
        return imageVersion != 0 && confirmedVersions_.contains(imageVersion);
    }

    void RecordAck(uint64_t evtSession, const AgentTraySnapshot& snapshot, bool ackOk)
    {
        if (!ackOk)
            return;
        if (confirmedSession_ != evtSession) {
            confirmedSession_ = evtSession;
            confirmedVersions_.clear();
        }
        std::unordered_set<uint64_t> next;
        for (const auto& icon : snapshot.icons) {
            if (icon.imageVersion != 0 && (!icon.pixels.empty() || confirmedVersions_.contains(icon.imageVersion)))
                next.insert(icon.imageVersion);
        }
        confirmedVersions_ = std::move(next);
    }

    bool HasUnconfirmedWithEmptyPixels(uint64_t evtSession, const AgentTraySnapshot& snapshot)
    {
        return std::ranges::any_of(snapshot.icons, [&](const AgentTrayIconDto& icon) {
            return icon.imageVersion != 0 && icon.pixels.empty() && !IsConfirmed(evtSession, icon.imageVersion);
        });
    }

private:
    uint64_t confirmedSession_ = 0;
    std::unordered_set<uint64_t> confirmedVersions_;
};

inline AgentTrayIconDto BuildAgentTrayIconDto(const HostIcon& src, ULONGLONG nowMs, uint64_t evtSession, size_t rawPixelBudget, size_t& pixelBudgetUsed,
                                              TraySnapshotAckState& ackState, bool& deferredBitmapOut)
{
    AgentTrayIconDto dto;
    dto.key = src.key, dto.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(src.hwnd));
    dto.uid = src.uid, dto.callback = src.callback, dto.version = src.version, dto.showTip = src.showTip;
    dto.tip = src.tip, dto.exe = src.exe, dto.pid = src.pid, dto.imageVersion = src.imageVersion;
    dto.hidden = src.hidden, dto.added = src.added, dto.flashing = src.flash.until > nowMs;
    if (src.image && !src.image->Empty() && src.imageVersion != 0) {
        const size_t byteSize = src.image->ByteSize();
        const bool dimsOk = src.image->width > 0 && src.image->height > 0 && src.image->width <= kMaxAgentIconDimension &&
                            src.image->height <= kMaxAgentIconDimension &&
                            byteSize == static_cast<size_t>(src.image->width) * static_cast<size_t>(src.image->height) * 4;
        if (!dimsOk) {
            dto.imageVersion = 0;
        } else if (!ackState.IsConfirmed(evtSession, src.imageVersion)) {
            if (pixelBudgetUsed + byteSize <= rawPixelBudget) {
                dto.width = src.image->width, dto.height = src.image->height, dto.pixels = src.image->Bytes();
                pixelBudgetUsed += byteSize;
            } else {
                deferredBitmapOut = true;
            }
        }
    } else {
        dto.imageVersion = 0;
    }
    return dto;
}

HANDLE CreateSameUserNamedPipeServer(const std::wstring& pipeName, DWORD bufferSize = 64 * 1024);
bool VerifyNamedPipeClientSameUser(HANDLE pipe);
bool VerifyNamedPipePeerProcess(HANDLE pipe, bool isServerSide, const std::wstring& expectedImagePath, bool requireNonElevated, AgentPeerIdentity& out,
                                winrt::handle* procOut = nullptr);
inline bool VerifyLaunchedProcessMatchesPeer(HANDLE launchedProc, const AgentPeerIdentity& peer)
{
    if (!launchedProc || launchedProc == INVALID_HANDLE_VALUE || !peer.valid || peer.pid == 0)
        return false;
    const DWORD launchedPid = GetProcessId(launchedProc);
    FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
    return launchedPid != 0 && peer.pid == launchedPid && GetProcessTimes(launchedProc, &creation, &exitTime, &kernelTime, &userTime) &&
           CompareFileTime(&peer.creationTime, &creation) == 0;
}
bool ConnectNamedPipeOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent);
bool ReadPipeMessageTimed(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, DWORD timeoutMs, std::string& messageOut, bool& disconnectedOut);
inline bool ReadPipeMessageOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, std::string& messageOut, bool& disconnectedOut)
{
    return ReadPipeMessageTimed(pipe, ov, quitEvent, INFINITE, messageOut, disconnectedOut);
}
bool WritePipeMessageOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, const std::string& message);
HANDLE ConnectSameUserPipeClient(const std::wstring& pipeName, DWORD timeoutMs, HANDLE quitEvent = nullptr);
bool SendPipeRequestOverlapped(HANDLE pipe, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent = nullptr,
                               bool* writeCompletedOut = nullptr);
bool SendNamedPipeOneShot(const std::wstring& pipeName, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent = nullptr);

} // namespace atrium
