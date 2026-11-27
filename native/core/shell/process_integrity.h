// Process token integrity level inspection for UIPI boundary handling across Stage Manager,
// window snapshots, Settings IPC, and privilege-elevation detection.
#pragma once

#include <windows.h>

#include <vector>

namespace atrium {

inline bool IsHigherIntegrityRid(DWORD selfRid, DWORD targetRid) { return selfRid != 0 && targetRid > selfRid; }

inline bool IsElevatedIntegrityRid(DWORD rid) { return rid >= SECURITY_MANDATORY_HIGH_RID; }

inline DWORD ProcessIntegrityRid(HANDLE process, DWORD* errorOut = nullptr)
{
    if (errorOut)
        *errorOut = 0;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        if (errorOut)
            *errorOut = GetLastError();
        return 0;
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    DWORD rid = 0;
    if (size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
        const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
        if (count > 0)
            rid = *GetSidSubAuthority(label->Label.Sid, DWORD(count - 1));
    } else if (errorOut) {
        *errorOut = GetLastError();
    }
    CloseHandle(token);
    return rid;
}

inline DWORD SelfIntegrityRid()
{
    static const DWORD kSelf = ProcessIntegrityRid(GetCurrentProcess());
    return kSelf;
}

inline bool IsElevatedProcess() { return IsElevatedIntegrityRid(SelfIntegrityRid()); }

inline bool WindowHasHigherIntegrity(HWND window)
{
    if (!window)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD err = 0;
    const DWORD targetRid = ProcessIntegrityRid(proc, &err);
    CloseHandle(proc);
    if (targetRid == 0)
        return err == ERROR_ACCESS_DENIED;
    return IsHigherIntegrityRid(SelfIntegrityRid(), targetRid);
}

} // namespace atrium
