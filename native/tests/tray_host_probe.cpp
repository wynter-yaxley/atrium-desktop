// A live check of the tray host (shell/tray_host.h) on this desktop: not part of
// ctest, it puts a host in front of Explorer's tray for a few seconds. Run it with
// Atrium's own host off. It registers two icons of its own (protocol 0 by window
// and id, protocol 4 by GUID), checks that the host received them and Explorer
// still has them, sends clicks and checks what the icons' window received, answers
// for their place, survives malformed data, leaves another program's tray host in
// front and comes back after it, and removes its icons.
//   tray_host_probe.exe            the checks
//   tray_host_probe.exe --fake     (internal) a stand-in for another tray host
#include "shell/shell_taskbar.h"
#include "shell/tray_host.h"

#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace atrium;

namespace {

constexpr UINT kCallback = WM_APP + 41;
constexpr GUID kGuid = {0x5b8e2a61, 0x3c0d, 0x4f6e, {0x9a, 0x11, 0x2c, 0x7d, 0x40, 0xe3, 0x18, 0x5f}};
std::vector<std::pair<WPARAM, LPARAM>> g_got;
int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

LRESULT CALLBACK IconProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == kCallback) {
        g_got.emplace_back(w, l);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Pump(DWORD ms)
{
    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    }
    const ULONGLONG until = GetTickCount64() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= until)
            break;
        const DWORD remaining = static_cast<DWORD>(until - now);
        if (timer) {
            if (MsgWaitForMultipleObjects(1, &timer, FALSE, remaining, QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, remaining, QS_ALLINPUT);
        }
    }
    if (timer)
        CloseHandle(timer);
}

DWORD RunSelfAndWait(const wchar_t* argument, DWORD fallbackExit)
{
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + argument;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    DWORD code = fallbackExit;
    if (CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        for (;;) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                break;
            if (MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, DWORD(deadline - now), QS_ALLINPUT) == WAIT_OBJECT_0)
                break;
        }
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return code;
}

DWORD FirstTrayPid()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &pid);
    return pid;
}

NOTIFYICONDATAW Data(HWND w, bool v4)
{
    NOTIFYICONDATAW d{sizeof(d)};
    d.hWnd = w;
    d.uID = v4 ? 2 : 1;
    d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | (v4 ? NIF_GUID : 0);
    d.uCallbackMessage = kCallback;
    d.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcscpy_s(d.szTip, v4 ? L"Atrium probe v4" : L"Atrium probe v0");
    d.guidItem = kGuid;
    return d;
}

const HostIcon* Find(const std::vector<HostIcon>& icons, const wchar_t* tip)
{
    for (const HostIcon& i : icons)
        if (i.tip == tip)
            return &i;
    return nullptr;
}

int Fake()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Shell_TrayWnd", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Pump(3000);
    DestroyWindow(w);
    return 0;
}

void ExerciseIconActions(TrayHost& host, HWND window)
{
    NOTIFYICONDATAW v0 = Data(window, false), v4 = Data(window, true);
    Check(Shell_NotifyIconW(NIM_ADD, &v0) != FALSE, "add (protocol 0)");
    Check(Shell_NotifyIconW(NIM_ADD, &v4) != FALSE, "add (protocol 4, GUID)");
    v4.uVersion = NOTIFYICON_VERSION_4;
    Check(Shell_NotifyIconW(NIM_SETVERSION, &v4) != FALSE, "set version 4");
    Pump(200);
    auto icons = host.Icons();
    const HostIcon* a = Find(icons, L"Atrium probe v0");
    const HostIcon* b = Find(icons, L"Atrium probe v4");
    Check(a && a->image && a->version == 0 && a->callback == kCallback && a->uid == 1, "the host has the protocol 0 icon, its picture and callback");
    Check(b && b->image && b->version == 4 && b->key.starts_with(L"guid:"), "the host has the GUID icon at protocol 4");

    // A changed tip is live in the host.
    wcscpy_s(v0.szTip, L"Atrium probe v0 changed");
    v0.uFlags = NIF_TIP;
    Check(Shell_NotifyIconW(NIM_MODIFY, &v0) != FALSE, "modify");
    Pump(100);
    icons = host.Icons();
    a = Find(icons, L"Atrium probe v0 changed");
    b = Find(icons, L"Atrium probe v4");
    Check(a != nullptr, "the change is live in the host");

    const POINT at{321, 654};
    if (a) {
        g_got.clear();
        Check(host.Send(a->key, tray::HostAction::Left, at) == HostSendStatus::Sent, "send a click (protocol 0)");
        Pump(150);
        Check(g_got.size() == 3 && g_got[0].first == 1 && g_got[1].second == WM_LBUTTONDOWN && g_got[2].second == WM_LBUTTONUP,
              "protocol 0 click: id in wParam, MOVE DOWN UP");
    }
    if (b) {
        g_got.clear();
        Check(host.Send(b->key, tray::HostAction::Right, at) == HostSendStatus::Sent, "send a right click (protocol 4)");
        Pump(150);
        Check(g_got.size() == 4 && LOWORD(g_got[3].second) == WM_CONTEXTMENU && HIWORD(g_got[3].second) == 2 && short(LOWORD(g_got[3].first)) == 321 &&
                  short(HIWORD(g_got[3].first)) == 654,
              "protocol 4 right click ends in WM_CONTEXTMENU at the pointer");
        g_got.clear();
        host.Send(b->key, tray::HostAction::Double, at);
        Pump(150);
        Check(g_got.size() == 6 && LOWORD(g_got[3].second) == NIN_SELECT && LOWORD(g_got[4].second) == WM_LBUTTONDBLCLK,
              "protocol 4 double click: the click, NIN_SELECT, then WM_LBUTTONDBLCLK");
        host.SetPlaces(RECT{10, 10, 50, 50});
        // Asked from another process, as an app asks.
        const DWORD code = RunSelfAndWait(L"--rect", 0);
        std::printf("      (left %ld)\n", long(int(code)));
        Check(int(code) == at.x - 16, "an app asking where its icon is gets the place of the click");
    }
}

void ExerciseMalformedAndYield(TrayHost& host)
{
    // Malformed data must neither crash the host nor stop it.
    HWND first = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD junk = 0x34753423;
    COPYDATASTRUCT bad{1, sizeof(junk), &junk};
    DWORD_PTR answer = 0;
    SendMessageTimeoutW(first, WM_COPYDATA, 0, LPARAM(&bad), SMTO_BLOCK, 3000, &answer);
    COPYDATASTRUCT empty{3, 0, nullptr};
    SendMessageTimeoutW(first, WM_COPYDATA, 0, LPARAM(&empty), SMTO_BLOCK, 3000, &answer);
    Check(host.Running() && SendMessageTimeoutW(first, WM_NULL, 0, 0, SMTO_BLOCK, 1000, &answer), "malformed data: the host still answers");

    // Another program's tray host in front: left there, and back after it.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --fake";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Pump(1000);
        Check(host.Yielded() && FirstTrayPid() == pi.dwProcessId, "another tray host in front is left there");
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Pump(800);
        Check(!host.Yielded() && FirstTrayPid() == GetCurrentProcessId(), "after it, the host is in front again");
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wstring(argv[1]) == L"--fake")
        return Fake();
    if (argc > 1 && std::wstring(argv[1]) == L"--rect") {
        NOTIFYICONIDENTIFIER id{sizeof(id)};
        id.guidItem = kGuid;
        RECT r{};
        return SUCCEEDED(Shell_NotifyIconGetRect(&id, &r)) ? int(r.left) : -1;
    }
    if (argc > 1 && std::wstring(argv[1]) == L"--appbar") {
        APPBARDATA abd{sizeof(abd)};
        return SHAppBarMessage(ABM_GETTASKBARPOS, &abd) && abd.rc.right > abd.rc.left ? 0 : 1;
    }
    if (FirstTrayPid() != 0) {
        DWORD shell = 0;
        GetWindowThreadProcessId(GetShellWindow(), &shell);
        if (FirstTrayPid() != shell) {
            std::printf("another tray host is in front (pid %lu): turn Atrium's off first\n", FirstTrayPid());
            return 2;
        }
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = IconProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AtriumTrayHostProbe";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    APPBARDATA state{sizeof(state)};
    const UINT_PTR stateBefore = SHAppBarMessage(ABM_GETSTATE, &state);
    bool kept = false;
    {
        TrayHost host(nullptr, WM_APP);
        Pump(300);
        Check(host.Running() && FirstTrayPid() == GetCurrentProcessId(), "the host is the first Shell_TrayWnd");

        APPBARDATA abd{sizeof(abd)};
        Check(AppBarMessage(ABM_GETSTATE, abd) == stateBefore, "the taskbar state Atrium reads is Explorer's while its host is first");
        abd.lParam = LPARAM(stateBefore);
        AppBarMessage(ABM_SETSTATE, abd);
        Check(SHAppBarMessage(ABM_GETSTATE, &state) == stateBefore, "setting the taskbar state reaches Explorer while the host is first");
        Check(RunSelfAndWait(L"--appbar", 1) == 0, "app bar messages reach Explorer through the host (another process)");

        ExerciseIconActions(host, window);
        ExerciseMalformedAndYield(host);
        kept = true;
    }
    // The host is gone: Explorer takes the calls, and it has had every icon all along.
    Pump(300);
    DWORD shell = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shell);
    Check(FirstTrayPid() == shell, "without the host Explorer's taskbar is first again");
    NOTIFYICONDATAW v0 = Data(window, false), v4 = Data(window, true);
    v0.uFlags = v4.uFlags = NIF_TIP;
    v4.uFlags |= NIF_GUID;
    Check(Shell_NotifyIconW(NIM_MODIFY, &v0) != FALSE && Shell_NotifyIconW(NIM_MODIFY, &v4) != FALSE, "Explorer had both icons");
    Check(Shell_NotifyIconW(NIM_DELETE, &v0) != FALSE && Shell_NotifyIconW(NIM_DELETE, &v4) != FALSE, "removed");
    DestroyWindow(window);
    std::printf("%s (%d failed)\n", g_failures || !kept ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
