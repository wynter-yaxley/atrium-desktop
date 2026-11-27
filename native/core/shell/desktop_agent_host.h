// Medium-integrity desktop interaction host process entrypoint (`--desktop-agent`).
#pragma once

#include <windows.h>

#include <cstdint>

namespace atrium {

int RunDesktopAgent(DWORD corePid, uint64_t generation, HINSTANCE instance, const wchar_t* cmdPipeOverride = nullptr, const wchar_t* evtPipeOverride = nullptr);

} // namespace atrium
