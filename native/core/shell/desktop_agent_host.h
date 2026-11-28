// Medium-integrity desktop interaction host process entrypoint (`--desktop-agent`).
#pragma once

#include "shell/desktop_agent_ipc.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace atrium {

struct DesktopAgentHostOptions {
    std::wstring cmdPipeOverride;
    std::wstring evtPipeOverride;
    DWORD staTaskTimeoutMs = 5000;
    HANDLE stopEvent = nullptr;
    std::function<void(const AgentCommandMessage&)> onBeforeStaCommandForTest;
    bool failOverlaySyncForTest = false;
    std::function<void(std::function<void()> pushSnapshot, std::function<void()> pushYield)> onReadyForTest;
    std::wstring nonce;
    bool failOverlayShowForTest = false;
    int failSnapshotPostCountForTest = 0;
    std::function<void(RECT)> onAnchorAppliedForTest;
};

int RunDesktopAgent(DWORD corePid, uint64_t generation, HINSTANCE instance, const wchar_t* cmdPipeOverride = nullptr, const wchar_t* evtPipeOverride = nullptr,
                    const wchar_t* nonceOverride = nullptr);
int RunDesktopAgentWithOptions(DWORD corePid, uint64_t generation, HINSTANCE instance, const DesktopAgentHostOptions& options);

} // namespace atrium
