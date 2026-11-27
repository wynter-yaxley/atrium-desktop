#include "launch_args.h"
#include "log.h"
#include "shell/process_integrity.h"
#include "shell/wifi.h"
#include "win32_path.h"

#include <shellapi.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace atrium;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

bool TestCoreLaunchOptions()
{
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"D:\\tests\\--exit-case"});
        if (!Require(!opt.exitRequested && !opt.guard && !opt.uninstall && !opt.debugCapturable && opt.readyEvent.empty(),
                     "--data-dir value with --exit-case must not set exitRequested"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"D:\\tests\\--guard 123"});
        if (!Require(!opt.guard && opt.guardPid == 0, "--data-dir value containing --guard 123 must not trigger guard"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"D:\\tests\\--debug-capturable"});
        if (!Require(!opt.debugCapturable, "--data-dir value containing --debug-capturable must not enable capture"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--data-dir", L"--exit"});
        if (!Require(!opt.exitRequested, "--data-dir followed by --exit must treat --exit as the path value"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--exit-case"});
        if (!Require(!opt.exitRequested, "--exit-case must not match --exit"))
            return false;
    }
    {
        const CoreLaunchOptions standalone = ParseCoreLaunchOptions({L"Atrium.exe", L"--uninstall"});
        const CoreLaunchOptions withExtra = ParseCoreLaunchOptions({L"Atrium.exe", L"--uninstall", L"--extra"});
        if (!Require(standalone.uninstall, "standalone --uninstall must set uninstall") ||
            !Require(!withExtra.uninstall, "--uninstall with extra arguments must not set uninstall"))
            return false;
    }
    {
        const CoreLaunchOptions missing = ParseCoreLaunchOptions({L"Atrium.exe", L"--guard"});
        const CoreLaunchOptions zero = ParseCoreLaunchOptions({L"Atrium.exe", L"--guard", L"0"});
        const CoreLaunchOptions trailing = ParseCoreLaunchOptions({L"Atrium.exe", L"--guard", L"12abc"});
        if (!Require(missing.guard && missing.guardPid == 0, "bare --guard must set guard=true and guardPid=0") ||
            !Require(zero.guard && zero.guardPid == 0, "--guard 0 must leave guardPid=0") ||
            !Require(trailing.guard && trailing.guardPid == 0, "--guard 12abc must leave guardPid=0"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions(
            {L"Atrium.exe", L"--guard", L"4242", L"--data-dir", L"D:\\tests\\--exit-case", L"--ready-event", L"Local\\Atrium.Update.Core.abc"});
        if (!Require(opt.guard && opt.guardPid == 4242 && !opt.exitRequested && opt.readyEvent == L"Local\\Atrium.Update.Core.abc",
                     "--guard 4242 with --data-dir and --ready-event must parse all tokens accurately"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--exit"});
        if (!Require(opt.exitRequested && !opt.replace, "exact --exit token must set exitRequested"))
            return false;
    }
    {
        const CoreLaunchOptions opt = ParseCoreLaunchOptions({L"Atrium.exe", L"--replace"});
        if (!Require(opt.replace && !opt.exitRequested, "exact --replace token must set replace"))
            return false;
    }
    {
        if (!Require(!IsHigherIntegrityRid(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID), "same RID must not count as higher integrity") ||
            !Require(!IsHigherIntegrityRid(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID), "lower RID must not count as higher integrity") ||
            !Require(IsHigherIntegrityRid(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID), "high RID > medium RID must count as higher") ||
            !Require(!IsElevatedIntegrityRid(SECURITY_MANDATORY_MEDIUM_RID), "medium RID must not count as elevated") ||
            !Require(IsElevatedIntegrityRid(SECURITY_MANDATORY_HIGH_RID), "high RID must count as elevated") ||
            !Require(SelfIntegrityRid() != 0, "SelfIntegrityRid must be non-zero"))
            return false;
    }
    return true;
}

bool TestSettingsLaunchArgs()
{
    const std::wstring devArgs = L" --development --data-dir \"D:\\dev\\--exit-case\"";
    if (!Require(FormatSettingsLaunchArgs(nullptr, L"").empty(), "nullptr page + empty dev args must be empty") ||
        !Require(FormatSettingsLaunchArgs(L"", L"").empty(), "empty page + empty dev args must be empty") ||
        !Require(FormatSettingsLaunchArgs(L"widgets", L"") == L"--page widgets", "widgets + empty dev args must format --page widgets") ||
        !Require(FormatSettingsLaunchArgs(nullptr, devArgs) == L"--development --data-dir \"D:\\dev\\--exit-case\"",
                 "nullptr page + dev args must trim leading space") ||
        !Require(FormatSettingsLaunchArgs(L"", devArgs) == L"--development --data-dir \"D:\\dev\\--exit-case\"",
                 "empty page + dev args must trim leading space") ||
        !Require(FormatSettingsLaunchArgs(L"widgets", devArgs) == L"--page widgets --development --data-dir \"D:\\dev\\--exit-case\"",
                 "widgets + dev args must join with a single space"))
        return false;

    const std::wstring backslashArgs = FormatSettingsLaunchArgs(L"widgets", L" --development --data-dir \"C:\\\\\"");
    const std::wstring cmdLine = L"\"Atrium.Settings.exe\" " + backslashArgs;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    const bool roundTripOk = argv && argc == 6 && std::wstring(argv[1]) == L"--page" && std::wstring(argv[2]) == L"widgets" &&
                             std::wstring(argv[3]) == L"--development" && std::wstring(argv[4]) == L"--data-dir" && std::wstring(argv[5]) == L"C:\\";
    if (argv)
        LocalFree(static_cast<HLOCAL>(argv));
    return Require(roundTripOk, "FormatSettingsLaunchArgs with escaped trailing backslash must round-trip through CommandLineToArgvW");
}

} // namespace

int main()
{
    const std::wstring self = ModulePath();
    if (!Require(!self.empty(), "ModulePath must succeed"))
        return 1;

    const std::wstring className = L"AtriumCoreExitTest_" + std::to_wstring(GetCurrentProcessId());
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = className.c_str();
    if (!Require(RegisterClassExW(&wc) != 0, "RegisterClassExW must succeed"))
        return 1;

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, className.c_str(), L"CoreExitTest", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!Require(hwnd != nullptr, "CreateWindowExW must succeed")) {
        UnregisterClassW(className.c_str(), instance);
        return 1;
    }

    const std::wstring otherPath = L"C:\\NonExistent\\OtherDir\\Atrium.exe";
    const bool ok =
        Require(!SameExecutableWindow(nullptr), "null window must not match") &&
        Require(SameExecutableWindow(hwnd), "window owned by current exe must match ModulePath()") &&
        Require(SameExecutableWindow(hwnd, self), "window owned by current exe must match explicit self path") &&
        Require(!SameExecutableWindow(hwnd, otherPath), "window must not match a different executable path") &&
        Require(FindWindowForExecutable(className.c_str()) == hwnd, "FindWindowForExecutable must find matching window") &&
        Require(FindWindowForExecutable(className.c_str(), otherPath) == nullptr, "FindWindowForExecutable must skip windows belonging to other executables") &&
        Require(FindWindowForExecutable(L"AtriumCoreExitMissingClass") == nullptr, "FindWindowForExecutable must return nullptr when class is absent");

    DestroyWindow(hwnd);
    UnregisterClassW(className.c_str(), instance);
    if (!ok || !Require(FindWindowForExecutable(className.c_str()) == nullptr, "destroyed window must not be found"))
        return 1;

    HANDLE canceled = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!Require(canceled != nullptr, "CreateEventW must succeed"))
        return 1;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<WifiNetwork> nets = ListWifi(true, canceled).networks;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CloseHandle(canceled);
    if (!Require(nets.empty() && ms < 1000, "ListWifi(true, signaled) must return empty without 3.5s sleep"))
        return 1;

    Log(L"core_exit: shutdown check");
    ShutdownLog();
    ShutdownLog();
    Log(L"core_exit: after shutdown");

    if (!TestCoreLaunchOptions() || !TestSettingsLaunchArgs())
        return 1;

    std::cout << "core exit: all checks passed\n";
    return 0;
}
