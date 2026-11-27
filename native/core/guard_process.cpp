#include "guard_process.h"

#include "log.h"
#include "settings.h"
#include "shell/process_integrity.h"
#include "shell/shell_taskbar.h"
#include "shell/system_chrome.h"

#include <objbase.h>

#include <atomic>
#include <cerrno>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace atrium {

namespace {

constexpr wchar_t kGuardMutex[] = L"Local\\Atrium.Guard"; // held by the guard while it runs
std::atomic<uint64_t> g_activeGeneration{0};

HWND FindProcessWindow(const wchar_t* className, DWORD pid)
{
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, className, nullptr)) != nullptr;) {
        DWORD owner = 0;
        if (GetWindowThreadProcessId(w, &owner); owner == pid)
            return w;
    }
    return nullptr;
}

uint64_t ParseGenerationText(std::wstring_view text)
{
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t' || text.front() == L'\r' || text.front() == L'\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r' || text.back() == L'\n'))
        text.remove_suffix(1);
    if (text.empty())
        return 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9')
            return 0;
    }
    const std::wstring copy(text);
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long val = std::wcstoull(copy.c_str(), &end, 10);
    if (errno != 0 || end == copy.c_str() || *end != L'\0')
        return 0;
    return static_cast<uint64_t>(val);
}

HANDLE AcquireGuardMutex()
{
    KernelObjectSecurity security = KernelObjectSecurity::CreateSameUserMutex();
    if (!security.Valid()) {
        Log(L"guard: could not build security descriptor for guard mutex (%lu)", GetLastError());
        return nullptr;
    }
    HANDLE held = CreateMutexExW(security.Get(), kGuardMutex, CREATE_MUTEX_INITIAL_OWNER, MUTEX_ALL_ACCESS);
    if (!held) {
        Log(L"guard: could not create guard mutex (%lu)", GetLastError());
        return nullptr;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD wait = WaitForSingleObject(held, 5000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            Log(L"guard: timed out or failed waiting for guard mutex (%lu, %lu)", wait, GetLastError());
            CloseHandle(held);
            return nullptr;
        }
    }
    return held;
}

} // namespace

std::wstring InstanceGenerationPath()
{
    const std::wstring dir = DataDirectory();
    return dir.empty() ? std::wstring() : (dir + L"\\instance.gen");
}

uint64_t ReadInstanceGeneration(const std::wstring& path)
{
    if (path.empty())
        return 0;
    return ParseGenerationText(ReadTextFile(path));
}

uint64_t AdvanceInstanceGeneration(const std::wstring& path)
{
    const uint64_t next = ReadInstanceGeneration(path) + 1;
    if (!path.empty() && !WriteTextFile(path, std::to_wstring(next)))
        Log(L"guard: could not persist instance generation %llu (%lu)", static_cast<unsigned long long>(next), GetLastError());
    g_activeGeneration.store(next);
    return next;
}

void SetActiveInstanceGeneration(uint64_t generation) { g_activeGeneration.store(generation); }

uint64_t ActiveInstanceGeneration() { return g_activeGeneration.load(); }

bool ShouldGuardUndoLeftover(uint64_t guardGeneration, uint64_t activeGeneration)
{
    if (guardGeneration == 0 || activeGeneration == 0)
        return true;
    return activeGeneration <= guardGeneration;
}

// While Windows' taskbar or its banners are hidden, a second Atrium.exe (`--guard`, RunGuard)
// watches this one: however it ends (a crash past the exception filter, abort, being killed),
// what it changed in Windows is put back at once; while it does not respond, the taskbar shows.
void EnsureGuard(HANDLE& guard)
{
    if (guard && WaitForSingleObject(guard, 0) == WAIT_TIMEOUT)
        return;
    if (guard) {
        CloseHandle(guard);
        guard = nullptr;
    }
    uint64_t generation = g_activeGeneration.load();
    if (generation == 0)
        generation = AdvanceInstanceGeneration();
    wchar_t exe[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe))))
        return;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --guard " + std::to_wstring(GetCurrentProcessId()) + L" --guard-gen " +
                           std::to_wstring(generation) + DataDirectoryArgs();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"guard: could not start (%lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    guard = pi.hProcess;
    Log(L"guard: started (%lu, gen %llu)", pi.dwProcessId, static_cast<unsigned long long>(generation));
}

int RunGuard(DWORD pid, const wchar_t* hostClass, uint64_t generation)
{
    HANDLE held = AcquireGuardMutex();
    if (!held)
        return 1;
    auto release = [](HANDLE h) {
        ReleaseMutex(h);
        CloseHandle(h);
    };
    std::unique_ptr<void, decltype(release)> guard(held, release);
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return 1;
    HWND host = nullptr;
    int silent = 0, traySilent = 0; // checks in a row without an answer (and Atrium's tray host)
    bool shown = false, trayBack = false;
    while (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
        // Apps' tray calls go to the tray host first and wait for it. Not answering
        // (Atrium hung, or its host thread), Explorer's taskbar goes in front of it so
        // that they reach Explorer; the host moves back once it runs again.
        HWND trayHost = FindProcessWindow(L"Shell_TrayWnd", pid);
        DWORD_PTR answer = 0;
        const bool trayAlive = !trayHost || SendMessageTimeoutW(trayHost, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &answer);
        traySilent = trayAlive ? 0 : traySilent + 1;
        if (traySilent >= 2) {
            if (HWND shell = ShellTaskbar(pid))
                SetWindowPos(shell, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            if (!trayBack)
                Log(L"guard: Atrium's tray host (%lu) is not responding: Explorer's tray in front", pid);
            trayBack = true;
        } else if (trayAlive && trayBack) {
            Log(L"guard: Atrium's tray host (%lu) responds again", pid);
            trayBack = false;
        }
        if (!host || !IsWindow(host))
            host = FindProcessWindow(hostClass, pid);
        const bool alive = !host || SendMessageTimeoutW(host, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &answer);
        silent = alive ? 0 : silent + 1;
        if (silent >= 4 && !shown) {
            // About 15 s without an answer: the taskbar back (Atrium hides it again if it recovers).
            Log(L"guard: Atrium (%lu) is not responding: Windows' taskbar shown", pid);
            SystemChrome::ShowTaskbarsNow();
            shown = true;
        } else if (alive && shown) {
            Log(L"guard: Atrium (%lu) responds again", pid);
            shown = false;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    const uint64_t activeGeneration = ReadInstanceGeneration();
    if (!ShouldGuardUndoLeftover(generation, activeGeneration)) {
        Log(L"guard: Atrium (%lu, gen %llu) superseded by gen %llu; skipping chrome undo", pid, static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(activeGeneration));
        return 0;
    }
    // A clean exit put everything back (system.json is gone); anything else is put back here.
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        if (SystemChrome::UndoLeftover())
            Log(L"guard: Atrium (%lu) ended without putting Windows back (exit 0x%08lX): done here", pid, code);
        CoUninitialize();
    }
    return 0;
}

// The guard of a copy that quits: it puts Windows' taskbar back as that copy ends.
void WaitForGuard()
{
    if (HANDLE guard = OpenMutexW(SYNCHRONIZE, FALSE, kGuardMutex)) {
        DWORD done = WaitForSingleObject(guard, 5000);
        if (done == WAIT_OBJECT_0 || done == WAIT_ABANDONED)
            ReleaseMutex(guard);
        CloseHandle(guard);
    }
}

} // namespace atrium
