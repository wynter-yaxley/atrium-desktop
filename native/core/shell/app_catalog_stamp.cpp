// Package installation timestamps, Start Menu shortcut indexing, and system/maker classification.

#include "shell/app_catalog.h"

#include "log.h"
#include "shell/app_catalog_rules.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace atrium {

namespace {

std::wstring Fold(const std::wstring& s) { return FoldCatalogText(s); }

bool IsPath(const std::wstring& id) { return IsCatalogDrivePath(id); }

constexpr const wchar_t* kHelperPackages[] = {
    L"microsoft.windowsfeedbackhub_",
    L"microsoft.gethelp_",
    L"microsoft.getstarted_",
    L"microsoft.windows.devhome_",
    L"microsoftcorporationii.quickassist_",
    L"microsoftcorporationii.microsoftfamily_",
    L"microsoft.powerautomatedesktop_",
    L"microsoft.xboxgamingoverlay_",
    L"microsoft.xboxdevices_",
    L"microsoft.sechealthui_",
    L"microsoft.yourphone_",
    L"microsoftwindows.crossdevice_",
    L"microsoft.storepurchaseapp_",
    L"microsoft.desktopappinstaller_",
    L"microsoft.microsoftpcmanager_",
    L"microsoft.copilot_",
    L"microsoft.startexperiencesapp_",
    L"microsoft.bingnews_",
    L"microsoft.bingweather_",
    L"microsoft.windowsmaps_",
    L"microsoft.people_",
    L"microsoft.mixedreality.portal_",
    L"microsoft.3dviewer_",
    L"microsoft.print3d_",
    L"microsoft.windowsalarms_",
    L"microsoft.windowscamera_",
    L"microsoft.windowssoundrecorder_",
    L"microsoftwindows.client.",
    L"windows.",
    L"appup.",
    L"nvidiacorp.",
    L"realteksemiconductorcorp.",
    L"dolbylaboratories.",
    L"b9ecedf6.",
    L"e046963f.",
    L"synapticsincorporated.",
    L"wavesaudio.",
    L"elanmicroelectronics.",
    L"amd.",
    L"advancedmicrodevices.",
    L"hpinc.",
    L"ad2f1837.",
};

int64_t Ticks(const FILETIME& t) { return static_cast<int64_t>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime); }

int64_t Created(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    return !path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) ? Ticks(fa.ftCreationTime) : 0;
}

struct PackageInfo {
    int64_t installed = 0;
    bool system = false;
};

std::map<std::wstring, PackageInfo> CollectInstalledPackages()
{
    std::map<std::wstring, PackageInfo> packages;
    try {
        namespace Wam = winrt::Windows::ApplicationModel;
        winrt::Windows::Management::Deployment::PackageManager manager;
        for (const auto& package : manager.FindPackagesForUser(L"")) {
            const auto id = package.Id();
            const std::wstring publisher = Fold(std::wstring(id.Publisher()));
            const std::wstring publisherDisplay = Fold(std::wstring(package.PublisherDisplayName()));
            const std::wstring family = Fold(std::wstring(id.FamilyName()));
            const bool system = package.SignatureKind() == Wam::PackageSignatureKind::System || CatalogByMaker(publisher) || CatalogByMaker(publisherDisplay) ||
                                std::ranges::any_of(kHelperPackages, [&](const wchar_t* p) { return family.starts_with(p); });
            packages[family] = {package.InstalledDate().time_since_epoch().count(), system};
        }
    } catch (const winrt::hresult_error&) {
        Log(L"catalog: packages' install dates unavailable");
    }
    return packages;
}

struct ShortcutIndex {
    std::map<std::wstring, int64_t> byTarget;
    std::map<std::wstring, int64_t> byName;
    std::set<std::wstring> systemTargets;
    std::set<std::wstring> systemNames;
    std::vector<CatalogApp> shortcuts;
};

void RecordEarliest(std::map<std::wstring, int64_t>& m, const std::wstring& key, int64_t t)
{
    if (key.empty() || t == 0)
        return;
    auto [it, added] = m.try_emplace(key, t);
    if (!added)
        it->second = std::min(it->second, t);
}

bool IsSystemShortcutRelativePath(const std::wstring& within)
{
    const size_t slash = within.find(L'\\');
    if (slash == std::wstring::npos)
        return false;
    const std::wstring top = within.substr(0, slash);
    return IsSystemStartFolder(top) || IsToolStartFolder(top);
}

bool IsStartupShortcutRelativePath(const std::wstring& within)
{
    const size_t slash = within.find(L'\\');
    return slash != std::wstring::npos && IsStartupStartFolder(within.substr(0, slash));
}

std::wstring ResolveMsiShortcutTarget(const std::wstring& link)
{
    static HMODULE msi = LoadLibraryW(L"msi.dll");
    using GetTargetFn = UINT(WINAPI*)(LPCWSTR, LPWSTR, LPWSTR, LPWSTR);
    using GetCompPathFn = UINT(WINAPI*)(LPCWSTR, LPCWSTR, LPWSTR, LPDWORD);
    static auto getTarget = msi ? reinterpret_cast<GetTargetFn>(GetProcAddress(msi, "MsiGetShortcutTargetW")) : nullptr;
    static auto getCompPath = msi ? reinterpret_cast<GetCompPathFn>(GetProcAddress(msi, "MsiGetComponentPathW")) : nullptr;
    if (!getTarget || !getCompPath)
        return {};
    wchar_t product[40]{}, component[40]{};
    if (getTarget(link.c_str(), product, nullptr, component) != ERROR_SUCCESS || !product[0] || !component[0])
        return {};
    DWORD len = 1024;
    std::wstring path(len, L'\0');
    const UINT state = getCompPath(product, component, path.data(), &len);
    if ((state == 3 || state == 4) && len > 0) { // INSTALLSTATE_LOCAL / INSTALLSTATE_SOURCE
        path.resize(len);
        return path;
    }
    return {};
}

std::wstring ResolveShellDisplayName(const std::wstring& link, const std::wstring& fallback)
{
    winrt::com_ptr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(link.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
        PWSTR s = nullptr;
        std::wstring out;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s && *s)
            out = s;
        CoTaskMemFree(s);
        if (!out.empty())
            return out;
    }
    return fallback;
}

void CollectTopLevelLinks(const std::wstring& dir, std::vector<std::pair<std::wstring, std::wstring>>& links)
{
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const std::wstring name = fd.cFileName;
        const std::wstring folded = Fold(name);
        if ((name.size() > 4 && folded.ends_with(L".lnk")) || (name.size() > 10 && folded.ends_with(L".appref-ms")))
            links.emplace_back(L"desktopAppLink", dir + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool NeedsMsiFallback(const std::wstring& target) { return target.empty() || Fold(target).find(L"\\windows\\installer\\") != std::wstring::npos; }

std::wstring PropVariantText(IPropertyStore* props, const PROPERTYKEY& key)
{
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        out = pv.pwszVal;
    PropVariantClear(&pv);
    return out;
}

void ReadShortcutEntry(const std::wstring& link, std::wstring& expandedTarget, std::wstring& args, std::wstring& aumid)
{
    winrt::com_ptr<IShellLinkW> shell;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shell.put()))) ||
        FAILED(shell.as<IPersistFile>()->Load(link.c_str(), STGM_READ)))
        return;
    wchar_t target[1024] = {};
    if (SUCCEEDED(shell->GetPath(target, 1024, nullptr, SLGP_RAWPATH)) && target[0])
        expandedTarget = ExpandKnownEnvFolders(target);
    if (NeedsMsiFallback(expandedTarget) && SUCCEEDED(shell->GetPath(target, 1024, nullptr, 0)) && target[0]) {
        std::wstring resolved = ExpandKnownEnvFolders(target);
        if (!NeedsMsiFallback(resolved) || expandedTarget.empty())
            expandedTarget = std::move(resolved);
    }
    if (auto props = shell.try_as<IPropertyStore>()) {
        aumid = PropVariantText(props.get(), PKEY_AppUserModel_ID);
        if (NeedsMsiFallback(expandedTarget))
            if (std::wstring parsed = PropVariantText(props.get(), PKEY_Link_TargetParsingPath); !parsed.empty())
                expandedTarget = ExpandKnownEnvFolders(parsed);
    }
    if (NeedsMsiFallback(expandedTarget))
        if (std::wstring msi = ResolveMsiShortcutTarget(link); !msi.empty())
            expandedTarget = std::move(msi);
    wchar_t rawArgs[1024] = {};
    if (SUCCEEDED(shell->GetArguments(rawArgs, static_cast<int>(std::size(rawArgs)))))
        args = rawArgs;
}

bool ValidDesktopShortcutTarget(const std::wstring& target, const std::wstring& windowsDir, bool isAppRef)
{
    if (target.empty() || Fold(target).starts_with(windowsDir))
        return false;
    if (isAppRef)
        return true;
    const DWORD attrs = GetFileAttributesW(target.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void IndexShortcutFolder(const KNOWNFOLDERID& id, ShortcutIndex& index, const std::wstring& windowsDir, bool recursive, bool desktopGuard)
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &root)) || !root) {
        CoTaskMemFree(root);
        return;
    }
    std::vector<std::pair<std::wstring, std::wstring>> links;
    const std::wstring baseDir = root;
    CoTaskMemFree(root);
    recursive ? CollectStartLinks(baseDir, links) : CollectTopLevelLinks(baseDir, links);
    for (const auto& [_, link] : links) {
        const std::wstring within = Fold(link.substr(std::min(link.size(), baseDir.size() + 1)));
        if (recursive && IsStartupShortcutRelativePath(within))
            continue;
        const int64_t t = Created(link);
        const std::wstring file = link.substr(link.find_last_of(L'\\') + 1);
        const size_t dot = file.rfind(L'.');
        const std::wstring stem = dot == std::wstring::npos ? file : file.substr(0, dot);
        const std::wstring displayName = ResolveShellDisplayName(link, stem);
        const bool inSystem = recursive && IsSystemShortcutRelativePath(within);
        if (!desktopGuard) {
            RecordEarliest(index.byName, Fold(stem), t);
            RecordEarliest(index.byName, Fold(displayName), t);
        }
        if (inSystem) {
            index.systemNames.insert(Fold(stem));
            index.systemNames.insert(Fold(displayName));
        }
        const bool isAppRef = Fold(file).ends_with(L".appref-ms");
        std::wstring expandedTarget = isAppRef ? link : L"", args, aumid;
        if (!isAppRef)
            ReadShortcutEntry(link, expandedTarget, args, aumid);
        if (desktopGuard && !ValidDesktopShortcutTarget(expandedTarget, windowsDir, isAppRef))
            continue;
        const std::wstring folded = Fold(expandedTarget);
        if (!desktopGuard)
            RecordEarliest(index.byTarget, folded, t);
        if (IsCatalogTargetMissing(expandedTarget))
            continue;
        if (inSystem && !folded.empty())
            index.systemTargets.insert(folded);
        if (!expandedTarget.empty() || !aumid.empty())
            index.shortcuts.push_back(CatalogApp{displayName, link, aumid, expandedTarget, args, t, inSystem});
    }
}

std::wstring ProgramFilesVendor(const std::wstring& target)
{
    const size_t pf = target.find(L"\\program files");
    if (pf == std::wstring::npos)
        return {};
    const size_t afterRoot = target.find(L'\\', pf + 1);
    if (afterRoot == std::wstring::npos)
        return {};
    const size_t nextSlash = target.find(L'\\', afterRoot + 1);
    return target.substr(afterRoot, nextSlash - afterRoot);
}

void StampApp(CatalogApp& app, const std::map<std::wstring, PackageInfo>& packages, const ShortcutIndex& index, const std::wstring& windowsDir)
{
    const std::wstring foldedId = Fold(app.id), foldedAumid = Fold(app.aumid);
    const std::wstring& aumid = app.aumid.empty() ? app.id : app.aumid;
    app.microsoft = IsBuiltInMicrosoftApp(foldedId, foldedAumid);
    if (const size_t bang = aumid.find(L'!'); bang != std::wstring::npos) {
        if (auto it = packages.find(Fold(aumid.substr(0, bang))); it != packages.end()) {
            app.installed = it->second.installed;
            app.system = !app.microsoft && it->second.system;
            return;
        }
    }
    const std::wstring target = Fold(app.target);
    const std::wstring exe = target.substr(target.find_last_of(L'\\') + 1);
    app.system = !app.microsoft &&
                 ((target.starts_with(windowsDir) && exe != L"explorer.exe") || index.systemTargets.contains(target) ||
                  index.systemNames.contains(Fold(app.name)) || (foldedId.starts_with(L"microsoft.windows.") && foldedId != L"microsoft.windows.explorer") ||
                  foldedId.starts_with(L"::{") || foldedId.starts_with(L"microsoft.office.setlang.") || exe == L"wslsettings.exe" || exe == L"setlang.exe" ||
                  IsVisualStudioInstallerApp(foldedId, target) || CatalogByMaker(ProgramFilesVendor(target)));
    if (IsPath(app.id) && Fold(app.id).ends_with(L".lnk")) {
        app.installed = Created(app.id);
        return;
    }
    if (auto it = index.byTarget.find(target); !app.target.empty() && it != index.byTarget.end())
        app.installed = it->second;
    else if (auto named = index.byName.find(Fold(app.name)); named != index.byName.end())
        app.installed = named->second;
    else
        app.installed = Created(app.target);
}

} // namespace

// When each app came, for Launchpad's order: a packaged app's package install date; a desktop app's start
// menu shortcut's creation (the installer made it), else its program's; 0 when unknown (Windows' own).
void StampInstalledApps(std::vector<CatalogApp>& apps)
{
    const std::map<std::wstring, PackageInfo> packages = CollectInstalledPackages();
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring windowsDir = Fold(std::wstring(windows) + L"\\");
    ShortcutIndex index;
    IndexShortcutFolder(FOLDERID_CommonPrograms, index, windowsDir, true, false);
    IndexShortcutFolder(FOLDERID_Programs, index, windowsDir, true, false);
    IndexShortcutFolder(FOLDERID_CommonStartMenu, index, windowsDir, false, false);
    IndexShortcutFolder(FOLDERID_StartMenu, index, windowsDir, false, false);
    IndexShortcutFolder(FOLDERID_PublicDesktop, index, windowsDir, false, true);
    IndexShortcutFolder(FOLDERID_Desktop, index, windowsDir, false, true);
    if (MergeStartMenuShortcuts(apps, index.shortcuts, &AppCatalog::IsLaunchable))
        SortCatalogApps(apps);
    for (CatalogApp& app : apps)
        StampApp(app, packages, index, windowsDir);
}

} // namespace atrium
