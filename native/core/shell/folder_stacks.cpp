// Folder stack entries: path normalization, deduplication, background
// display-name/icon loading and Explorer window reuse or launch.

#include "shell/folder_stacks.h"

#include "shell/jump_list.h"
#include "shell/shell_icon.h"
#include "shell/shell_launch.h"
#include "shell/shell_taskbar.h"
#include "win32_path.h"

#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <ocidl.h>
#include <exdisp.h>
#include <servprov.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace atrium {

namespace {

bool IsWhitespace(wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L'\v' || ch == L'\f'; }

bool IsSlash(wchar_t ch) { return ch == L'\\' || ch == L'/'; }

bool IsRootSlash(const std::wstring& path)
{
    if (path.size() == 1)
        return IsSlash(path[0]);
    if (path.size() == 2)
        return path[0] == L'\\' && path[1] == L'\\';
    if (path.size() == 3)
        return std::iswalpha(static_cast<wint_t>(path[0])) != 0 && path[1] == L':' && IsSlash(path[2]);
    if (path.size() == 7 && (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")))
        return std::iswalpha(static_cast<wint_t>(path[4])) != 0 && path[5] == L':' && IsSlash(path[6]);
    return false;
}

std::wstring DedupeKey(std::wstring path)
{
    for (wchar_t& ch : path) {
        if (ch == L'/')
            ch = L'\\';
    }
    return FoldCase(path);
}

std::wstring FallbackFolderName(const std::wstring& path)
{
    size_t end = path.size();
    while (end > 0 && IsSlash(path[end - 1]))
        --end;
    if (end == 0)
        return path;
    const size_t slash = path.find_last_of(L"\\/", end - 1);
    if (slash == std::wstring::npos || slash + 1 >= end)
        return path;
    return path.substr(slash + 1, end - (slash + 1));
}

std::wstring FileNameStem(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    const size_t dot = path.find_last_of(L'.');
    const size_t end = (dot != std::wstring::npos && dot > start) ? dot : path.size();
    return path.substr(start, end - start);
}

std::wstring TrimAscii(std::wstring path)
{
    size_t start = 0;
    while (start < path.size() && IsWhitespace(path[start]))
        ++start;
    size_t end = path.size();
    while (end > start && IsWhitespace(path[end - 1]))
        --end;
    if (start > 0 || end < path.size())
        path = path.substr(start, end - start);
    return path;
}

std::vector<std::wstring> DedupePaths(std::vector<std::wstring> paths, size_t maxCount, bool isFolder)
{
    std::vector<std::wstring> out;
    if (maxCount == 0)
        return out;
    out.reserve(std::min(paths.size(), maxCount));
    std::unordered_set<std::wstring> seen;
    for (auto& raw : paths) {
        std::wstring norm = isFolder ? NormalizeFolderPath(std::move(raw)) : TrimAscii(std::move(raw));
        if (norm.empty())
            continue;
        if (seen.insert(DedupeKey(norm)).second) {
            out.push_back(std::move(norm));
            if (out.size() >= maxCount)
                break;
        }
    }
    return out;
}

struct StaComScope {
    bool owned = false;
    StaComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ~StaComScope()
    {
        if (owned)
            CoUninitialize();
    }
    StaComScope(const StaComScope&) = delete;
    StaComScope& operator=(const StaComScope&) = delete;
};

struct ScopedPidl {
    PIDLIST_ABSOLUTE pidl = nullptr;
    ScopedPidl() = default;
    explicit ScopedPidl(PIDLIST_ABSOLUTE p) : pidl(p) {}
    ~ScopedPidl() { CoTaskMemFree(pidl); }
    ScopedPidl(const ScopedPidl&) = delete;
    ScopedPidl& operator=(const ScopedPidl&) = delete;
};

bool IsImageFile(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return false;
    const wchar_t* ext = path.c_str() + dot;
    static constexpr const wchar_t* kImageExts[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".ico", L".tif", L".tiff", L".heic", L".avif",
    };
    return std::ranges::any_of(kImageExts, [ext](const wchar_t* e) { return _wcsicmp(ext, e) == 0; });
}

StackFolder ResolveEntry(const std::wstring& path, int iconPx, bool isFile)
{
    StackFolder entry;
    entry.path = path;
    if (IsRemotePath(path)) {
        entry.isFile = isFile;
        const std::wstring fallback = isFile ? FileNameStem(path) : FallbackFolderName(path);
        entry.name = fallback.empty() ? path : fallback;
        return entry;
    }

    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
        isFile = (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    entry.isFile = isFile;
    const std::wstring fallback = isFile ? FileNameStem(path) : FallbackFolderName(path);

    winrt::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))) || !item) {
        entry.name = fallback.empty() ? path : fallback;
        entry.missing = true;
        return entry;
    }

    if (isFile) {
        entry.name = fallback.empty() ? FallbackFolderName(path) : fallback;
    } else {
        PWSTR display = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display && *display)
            entry.name = display;
        else
            entry.name = fallback;
        CoTaskMemFree(display);
    }

    if (!isFile || !IsImageFile(path) || !LoadShellImage(path, iconPx, entry.icon, true))
        LoadShellImage(path, iconPx, entry.icon, false);
    return entry;
}

bool TryFocusOpenExplorer(const std::wstring& path)
{
    if (IsRemotePath(path))
        return false;
    PIDLIST_ABSOLUTE rawTarget = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &rawTarget, 0, nullptr)) || !rawTarget)
        return false;
    const ScopedPidl target(rawTarget);

    winrt::com_ptr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put()))) || !windows)
        return false;

    long count = 0;
    if (FAILED(windows->get_Count(&count)))
        return false;

    for (long i = 0; i < count; ++i) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = i;
        winrt::com_ptr<IDispatch> disp;
        if (windows->Item(index, disp.put()) != S_OK || !disp)
            continue;
        auto browser = disp.try_as<IWebBrowserApp>();
        auto sp = disp.try_as<IServiceProvider>();
        if (!browser || !sp)
            continue;
        SHANDLE_PTR rawHwnd = 0;
        if (FAILED(browser->get_HWND(&rawHwnd)) || !rawHwnd)
            continue;
        const HWND hwnd = reinterpret_cast<HWND>(rawHwnd);
        if (!IsWindow(hwnd) || IsHungAppWindow(hwnd) || !ShellResponsive(hwnd, 150, false))
            continue;
        winrt::com_ptr<IShellBrowser> shellBrowser;
        if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(shellBrowser.put()))) || !shellBrowser)
            continue;
        winrt::com_ptr<IShellView> shellView;
        if (FAILED(shellBrowser->QueryActiveShellView(shellView.put())) || !shellView)
            continue;
        auto folderView = shellView.try_as<IFolderView>();
        if (!folderView)
            continue;
        winrt::com_ptr<IPersistFolder2> persistFolder;
        if (FAILED(folderView->GetFolder(IID_PPV_ARGS(persistFolder.put()))) || !persistFolder)
            continue;
        PIDLIST_ABSOLUTE rawCur = nullptr;
        if (FAILED(persistFolder->GetCurFolder(&rawCur)) || !rawCur)
            continue;
        const ScopedPidl cur(rawCur);
        if (!ILIsEqual(target.pidl, cur.pidl))
            continue;
        if (IsIconic(hwnd))
            ShowWindowAsync(hwnd, SW_RESTORE);
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(hwnd);
        return true;
    }
    return false;
}

// A new File Explorer window on `path`, without waiting on Explorer: the new explorer.exe hands the
// folder to the running shell and leaves.
void LaunchExplorer(const std::wstring& path)
{
    // A root's trailing backslash would escape the closing quote; doubled, it reads as one.
    const std::wstring arg = path.ends_with(L'\\') ? path + L'\\' : path;
    LaunchRequest req;
    req.target = L"explorer.exe";
    req.parameters = L"\"" + arg + L"\"";
    req.verb = L"open";
    req.kind = LaunchTargetKind::Folder;
    req.privilege = LaunchPrivilege::ShellDefault;
    ShellLaunch(req);
}

void OpenFile(const std::wstring& path)
{
    LaunchRequest req;
    req.target = path;
    req.verb = L"open";
    req.kind = LaunchTargetKind::Document;
    req.privilege = LaunchPrivilege::ShellDefault;
    req.noAsync = !IsRemotePath(path);
    ShellLaunch(req);
}

} // namespace

std::wstring NormalizeFolderPath(std::wstring path)
{
    path = TrimAscii(std::move(path));
    while (!path.empty() && IsSlash(path.back()) && !IsRootSlash(path))
        path.pop_back();
    return path;
}

std::vector<std::wstring> DedupeFolders(std::vector<std::wstring> paths, size_t maxCount) { return DedupePaths(std::move(paths), maxCount, true); }

FolderStackModel::FolderStackModel(HWND notify, UINT msg) : notify_(notify), msg_(msg), thread_(&FolderStackModel::Worker, this) {}

FolderStackModel::~FolderStackModel()
{
    {
        std::lock_guard g(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void FolderStackModel::SetFolders(std::vector<std::wstring> folders, std::vector<std::wstring> files, int iconPx)
{
    std::vector<std::wstring> normFolders = DedupePaths(std::move(folders), 32, true);
    std::vector<std::wstring> normFiles = DedupePaths(std::move(files), 32, false);
    const int clampedPx = std::clamp(iconPx, 8, 512);
    {
        std::lock_guard g(mutex_);
        pendingFolders_ = std::move(normFolders);
        pendingFiles_ = std::move(normFiles);
        pendingIconPx_ = clampedPx;
        ++pendingGen_;
    }
    cv_.notify_one();
}

std::vector<StackFolder> FolderStackModel::Snapshot() const
{
    std::lock_guard g(mutex_);
    return snapshot_;
}

void FolderStackModel::OpenFolder(const std::wstring& path, bool forceNewWindow, bool isFile)
{
    std::wstring norm = isFile ? TrimAscii(path) : NormalizeFolderPath(path);
    if (norm.empty())
        return;
    {
        std::lock_guard g(mutex_);
        openJobs_.push_back(OpenJob{std::move(norm), forceNewWindow, isFile});
    }
    cv_.notify_one();
}

void FolderStackModel::Worker()
{
    const StaComScope com;
    for (;;) {
        OpenJob openJob;
        bool hasOpen = false;
        std::vector<std::wstring> folders, files;
        int iconPx = 32;
        uint64_t gen = 0;
        {
            std::unique_lock g(mutex_);
            cv_.wait(g, [&] { return stop_ || !openJobs_.empty() || pendingGen_ != resolvedGen_; });
            if (stop_)
                break;
            if (!openJobs_.empty()) {
                openJob = std::move(openJobs_.front());
                openJobs_.pop_front();
                hasOpen = true;
            } else {
                folders = pendingFolders_;
                files = pendingFiles_;
                iconPx = pendingIconPx_;
                gen = pendingGen_;
            }
        }
        if (hasOpen) {
            if (openJob.isFile) {
                OpenFile(openJob.path);
                continue;
            }
            // Explorer's windows are asked only while it answers: a hung shell never holds this thread
            // (nor Atrium's exit, which joins it).
            const bool tryFocus = !openJob.forceNewWindow && !IsRemotePath(openJob.path) && ShellResponsive(GetShellWindow(), 300, false);
            if (!tryFocus || !TryFocusOpenExplorer(openJob.path))
                LaunchExplorer(openJob.path);
            continue;
        }
        ResolveBatch(folders, files, iconPx, gen);
    }
}

void FolderStackModel::ResolveBatch(const std::vector<std::wstring>& folders, const std::vector<std::wstring>& files, int iconPx, uint64_t gen)
{
    std::vector<StackFolder> resolved;
    resolved.reserve(folders.size() + files.size());
    auto append = [&](const std::vector<std::wstring>& paths, bool isFile) {
        for (const std::wstring& path : paths) {
            {
                std::lock_guard g(mutex_);
                if (stop_ || pendingGen_ != gen)
                    return false;
            }
            resolved.push_back(ResolveEntry(path, iconPx, isFile));
        }
        return true;
    };
    if (!append(folders, false) || !append(files, true))
        return;
    HWND notify = nullptr;
    UINT msg = 0;
    {
        std::lock_guard g(mutex_);
        if (stop_ || pendingGen_ != gen)
            return;
        snapshot_ = std::move(resolved);
        resolvedGen_ = gen;
        notify = notify_;
        msg = msg_;
    }
    if (notify)
        PostMessageW(notify, msg, 0, 0);
}

} // namespace atrium
