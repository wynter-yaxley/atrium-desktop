#include "shell/app_processes.h"
#include "shell/task_model.h"
#include "shell/task_rules.h"

#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

using namespace atrium;
namespace fs = std::filesystem;

namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

bool g_failed = false;

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << "FAIL: " << message << '\n';
    g_failed = true;
    return false;
}

Handle Spawn(const fs::path& exe, const std::vector<std::wstring>& arguments)
{
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    for (const auto& argument : arguments)
        command += L" \"" + argument + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
                 "start isolated probe process"))
        return Handle(nullptr, CloseHandle);
    CloseHandle(process.hThread);
    return Handle(process.hProcess, CloseHandle);
}

Handle FindProcess(const fs::path& exe)
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), CloseHandle);
    if (!Require(snapshot.get() != INVALID_HANDLE_VALUE, "snapshot probe processes"))
        return Handle(nullptr, CloseHandle);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID), CloseHandle);
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT && QueryFullProcessImageNameW(process.get(), 0, path, &length) &&
            _wcsicmp(path, exe.c_str()) == 0)
            return process;
    }
    return Handle(nullptr, CloseHandle);
}

struct Fixture {
    fs::path root;
    std::vector<fs::path> copies;
    std::vector<Handle> children;

    Fixture()
    {
        wchar_t temporary[MAX_PATH]{};
        if (!Require(GetTempPathW(ARRAYSIZE(temporary), temporary) != 0, "read temporary directory"))
            return;
        root = fs::path(temporary) / (L"Atrium-process-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        fs::create_directory(root);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path Copy(const fs::path& source, const wchar_t* directory, const wchar_t* name)
    {
        const auto folder = root / directory;
        fs::create_directory(folder);
        const auto destination = folder / name;
        fs::copy_file(source, destination);
        copies.push_back(destination);
        return destination;
    }

    ~Fixture()
    {
        // Cleanup only disposable probe copies, even if a check failed halfway through.
        for (auto& process : children) {
            if (!process)
                continue;
            TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process.get(), 5000);
        }
        for (const auto& copy : copies)
            while (auto process = FindProcess(copy)) {
                TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
                if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0)
                    break;
            }
        std::error_code ignored;
        for (const auto& copy : copies) {
            fs::remove(copy, ignored);
            fs::remove(copy.parent_path(), ignored);
        }
        if (!root.empty())
            fs::remove(root, ignored);
    }
};

void CheckSharedFolders()
{
    auto known = [](REFKNOWNFOLDERID id) {
        PWSTR raw = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &raw);
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> guard(raw, CoTaskMemFree);
        if (!Require(SUCCEEDED(hr) && raw != nullptr, "query known folder path"))
            return fs::path{};
        return fs::path(raw);
    };
    wchar_t buffer[MAX_PATH]{};
    if (!Require(GetWindowsDirectoryW(buffer, ARRAYSIZE(buffer)) != 0, "read Windows directory"))
        return;
    const fs::path windowsDir(buffer);
    if (!Require(GetSystemDirectoryW(buffer, ARRAYSIZE(buffer)) != 0, "read System32 directory"))
        return;
    const fs::path systemDir(buffer);
    if (!Require(GetTempPathW(ARRAYSIZE(buffer), buffer) != 0, "read temporary directory"))
        return;
    const fs::path tempDir(buffer);
    const fs::path programFiles = known(FOLDERID_ProgramFiles);
    const fs::path downloads = known(FOLDERID_Downloads);
    const fs::path localAppData = known(FOLDERID_LocalAppData);
    if (programFiles.empty() || downloads.empty() || localAppData.empty())
        return;

    Require(IsSharedAppFolder(L""), "empty folder is treated as shared");
    Require(IsSharedAppFolder(L"C:\\"), "drive root is treated as shared");
    Require(IsSharedAppFolder(L"c:/"), "drive root with forward slash is treated as shared");
    Require(IsSharedAppFolder(windowsDir.wstring()), "Windows directory root is shared");
    Require(!IsSharedAppFolder((windowsDir / L"CustomApp").wstring()), "Windows subdirectory outside System32/SysWOW64 is allowed");
    Require(IsSharedAppFolder(systemDir.wstring()), "System32 is shared");
    Require(IsSharedAppFolder((systemDir / L"WindowsPowerShell\\v1.0\\").wstring()), "System32 subdirectory is shared");
    Require(IsSharedAppFolder((systemDir.wstring() + L"/WindowsPowerShell//v1.0")), "normalized System32 subdirectory is shared");
    if (GetSystemWow64DirectoryW(buffer, ARRAYSIZE(buffer)) != 0) {
        const fs::path sysWow64(buffer);
        Require(IsSharedAppFolder(sysWow64.wstring()), "SysWOW64 is shared");
        Require(IsSharedAppFolder((sysWow64 / L"WindowsPowerShell\\v1.0\\").wstring()), "SysWOW64 subdirectory is shared");
    }
    Require(IsSharedAppFolder(programFiles.wstring()), "Program Files root is shared");
    Require(IsSharedAppFolder(downloads.wstring()), "Downloads root is shared");
    Require(IsSharedAppFolder(tempDir.wstring()), "Temp root is shared");
    Require(!IsSharedAppFolder(L"C:\\Program Files\\MyApp\\"), "dedicated Program Files app folder is not shared");
    Require(!IsSharedAppFolder((programFiles / L"MyApp").wstring()), "Program Files app subfolder is not shared");
    Require(!IsSharedAppFolder((localAppData / L"Programs\\MyApp\\").wstring()), "LocalAppData Programs app folder is not shared");
}

void CheckTermination(const fs::path& self)
{
    Require(!CanTerminateApp({}), "missing identity is disabled");
    Require(!CanTerminateApp({L"C:\\Windows\\EXPLORER.EXE", {}}), "Explorer is protected regardless of case");
    Require(!CanTerminateApp({L"C:\\Windows\\System32\\ApplicationFrameHost.exe", L"app!id"}), "shared UWP host is protected");
    Require(CanTerminateApp({{}, L"package!app"}), "packaged apps can use AUMID alone");

    Fixture fixture;
    if (fixture.root.empty())
        return;
    Require(!IsSharedAppFolder((fixture.root / L"app").wstring()), "isolated test app folder is not shared");
    const auto app = fixture.Copy(self, L"app", L"probe.exe");
    // The app starts a helper from its own folder, which starts another program from elsewhere.
    const auto helper = fixture.Copy(self, L"app\\bin", L"helper.exe");
    const auto grandchild = fixture.Copy(self, L"game", L"background.exe");
    const auto unrelated = fixture.Copy(self, L"unrelated", L"probe.exe");
    const auto shell = fixture.Copy(self, L"shell", L"explorer.exe");
    const auto eventName = L"Local\\AtriumProcessTests-" + std::to_wstring(GetCurrentProcessId());
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()), CloseHandle);
    if (!Require(bool(ready), "create probe readiness event"))
        return;
    fixture.children.push_back(Spawn(app, {L"--probe", eventName, helper.wstring(), grandchild.wstring()}));
    if (!Require(bool(fixture.children.back()), "start probe root") ||
        !Require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0, "helper tree is ready"))
        return;
    fixture.children.push_back(FindProcess(helper));
    fixture.children.push_back(FindProcess(grandchild));
    if (!Require(bool(fixture.children[1]) && bool(fixture.children[2]), "capture helper tree handles"))
        return;
    fixture.children.push_back(Spawn(app, {L"--probe", eventName}));
    fixture.children.push_back(Spawn(unrelated, {L"--probe", eventName}));
    fixture.children.push_back(Spawn(shell, {L"--probe", eventName}));
    if (!Require(bool(fixture.children[3]) && bool(fixture.children[4]) && bool(fixture.children[5]), "start sibling probes"))
        return;

    std::wstring upperPath = app.wstring();
    CharUpperBuffW(upperPath.data(), DWORD(upperPath.size()));
    const AppProcessIdentity target{upperPath, {}};
    Require(TerminateAppProcesses({&target, 1}), "end application and background processes");
    for (size_t i : {0, 1, 3})
        Require(WaitForSingleObject(fixture.children[i].get(), 5000) == WAIT_OBJECT_0, "app, its helper and background instance exit");
    Require(WaitForSingleObject(fixture.children[2].get(), 0) == WAIT_TIMEOUT, "a program the app started from elsewhere stays alive");
    Require(WaitForSingleObject(fixture.children[4].get(), 0) == WAIT_TIMEOUT, "same filename in another directory stays alive");

    const AppProcessIdentity shellTarget{shell.wstring(), {}};
    Require(TerminateAppProcesses({&shellTarget, 1}), "protected hosts are skipped");
    Require(WaitForSingleObject(fixture.children[5].get(), 0) == WAIT_TIMEOUT, "Explorer process is never terminated");
    const AppProcessIdentity selfTarget{self.wstring(), {}};
    Require(TerminateAppProcesses({&selfTarget, 1}), "current process is protected");
    Require(WaitForSingleObject(fixture.children[4].get(), 0) == WAIT_TIMEOUT, "current process descendants stay alive when it is skipped");
}

IconImage MakeBadge(int size, uint8_t r, uint8_t g, uint8_t b, bool splitPattern = false)
{
    std::vector<uint8_t> px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4, 0);
    const int pad = size / 8;
    for (int y = pad; y < size - pad; ++y) {
        for (int x = pad; x < size - pad; ++x) {
            const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)) * 4;
            const bool rightHalf = splitPattern && x >= size / 2;
            px[off + 0] = rightHalf ? r : b;
            px[off + 1] = g;
            px[off + 2] = rightHalf ? b : r;
            px[off + 3] = 255;
        }
    }
    return IconImage::FromBgra(size, size, std::move(px));
}

void CheckTaskRules()
{
    Require(IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, 0, false, false, 1200, 800), "normal overlapped window is accepted");
    Require(IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, 0, false, true, 0, 0), "iconic window with zero rect is accepted");
    Require(IsTaskbarWindowCandidate(WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, false, false, 1280, 720),
            "frameless Chrome/Electron window with WS_SYSMENU|WS_MINIMIZEBOX is accepted");
    Require(IsTaskbarWindowCandidate(WS_POPUP, WS_EX_TOPMOST, false, false, 800, 600), "large frameless WS_POPUP window is accepted");
    Require(!IsTaskbarWindowCandidate(WS_POPUP, WS_EX_TOPMOST, false, false, 200, 60), "small floating horizontal WS_POPUP widget is rejected");
    Require(!IsTaskbarWindowCandidate(WS_POPUP, WS_EX_TOPMOST, false, false, 64, 64), "small floating 64x64 WS_POPUP widget is rejected");
    Require(!IsTaskbarWindowCandidate(WS_POPUP, WS_EX_NOACTIVATE, false, false, 120, 240), "small floating vertical WS_POPUP widget is rejected");
    Require(!IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_APPWINDOW, false, false, 1920, 1080),
            "click-through layered overlay is rejected");
    Require(!IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_APPWINDOW, false, false, 1200, 800),
            "WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE is rejected even with WS_EX_APPWINDOW");
    Require(!IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, 0, false, false, 0, 0), "zero-size non-iconic window is rejected");
    Require(!IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, 0, true, false, 800, 600), "owned window without WS_EX_APPWINDOW is rejected");
    Require(IsTaskbarWindowCandidate(WS_OVERLAPPEDWINDOW, WS_EX_APPWINDOW, true, false, 800, 600), "owned window with WS_EX_APPWINDOW is accepted");

    Require(AllowsMultipleWindows(PinnedApp{L"Explorer", L"C:\\Windows\\explorer.exe", {}, L"Microsoft.Windows.Explorer", std::nullopt}),
            "Explorer defaults to multiple windows when unspecified");
    Require(!AllowsMultipleWindows(PinnedApp{L"Explorer", L"C:\\Windows\\explorer.exe", {}, L"Microsoft.Windows.Explorer", false}),
            "explicit multipleWindows=false overrides Explorer default");
    Require(!AllowsMultipleWindows(PinnedApp{L"Explorer", {}, {}, L"Microsoft.Windows.Explorer", false}),
            "explicit multipleWindows=false overrides Explorer AUMID default");
    Require(AllowsMultipleWindows(PinnedApp{L"Chrome", L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", {}, {}, std::nullopt}),
            "browsers default to multiple windows when unspecified");
    Require(!AllowsMultipleWindows(PinnedApp{L"Notes", L"C:\\Apps\\notes.exe", {}, {}, std::nullopt}),
            "other desktop apps default to single-window activation");
    Require(AllowsMultipleWindows(PinnedApp{L"Notes", L"C:\\Apps\\notes.exe", {}, {}, true}),
            "explicit multipleWindows=true enables new windows for other apps");

    const IconImage badge32 = MakeBadge(32, 220, 40, 40);
    const IconImage badge64 = MakeBadge(64, 220, 40, 40);
    const IconImage blue64 = MakeBadge(64, 40, 40, 220);
    const IconImage split64 = MakeBadge(64, 220, 40, 40, true);
    const IconImage empty;
    Require(!ImagesVisuallyDifferent(badge32, badge64), "identical badge at 32x32 and 64x64 compares equal");
    Require(ImagesVisuallyDifferent(badge32, blue64), "red vs blue badges compare different");
    Require(ImagesVisuallyDifferent(badge64, split64), "distinct badge patterns compare different");
    Require(!ImagesVisuallyDifferent(empty, empty), "two empty images compare safe and equal");
    Require(ImagesVisuallyDifferent(empty, badge32), "empty vs valid image is safe");
}

void CheckPendingRestores()
{
    const HWND a = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1001)), b = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1002)),
               c = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1003));
    std::vector<PendingRestore> pending{{a, 2000}, {b, 2000}, {c, 3000}};
    auto ready = TakeRestored(pending, 1000, [&](HWND h) { return h == a ? WindowShow::Shown : h == b ? WindowShow::Gone : WindowShow::Minimized; });
    Require(ready.size() == 1 && ready[0] == a && pending.size() == 1 && pending[0].window == c, "a restored window is taken, a gone one dropped");
    ready = TakeRestored(pending, 3000, [](HWND) { return WindowShow::Minimized; });
    Require(ready.empty() && pending.empty(), "a window still minimized at its deadline is dropped, not activated");
}

void CheckHiddenMainWindow()
{
    Require(PickHiddenMainWindow({}) == -1, "no hidden window, none picked");
    Require(PickHiddenMainWindow({{{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, 0, true},
                                  {{0, 0, 1200, 800}, WS_OVERLAPPEDWINDOW, WS_EX_TOOLWINDOW, false},
                                  {{100, 100, 299, 300}, WS_OVERLAPPEDWINDOW, 0, false},
                                  {{100, 100, 400, 249}, WS_OVERLAPPEDWINDOW, 0, false}}) == -1,
            "owned, tool and small windows are never the main one");
    Require(PickHiddenMainWindow({{{0, 0, 1920, 1080}, WS_POPUP, 0, false},
                                  {{0, 0, 400, 300}, WS_OVERLAPPEDWINDOW, 0, false},
                                  {{152, 152, 2880, 1591}, WS_CAPTION | WS_THICKFRAME, 0, false}}) == 2,
            "the largest captioned window comes before a larger uncaptioned one");
    Require(PickHiddenMainWindow({{{0, 0, 300, 200}, WS_POPUP, 0, false}, {{0, 0, 1280, 800}, WS_POPUP, 0, false}}) == 1,
            "among uncaptioned windows the largest");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && std::wstring(argv[1]) == L"--probe") {
        if (argc > 3) {
            std::vector<std::wstring> arguments{L"--probe", argv[2]};
            for (int i = 4; i < argc; ++i)
                arguments.emplace_back(argv[i]);
            auto child = Spawn(argv[3], arguments);
            if (!child)
                return 1;
        } else {
            Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]), CloseHandle);
            if (!Require(ready && SetEvent(ready.get()), "signal probe readiness"))
                return 1;
        }
        Handle idle(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
        if (idle)
            WaitForSingleObject(idle.get(), INFINITE);
        return 0;
    }
    wchar_t self[32768]{};
    if (Require(GetModuleFileNameW(nullptr, self, ARRAYSIZE(self)) != 0, "read test executable path")) {
        CheckSharedFolders();
        CheckTermination(self);
        CheckTaskRules();
        CheckPendingRestores();
        CheckHiddenMainWindow();
    }
    if (g_failed)
        return 1;
    std::cout << "PASS: application termination, taskbar rules, and process isolation\n";
    return 0;
}
