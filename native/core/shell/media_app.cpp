// The media session's player as an app: its window brought forward, its name as the shell shows it.
#include "shell/media_app.h"

#include "shell/shell_launch.h"
#include "shell/task_rules.h"
#include "win32_path.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <winrt/base.h>

#include <cwctype>
#include <vector>

namespace atrium {

namespace {

bool ProcessMatchesAppId(DWORD pid, const std::wstring& targetFolded, const std::wstring& targetStem, std::wstring* outImagePath = nullptr)
{
    const winrt::handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process)
        return false;
    const std::wstring aumid = FoldCase(ProcessAumid(process.get()));
    const std::wstring rawPath = ProcessImagePath(process.get());
    const std::wstring path = FoldCase(rawPath);
    const bool matched =
        (!aumid.empty() && aumid == targetFolded) || (!path.empty() && (path == targetFolded || (!targetStem.empty() && ExeStemFolded(path) == targetStem)));
    if (matched && outImagePath)
        *outImagePath = rawPath;
    return matched;
}

std::wstring FindRunningProcessImage(const std::wstring& targetFolded, const std::wstring& targetStem)
{
    const winrt::handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap)
        return {};
    PROCESSENTRY32W pe{sizeof(pe)};
    std::wstring imagePath;
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        if (ProcessMatchesAppId(pe.th32ProcessID, targetFolded, targetStem, &imagePath) && !imagePath.empty())
            return imagePath;
    }
    return {};
}

struct FindWindowCtx {
    std::wstring targetFolded, targetStem;
    HWND found = nullptr;
};

BOOL CALLBACK EnumAppWindowProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid && ProcessMatchesAppId(pid, ctx->targetFolded, ctx->targetStem)) {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindHiddenAppWindow(const std::wstring& targetFolded, const std::wstring& targetStem)
{
    struct Look {
        const std::wstring *folded, *stem;
        std::vector<HWND> hwnds;
        std::vector<HiddenWindow> cands;
    } look{&targetFolded, &targetStem};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto& k = *reinterpret_cast<Look*>(l);
            const DWORD ex = static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE));
            const bool owned = GetWindow(h, GW_OWNER) != nullptr, vis = IsWindowVisible(h) != FALSE, en = IsWindowEnabled(h) != FALSE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t cls[128] = {}, title[512] = {};
            GetClassNameW(h, cls, ARRAYSIZE(cls));
            InternalGetWindowText(h, title, ARRAYSIZE(title));
            if (!IsHiddenMainWindowCandidate(vis, owned, ex, en, cloaked != 0, title, cls))
                return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            RECT r{};
            if (!pid || !ProcessMatchesAppId(pid, *k.folded, *k.stem) || !GetWindowRect(h, &r))
                return TRUE;
            k.hwnds.push_back(h);
            k.cands.push_back(
                {r, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)), ex, owned, en, cloaked != 0, title[0] != L'\0', IsSystemHelperWindowClass(cls)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&look));
    const int i = PickHiddenMainWindow(look.cands);
    return i >= 0 ? look.hwnds[size_t(i)] : nullptr;
}

} // namespace

void ActivatePlayerApp(const std::wstring& appId)
{
    if (appId.empty())
        return;
    FindWindowCtx ctx{FoldCase(appId), ExeStemFolded(appId), nullptr};
    EnumWindows(EnumAppWindowProc, reinterpret_cast<LPARAM>(&ctx));
    HWND hwnd = ctx.found;
    const bool hasBang = appId.find(L'!') != std::wstring::npos;
    const bool isPackaged = hasBang || ctx.targetFolded.find(L"\\windowsapps\\") != std::wstring::npos;
    if (!hwnd && !isPackaged) {
        hwnd = FindHiddenAppWindow(ctx.targetFolded, ctx.targetStem);
        if (hwnd)
            ShowWindowAsync(hwnd, SW_SHOW);
    }
    if (hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (IsIconic(hwnd))
            ShowWindowAsync(hwnd, SW_RESTORE);
        if (pid != 0)
            AllowSetForegroundWindow(pid);
        SetForegroundWindow(hwnd);
        if (GetForegroundWindow() != hwnd)
            SwitchToThisWindow(hwnd, TRUE);
        return;
    }
    std::wstring target = isPackaged ? std::wstring{} : FindRunningProcessImage(ctx.targetFolded, ctx.targetStem);
    if (FoldCase(target).find(L"\\windowsapps\\") != std::wstring::npos)
        target.clear();
    LaunchRequest req;
    if (target.empty()) {
        if (hasBang) {
            req.target = appId;
            req.kind = LaunchTargetKind::AppUserModelId;
        } else if (isPackaged) {
            return;
        } else {
            req.target = appId;
            req.kind = LaunchTargetKind::Executable;
        }
    } else {
        req.target = target;
        req.kind = LaunchTargetKind::Executable;
    }
    req.verb = L"open";
    req.privilege = LaunchPrivilege::ShellDefault;
    req.noAsync = true;
    ShellLaunch(req);
}

std::wstring ResolveAppName(const std::wstring& appId, std::unordered_map<std::wstring, std::wstring>& cache)
{
    if (appId.empty())
        return {};
    if (const auto it = cache.find(appId); it != cache.end())
        return it->second;
    std::wstring name;
    winrt::com_ptr<IShellItem> item;
    const std::wstring parsing = L"shell:AppsFolder\\" + appId;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(item.put()))) && item) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) && raw) {
            name = raw;
            CoTaskMemFree(raw);
        }
    }
    if (name.empty()) {
        const size_t slash = appId.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? appId : appId.substr(slash + 1);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
            name.resize(name.size() - 4);
        if (!name.empty() && std::iswlower(static_cast<wint_t>(name.front())))
            name.front() = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(name.front())));
    }
    cache[appId] = name;
    return name;
}

} // namespace atrium
