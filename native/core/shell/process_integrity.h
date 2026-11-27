// Process token integrity level inspection, privilege classification, and least-privilege ACL builders.
#pragma once

#include <windows.h>

#include <string>

namespace atrium {

enum class IntegrityRelation { Lower, Equal, Higher, Unknown };

enum class PrivilegeMode { Standard, ElevatedSplit, ElevatedUnified, CrossAccountElevated, Unknown };

struct ProcessSecurityContext {
    DWORD integrityRid = 0;
    std::wstring userSid;
    LUID logonId{};
    DWORD sessionId = 0;
    bool isElevated = false;
    bool hasUiAccess = false;
    bool isAppContainer = false;
    bool isRestricted = false;
    bool valid = false;
};

struct ShellProcessIdentity {
    DWORD pid = 0;
    FILETIME creationTime{};
    ProcessSecurityContext context;
    bool valid = false;
};

// Exact client rights for same-user named pipes: avoids GENERIC_ALL and FILE_CREATE_PIPE_INSTANCE (0x0004).
inline constexpr DWORD kPipeClientAccessMask = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
inline constexpr DWORD kMutexSameUserAccessMask = MUTEX_ALL_ACCESS;

inline bool IsHigherIntegrityRid(DWORD selfRid, DWORD targetRid) { return selfRid != 0 && targetRid > selfRid; }

inline bool IsElevatedIntegrityRid(DWORD rid) { return rid >= SECURITY_MANDATORY_HIGH_RID; }

// Directional: compares callerRid against targetRid; returns Unknown if either RID is 0.
IntegrityRelation CompareIntegrity(DWORD callerRid, DWORD targetRid);

PrivilegeMode ClassifyPrivilegeMode(const ProcessSecurityContext& self, const ProcessSecurityContext& shell);

bool CanDeElevateToShell(PrivilegeMode mode);

const wchar_t* PrivilegeModeWireName(PrivilegeMode mode);

DWORD ProcessIntegrityRid(HANDLE process, DWORD* errorOut = nullptr);

DWORD SelfIntegrityRid();

bool IsElevatedProcess();

bool WindowHasHigherIntegrity(HWND window);

ProcessSecurityContext QueryProcessSecurityContext(HANDLE process, DWORD* errorOut = nullptr);

ProcessSecurityContext QueryPidSecurityContext(DWORD pid, DWORD* errorOut = nullptr);

const ProcessSecurityContext& SelfSecurityContext();

ShellProcessIdentity ResolveShellProcessIdentity();

PrivilegeMode CurrentPrivilegeMode();

std::wstring BuildSameUserPipeSddl(const std::wstring& userSid, bool includeMediumLabel);

std::wstring BuildSameUserMutexSddl(const std::wstring& userSid, bool includeMediumLabel);

// Owns a Win32 SECURITY_DESCRIPTOR and SECURITY_ATTRIBUTES for kernel objects.
class KernelObjectSecurity {
public:
    KernelObjectSecurity() = default;
    ~KernelObjectSecurity();
    KernelObjectSecurity(const KernelObjectSecurity&) = delete;
    KernelObjectSecurity& operator=(const KernelObjectSecurity&) = delete;
    KernelObjectSecurity(KernelObjectSecurity&& other) noexcept;
    KernelObjectSecurity& operator=(KernelObjectSecurity&& other) noexcept;

    static KernelObjectSecurity CreateSameUserPipe();
    static KernelObjectSecurity CreateSameUserMutex();
    static KernelObjectSecurity FromSddl(const std::wstring& sddl);

    bool Valid() const { return sd_ != nullptr; }
    bool ShouldFailClosed() const { return !Valid() && IsElevatedProcess(); }
    SECURITY_ATTRIBUTES* Get() { return sd_ ? &sa_ : nullptr; }

private:
    explicit KernelObjectSecurity(PSECURITY_DESCRIPTOR sd);

    PSECURITY_DESCRIPTOR sd_ = nullptr;
    SECURITY_ATTRIBUTES sa_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};

} // namespace atrium
