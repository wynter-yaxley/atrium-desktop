// Window identity, taskbar window filtering, shortcut reading, and pin resolution.

#include "shell/task_model.h"
#include "shell/shell_icon.h"
#include "shell/task_rules.h"

#include <windows.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace atrium::detail {

namespace {

constexpr UINT kWindowIconTimeoutMs = 100; // WM_GETICON on a possibly hung window

// AUMID of a packaged process; empty for classic desktop processes.
std::wstring PackagedAumid(HANDLE process)
{
    wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH] = {};
    UINT32 len = ARRAYSIZE(buf);
    if (GetApplicationUserModelId(process, &len, buf) != ERROR_SUCCESS)
        return {};
    return buf;
}

std::wstring StoreAumid(IPropertyStore* store)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        result = v.pwszVal;
    PropVariantClear(&v);
    return result;
}

struct WindowAppIdentity {
    std::wstring aumid;
    std::wstring relaunchCommand;
    std::wstring relaunchIcon;
    std::wstring relaunchName;
};

std::wstring StoreStringProp(IPropertyStore* store, const PROPERTYKEY& key)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        result = v.pwszVal;
    PropVariantClear(&v);
    return result;
}

std::wstring ResolveDisplayResource(const std::wstring& raw)
{
    if (raw.empty() || raw.front() != L'@')
        return raw;
    wchar_t buf[512] = {};
    if (SUCCEEDED(SHLoadIndirectString(raw.c_str(), buf, ARRAYSIZE(buf), nullptr)) && buf[0])
        return buf;
    return {};
}

WindowAppIdentity ReadWindowAppIdentity(HWND h)
{
    WindowAppIdentity id;
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(store.put()))) || !store)
        return id;
    id.aumid = StoreStringProp(store.get(), PKEY_AppUserModel_ID);
    id.relaunchCommand = StoreStringProp(store.get(), PKEY_AppUserModel_RelaunchCommand);
    id.relaunchIcon = StoreStringProp(store.get(), PKEY_AppUserModel_RelaunchIconResource);
    id.relaunchName = ResolveDisplayResource(StoreStringProp(store.get(), PKEY_AppUserModel_RelaunchDisplayNameResource));
    return id;
}

// UWP frame windows live in ApplicationFrameHost; the app itself owns a child
// "Windows.UI.Core.CoreWindow" in another process.
std::wstring HostedUwpAumid(HWND frame, DWORD framePid)
{
    struct Ctx {
        DWORD framePid;
        std::wstring aumid;
    } ctx{framePid, {}};
    EnumChildWindows(
        frame,
        [](HWND child, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[64] = {};
            GetClassNameW(child, cls, ARRAYSIZE(cls));
            if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (!pid || pid == c->framePid)
                return TRUE;
            winrt::handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
            if (proc)
                c->aumid = PackagedAumid(proc.get());
            return c->aumid.empty() ? TRUE : FALSE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.aumid;
}

// The program that started this process, where it verifiably is the app's launcher (#52, Baidu
// Netdisk: BaiduNetdisk.exe starts module\BrowserEngine\BaiduNetdiskUnite.exe, whose window is the
// app's): the same session, started before it, in a folder above its program, which is named after
// it. A game started by its store (a folder below too, another name) stays apart.
std::wstring LauncherOf(HANDLE process, DWORD pid, ULONGLONG created, const std::wstring& exePath)
{
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto kQuery = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    struct Basic { // PROCESS_BASIC_INFORMATION
        LONG exitStatus;
        PVOID peb;
        ULONG_PTR affinity;
        LONG priority;
        ULONG_PTR pid, parent;
    } info{};
    if (!kQuery || kQuery(process, 0 /* ProcessBasicInformation */, &info, sizeof(info), nullptr) < 0 || !info.parent)
        return {};
    const DWORD parentPid = static_cast<DWORD>(info.parent);
    DWORD session = 0, parentSession = 0;
    if (!ProcessIdToSessionId(pid, &session) || !ProcessIdToSessionId(parentPid, &parentSession) || session != parentSession)
        return {};
    winrt::handle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid));
    FILETIME c{}, e{}, k{}, u{};
    if (!parent || !GetProcessTimes(parent.get(), &c, &e, &k, &u) || ((static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime) > created)
        return {}; // that id is another, later process now
    std::wstring launcher = TaskProcessImagePath(parent.get());
    const size_t slash = launcher.find_last_of(L'\\');
    if (slash == std::wstring::npos)
        return {};
    std::wstring dir = Lower(launcher.substr(0, slash + 1)), own = Lower(exePath);
    if (!own.starts_with(dir) || own.find(L'\\', dir.size()) == std::wstring::npos)
        return {}; // not in a folder below the launcher's
    std::wstring stem = Lower(FileStem(launcher));
    if (stem.size() < 4 || !Lower(FileStem(exePath)).starts_with(stem))
        return {};
    return launcher;
}

} // namespace

std::wstring TaskHwndKey(HWND h)
{
    wchar_t buf[40];
    swprintf_s(buf, L"hwnd:%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return buf;
}

TaskWinInfo ComputeTaskWinInfo(HWND h, DWORD pid, const wchar_t* cls)
{
    TaskWinInfo wi;
    wi.pid = pid;
    bool frame = wcscmp(cls, L"ApplicationFrameWindow") == 0;
    winrt::handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (frame) {
        wi.aumid = HostedUwpAumid(h, pid);
        if (wi.aumid.empty()) {
            const WindowAppIdentity id = ReadWindowAppIdentity(h);
            wi.aumid = id.aumid;
            wi.relaunchCommand = id.relaunchCommand;
            wi.relaunchIcon = id.relaunchIcon;
            wi.relaunchName = id.relaunchName;
        }
        // exePath stays empty: it would be ApplicationFrameHost.exe for every app.
    } else {
        const WindowAppIdentity id = ReadWindowAppIdentity(h);
        wi.aumid = id.aumid;
        wi.relaunchCommand = id.relaunchCommand;
        wi.relaunchIcon = id.relaunchIcon;
        wi.relaunchName = id.relaunchName;
        if (proc) {
            wi.exePath = TaskProcessImagePath(proc.get());
            if (wi.aumid.empty())
                wi.aumid = PackagedAumid(proc.get());
            if (wi.aumid.empty() && wcscmp(cls, L"CabinetWClass") == 0 && Lower(PathFindFileNameW(wi.exePath.c_str())) == L"explorer.exe")
                wi.aumid = L"Microsoft.Windows.Explorer";
        }
    }
    if (proc) {
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(proc.get(), &c, &e, &k, &u))
            wi.created = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    }
    wi.webApp = IsWebApp(wi.aumid, wi.relaunchCommand);
    if (wi.aumid.empty() && proc && !wi.exePath.empty() && wi.created)
        wi.launcher = LauncherOf(proc.get(), pid, wi.created, wi.exePath);
    if (!wi.aumid.empty())
        wi.key = Lower(wi.aumid);
    else if (!wi.exePath.empty())
        wi.key = Lower(wi.exePath);
    else
        wi.key = TaskHwndKey(h);
    return wi;
}

// The Windows taskbar's rules for what gets a button.
bool IsTaskAppWindow(HWND h, DWORD& pid, wchar_t* cls, int clsCap, std::wstring& title)
{
    RECT rc{};
    if (!IsWindowVisible(h) || !GetWindowRect(h, &rc))
        return false;
    if (!IsTaskbarWindowCandidate(GetWindowLongPtrW(h, GWL_STYLE), GetWindowLongPtrW(h, GWL_EXSTYLE), GetWindow(h, GW_OWNER) != nullptr, IsIconic(h) != FALSE,
                                  rc.right - rc.left, rc.bottom - rc.top))
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId())
        return false;
    if (!GetClassNameW(h, cls, clsCap))
        return false;
    // Shell windows, plus top-level CoreWindows (Start, Search: shell UI, not apps;
    // real UWP apps appear through their ApplicationFrameWindow).
    for (const wchar_t* shell : {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW", L"Windows.UI.Core.CoreWindow"})
        if (wcscmp(cls, shell) == 0)
            return false;
    // InternalGetWindowText never sends WM_GETTEXT, so a hung window cannot block us.
    wchar_t text[512] = {};
    if (InternalGetWindowText(h, text, ARRAYSIZE(text)) <= 0)
        return false;
    title = text;
    return true;
}

// Loads a .lnk without resolving it (no disk search, no UI).
bool ReadTaskLink(const std::wstring& path, std::wstring& target, std::wstring& aumid, std::wstring* args)
{
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))))
        return false;
    auto file = link.try_as<IPersistFile>();
    if (!file || FAILED(file->Load(path.c_str(), STGM_READ)))
        return false;
    wchar_t buf[MAX_PATH * 2] = {};
    if (SUCCEEDED(link->GetPath(buf, ARRAYSIZE(buf), nullptr, 0)))
        target = buf;
    if (args) {
        std::array<wchar_t, 1024> line{};
        if (SUCCEEDED(link->GetArguments(line.data(), static_cast<int>(line.size()))))
            *args = line.data();
    }
    if (auto store = link.try_as<IPropertyStore>())
        aumid = StoreAumid(store.get());
    return true;
}

std::wstring TaskFileDescription(const std::wstring& path)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    struct Lang {
        WORD lang, codepage;
    };
    std::vector<Lang> langs;
    Lang* tr = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr)
        for (UINT i = 0; i < len / sizeof(Lang); ++i)
            langs.push_back(tr[i]);
    langs.insert(langs.end(), {{0x0409, 0x04B0}, {0x0409, 0x04E4}});
    for (const Lang& l : langs) {
        wchar_t sub[80];
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription", l.lang, l.codepage);
        wchar_t* value = nullptr;
        UINT n = 0;
        if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &n) && value && n > 1 && value[0])
            return value;
    }
    return {};
}

std::wstring TaskShellDisplayName(const std::wstring& parsingName)
{
    winrt::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
        return {};
    PWSTR name = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name)
        result = name;
    CoTaskMemFree(name);
    return result;
}

// The window's own icon via WM_GETICON only (no GCLP_HICON class fallback).
// SendMessageTimeout bounds hung windows to 100 ms; the HICON belongs to the app.
bool TaskWindowDirectIcon(HWND h, int size, IconImage& out)
{
    if (!IsWindow(h) || IsHungAppWindow(h))
        return false;
    const WPARAM order[] = {size > 24 ? WPARAM(ICON_BIG) : WPARAM(ICON_SMALL), WPARAM(ICON_SMALL2), WPARAM(ICON_BIG), WPARAM(ICON_SMALL)};
    for (WPARAM type : order) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(h, WM_GETICON, type, 0, SMTO_ABORTIFHUNG, kWindowIconTimeoutMs, &result) && result &&
            HIconToImage(reinterpret_cast<HICON>(result), out))
            return true;
    }
    return false;
}

bool TaskWindowIcon(HWND h, int size, IconImage& out)
{
    if (TaskWindowDirectIcon(h, size, out))
        return true;
    for (int index : {GCLP_HICON, GCLP_HICONSM}) {
        HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(h, index));
        if (icon && HIconToImage(icon, out))
            return true;
    }
    return false;
}

TaskPin ResolveTaskPin(const PinnedApp& app)
{
    TaskPin p;
    p.app = app;
    std::wstring aumid = app.aumid;
    std::wstring target, args = app.args;
    if (!app.path.empty()) {
        if (IsLink(app.path)) {
            std::wstring linkAumid;
            if (ReadTaskLink(app.path, target, linkAumid, &args) && aumid.empty())
                aumid = linkAumid; // the link's own AUMID wins over the path
        } else {
            target = app.path;
        }
    }
    if (aumid.empty() && !target.empty() && IsPackagedPath(target))
        aumid = PackagedAumidForExe(target);
    p.webApp = IsWebApp(aumid, args);
    p.specific = p.webApp || Lower(args).find(L"--profile-directory") != std::wstring::npos;
    p.exe = AppExecutable(target);
    p.aumid = aumid;
    if (Lower(aumid) == L"microsoft.windows.explorer" && p.exe.empty()) {
        wchar_t winDir[MAX_PATH] = {};
        if (GetWindowsDirectoryW(winDir, ARRAYSIZE(winDir))) {
            p.exe = std::wstring(winDir) + (winDir[wcslen(winDir) - 1] == L'\\' ? L"explorer.exe" : L"\\explorer.exe");
            target = p.exe;
        }
    }
    p.multipleWindows = AllowsMultipleWindows(PinnedApp{app.name, p.exe, args, aumid, app.multipleWindows});
    p.target = Lower(target);
    if (!aumid.empty()) {
        p.key = Lower(aumid);
    } else if (!target.empty()) {
        p.key = p.target;
    } else {
        p.key = Lower(app.path);
    }
    if (p.key.empty())
        p.key = Lower(app.name);
    return p;
}

} // namespace atrium::detail
