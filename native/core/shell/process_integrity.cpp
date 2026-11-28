#include "shell/process_integrity.h"

#include <sddl.h>

#include <mutex>
#include <utility>
#include <vector>

namespace atrium {

namespace {

struct ScopedToken {
    HANDLE handle = nullptr;
    ~ScopedToken()
    {
        if (handle)
            CloseHandle(handle);
    }
    ScopedToken() = default;
    ScopedToken(const ScopedToken&) = delete;
    ScopedToken& operator=(const ScopedToken&) = delete;
};

bool QueryTokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS infoClass, std::vector<BYTE>& buffer)
{
    DWORD size = 0;
    GetTokenInformation(token, infoClass, nullptr, 0, &size);
    if (!size)
        return false;
    buffer.resize(size);
    return GetTokenInformation(token, infoClass, buffer.data(), size, &size) != FALSE;
}

template<class T> bool QueryTokenPod(HANDLE token, TOKEN_INFORMATION_CLASS infoClass, T& out)
{
    DWORD size = sizeof(T);
    return GetTokenInformation(token, infoClass, &out, size, &size) != FALSE && size == sizeof(T);
}

DWORD ReadTokenIntegrityRid(HANDLE token, DWORD* errorOut)
{
    std::vector<BYTE> buffer;
    if (!QueryTokenBuffer(token, TokenIntegrityLevel, buffer)) {
        if (errorOut)
            *errorOut = GetLastError();
        return 0;
    }
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0)
        return 0;
    return *GetSidSubAuthority(label->Label.Sid, DWORD(count - 1));
}

std::wstring ReadTokenUserSid(HANDLE token)
{
    std::vector<BYTE> buffer;
    if (!QueryTokenBuffer(token, TokenUser, buffer))
        return {};
    wchar_t* sidText = nullptr;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!ConvertSidToStringSidW(user->User.Sid, &sidText) || !sidText)
        return {};
    std::wstring result(sidText);
    LocalFree(sidText);
    return result;
}

bool SameCreationTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::mutex g_shellCacheMutex;
ShellProcessIdentity g_shellCache;

} // namespace

IntegrityRelation CompareIntegrity(DWORD callerRid, DWORD targetRid)
{
    if (callerRid == 0 || targetRid == 0)
        return IntegrityRelation::Unknown;
    if (callerRid < targetRid)
        return IntegrityRelation::Lower;
    if (callerRid == targetRid)
        return IntegrityRelation::Equal;
    return IntegrityRelation::Higher;
}

PrivilegeMode ClassifyPrivilegeMode(const ProcessSecurityContext& self, const ProcessSecurityContext& shell)
{
    if (!self.valid || !shell.valid)
        return PrivilegeMode::Unknown;
    const bool selfElevated = self.isElevated || IsElevatedIntegrityRid(self.integrityRid);
    const bool shellElevated = shell.isElevated || IsElevatedIntegrityRid(shell.integrityRid);
    const bool sameUser = !self.userSid.empty() && _wcsicmp(self.userSid.c_str(), shell.userSid.c_str()) == 0;
    if (!selfElevated)
        return sameUser ? PrivilegeMode::Standard : PrivilegeMode::Unknown;
    if (!sameUser)
        return PrivilegeMode::CrossAccountElevated;
    return shellElevated ? PrivilegeMode::ElevatedUnified : PrivilegeMode::ElevatedSplit;
}

bool CanDeElevateToShell(PrivilegeMode mode) { return mode == PrivilegeMode::ElevatedSplit; }

const wchar_t* PrivilegeModeWireName(PrivilegeMode mode)
{
    switch (mode) {
    case PrivilegeMode::Standard: return L"standard";
    case PrivilegeMode::ElevatedSplit: return L"elevated_split";
    case PrivilegeMode::ElevatedUnified: return L"elevated_unified";
    case PrivilegeMode::CrossAccountElevated: return L"cross_account_elevated";
    case PrivilegeMode::Unknown: return L"unknown";
    }
    return L"unknown";
}

DWORD ProcessIntegrityRid(HANDLE process, DWORD* errorOut)
{
    if (errorOut)
        *errorOut = 0;
    if (!process) {
        if (errorOut)
            *errorOut = ERROR_INVALID_PARAMETER;
        return 0;
    }
    ScopedToken token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.handle)) {
        if (errorOut)
            *errorOut = GetLastError();
        return 0;
    }
    return ReadTokenIntegrityRid(token.handle, errorOut);
}

DWORD SelfIntegrityRid()
{
    static const DWORD kSelf = ProcessIntegrityRid(GetCurrentProcess());
    return kSelf;
}

bool IsElevatedProcess() { return IsElevatedIntegrityRid(SelfIntegrityRid()); }

bool WindowHasHigherIntegrity(HWND window)
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

ProcessSecurityContext QueryProcessSecurityContext(HANDLE process, DWORD* errorOut)
{
    if (errorOut)
        *errorOut = 0;
    ProcessSecurityContext ctx;
    if (!process) {
        if (errorOut)
            *errorOut = ERROR_INVALID_PARAMETER;
        return ctx;
    }
    ScopedToken token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.handle)) {
        if (errorOut)
            *errorOut = GetLastError();
        return ctx;
    }
    ctx.integrityRid = ReadTokenIntegrityRid(token.handle, errorOut);
    ctx.userSid = ReadTokenUserSid(token.handle);
    TOKEN_STATISTICS stats{};
    if (QueryTokenPod(token.handle, TokenStatistics, stats))
        ctx.logonId = stats.AuthenticationId;
    DWORD sessionId = 0;
    if (QueryTokenPod(token.handle, TokenSessionId, sessionId))
        ctx.sessionId = sessionId;
    TOKEN_ELEVATION elevation{};
    if (QueryTokenPod(token.handle, TokenElevation, elevation))
        ctx.isElevated = elevation.TokenIsElevated != 0;
    DWORD uiAccess = 0;
    if (QueryTokenPod(token.handle, TokenUIAccess, uiAccess))
        ctx.hasUiAccess = uiAccess != 0;
    DWORD appContainer = 0;
    if (QueryTokenPod(token.handle, TokenIsAppContainer, appContainer))
        ctx.isAppContainer = appContainer != 0;
    ctx.isRestricted = IsTokenRestricted(token.handle) != FALSE;
    ctx.valid = ctx.integrityRid != 0 && !ctx.userSid.empty();
    return ctx;
}

ProcessSecurityContext QueryPidSecurityContext(DWORD pid, DWORD* errorOut)
{
    if (errorOut)
        *errorOut = 0;
    if (!pid) {
        if (errorOut)
            *errorOut = ERROR_INVALID_PARAMETER;
        return {};
    }
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        if (errorOut)
            *errorOut = GetLastError();
        return {};
    }
    ProcessSecurityContext ctx = QueryProcessSecurityContext(proc, errorOut);
    CloseHandle(proc);
    return ctx;
}

const ProcessSecurityContext& SelfSecurityContext()
{
    static const ProcessSecurityContext kSelf = QueryProcessSecurityContext(GetCurrentProcess());
    return kSelf;
}

ShellProcessIdentity ResolveShellProcessIdentity()
{
    HWND shellWnd = GetShellWindow();
    if (!shellWnd)
        return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(shellWnd, &pid);
    if (!pid)
        return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return {};
    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    const BOOL gotTimes = GetProcessTimes(proc, &creation, &exitTime, &kernelTime, &userTime);
    if (!gotTimes) {
        CloseHandle(proc);
        return {};
    }
    {
        std::lock_guard lock(g_shellCacheMutex);
        if (g_shellCache.valid && g_shellCache.pid == pid && SameCreationTime(g_shellCache.creationTime, creation)) {
            CloseHandle(proc);
            return g_shellCache;
        }
    }
    ShellProcessIdentity resolved;
    resolved.pid = pid;
    resolved.creationTime = creation;
    resolved.context = QueryProcessSecurityContext(proc);
    resolved.valid = resolved.context.valid;
    CloseHandle(proc);
    if (resolved.valid) {
        std::lock_guard lock(g_shellCacheMutex);
        g_shellCache = resolved;
    }
    return resolved;
}

PrivilegeMode CurrentPrivilegeMode() { return ClassifyPrivilegeMode(SelfSecurityContext(), ResolveShellProcessIdentity().context); }

std::wstring BuildSameUserPipeSddl(const std::wstring& userSid, bool includeMediumLabel)
{
    if (userSid.empty())
        return {};
    static_assert((kPipeClientAccessMask & GENERIC_ALL) == 0);
    static_assert((kPipeClientAccessMask & FILE_CREATE_PIPE_INSTANCE) == 0);
    std::wstring sddl = L"O:" + userSid + L"D:P(A;;0x0012019B;;;" + userSid + L")";
    if (includeMediumLabel)
        sddl += L"S:(ML;;NW;;;ME)";
    return sddl;
}

std::wstring BuildSameUserMutexSddl(const std::wstring& userSid, bool includeMediumLabel)
{
    if (userSid.empty())
        return {};
    static_assert((kMutexSameUserAccessMask & GENERIC_ALL) == 0);
    std::wstring sddl = L"O:" + userSid + L"D:P(A;;0x001F0001;;;" + userSid + L")";
    if (includeMediumLabel)
        sddl += L"S:(ML;;NW;;;ME)";
    return sddl;
}

KernelObjectSecurity::KernelObjectSecurity(PSECURITY_DESCRIPTOR sd) : sd_(sd) { sa_.lpSecurityDescriptor = sd_; }

KernelObjectSecurity::~KernelObjectSecurity()
{
    if (sd_)
        LocalFree(sd_);
}

KernelObjectSecurity::KernelObjectSecurity(KernelObjectSecurity&& other) noexcept : sd_(std::exchange(other.sd_, nullptr))
{
    sa_.lpSecurityDescriptor = sd_;
    other.sa_.lpSecurityDescriptor = nullptr;
}

KernelObjectSecurity& KernelObjectSecurity::operator=(KernelObjectSecurity&& other) noexcept
{
    if (this == &other)
        return *this;
    if (sd_)
        LocalFree(sd_);
    sd_ = std::exchange(other.sd_, nullptr);
    sa_.lpSecurityDescriptor = sd_;
    other.sa_.lpSecurityDescriptor = nullptr;
    return *this;
}

KernelObjectSecurity KernelObjectSecurity::FromSddl(const std::wstring& sddl)
{
    if (sddl.empty())
        return {};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) || !sd)
        return {};
    return KernelObjectSecurity(sd);
}

KernelObjectSecurity KernelObjectSecurity::CreateSameUserPipe()
{
    const auto& self = SelfSecurityContext();
    if (!self.valid)
        return {};
    return FromSddl(BuildSameUserPipeSddl(self.userSid, self.integrityRid >= SECURITY_MANDATORY_MEDIUM_RID));
}

KernelObjectSecurity KernelObjectSecurity::CreateSameUserMutex()
{
    const auto& self = SelfSecurityContext();
    if (!self.valid)
        return {};
    return FromSddl(BuildSameUserMutexSddl(self.userSid, self.integrityRid >= SECURITY_MANDATORY_MEDIUM_RID));
}

} // namespace atrium
