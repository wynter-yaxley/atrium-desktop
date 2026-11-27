#include "shell/app_catalog.h"

#include "log.h"
#include "shell/start_layout.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propkey.h>

#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <string_view>

namespace atrium {

namespace {

constexpr const wchar_t* kSystemFolders[] = {
    L"accessibility",
    L"accessories",
    L"administrative tools",
    L"maintenance",
    L"system tools",
    L"windows accessibility",
    L"windows accessories",
    L"windows administrative tools",
    L"windows ease of access",
    L"windows powershell",
    L"windows system",
    L"windows tools",
    L"windows 附件",
    L"windows 管理工具",
    L"windows 轻松使用",
    L"windows 系统",
    L"windows 工具",
    L"管理工具",
    L"系统工具",
    L"附件",
    L"辅助功能",
    L"轻松使用",
    L"维护",
};
constexpr const wchar_t* kToolFolders[] = {L"microsoft office tools", L"microsoft office 工具", L"windows kits"};

std::wstring Fold(const std::wstring& s)
{
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), out.data(), int(out.size()), nullptr, nullptr, 0);
    return out;
}

std::wstring PropString(IShellItem2* item, const PROPERTYKEY& key)
{
    PWSTR v = nullptr;
    std::wstring s;
    if (SUCCEEDED(item->GetString(key, &v)) && v)
        s = v;
    CoTaskMemFree(v);
    return s;
}

bool IsPath(const std::wstring& id) { return id.size() > 2 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/'); }

bool SameText(const std::wstring& a, const std::wstring& b)
{
    return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

bool IsWindows11()
{
    static const bool kEleven = [] {
        OSVERSIONINFOW v{sizeof(v)};
        auto get = reinterpret_cast<LONG(WINAPI*)(OSVERSIONINFOW*)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return get && get(&v) == 0 && v.dwBuildNumber >= 22000;
    }();
    return kEleven;
}

void UpdateMaxFileTime(FILETIME& best, const FILETIME& candidate)
{
    if (CompareFileTime(&candidate, &best) > 0)
        best = candidate;
}

void ScanCloudStoreChildren(HKEY parent, FILETIME& best)
{
    FILETIME ft{};
    if (RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &ft) == ERROR_SUCCESS)
        UpdateMaxFileTime(best, ft);
    wchar_t name[256]{};
    for (DWORD i = 0;; ++i) {
        DWORD len = DWORD(std::size(name));
        if (RegEnumKeyExW(parent, i, name, &len, nullptr, nullptr, nullptr, &ft) != ERROR_SUCCESS)
            break;
        UpdateMaxFileTime(best, ft);
    }
}

bool Win10TileLayoutStamp(FILETIME& stamp)
{
    static constexpr const wchar_t* kRoots[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\Cache\\DefaultAccount",
    };
    FILETIME best{};
    for (const wchar_t* rootPath : kRoots) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, rootPath, 0, KEY_READ, &root) != ERROR_SUCCESS)
            continue;
        wchar_t name[512]{};
        FILETIME ft{};
        for (DWORD i = 0;; ++i) {
            DWORD len = DWORD(std::size(name));
            if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, &ft) != ERROR_SUCCESS)
                break;
            const std::wstring folded = Fold(std::wstring(name, len));
            if (folded.find(L"curatedtilecollection") == std::wstring::npos && folded.find(L"unifiedtile.startglobalproperties") == std::wstring::npos)
                continue;
            UpdateMaxFileTime(best, ft);
            HKEY child = nullptr;
            if (RegOpenKeyExW(root, name, 0, KEY_READ, &child) == ERROR_SUCCESS) {
                ScanCloudStoreChildren(child, best);
                RegCloseKey(child);
            }
        }
        RegCloseKey(root);
    }
    if (best.dwHighDateTime == 0 && best.dwLowDateTime == 0)
        return false;
    stamp = best;
    return true;
}

bool StartLayoutStamp(bool win11, FILETIME& stamp)
{
    if (!win11)
        return Win10TileLayoutStamp(stamp);
    PWSTR local = nullptr;
    bool ok = false;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)) && local) {
        const std::wstring bin = std::wstring(local) + L"\\Packages\\Microsoft.Windows.StartMenuExperienceHost_cw5n1h2txyewy\\LocalState\\start2.bin";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(bin.c_str(), GetFileExInfoStandard, &fa) != 0) {
            stamp = fa.ftLastWriteTime;
            ok = true;
        }
    }
    CoTaskMemFree(local);
    return ok;
}

std::wstring ReadUtf8File(const std::wstring& path)
{
    std::wstring text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return text;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string bytes(size_t(std::min<LONGLONG>(size.QuadPart, 4 << 20)), '\0');
    DWORD read = 0;
    if (!bytes.empty() && ReadFile(f, bytes.data(), DWORD(bytes.size()), &read, nullptr)) {
        bytes.resize(read);
        const size_t skip = bytes.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, int(bytes.size() - skip), nullptr, 0);
        text.resize(size_t(std::max(n, 0)));
        MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, int(bytes.size() - skip), text.data(), n);
    }
    CloseHandle(f);
    return text;
}

// The Windows start menu's layout file (start2.bin) is encrypted; the
// supported way to read it is Export-StartLayout, which writes JSON or XML.
std::wstring ExportStartLayout(HANDLE stopEvent, bool xml, bool* cancelled = nullptr)
{
    if (cancelled)
        *cancelled = false;
    if (stopEvent && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
        if (cancelled)
            *cancelled = true;
        return {};
    }
    wchar_t temp[MAX_PATH] = {}, sys[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring file = std::wstring(temp) + L"atrium-start-layout-" + std::to_wstring(GetCurrentProcessId()) + (xml ? L".xml" : L".json");
    std::wstring quoted; // a PowerShell single-quoted string
    for (wchar_t ch : file)
        quoted += ch == L'\'' ? L"''" : std::wstring(1, ch);
    std::wstring cmd = L"\"" + std::wstring(sys) +
                       L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       L"-Command \"Export-StartLayout -Path '" +
                       quoted + L"'\"";
    DeleteFileW(file.c_str());
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"catalog: Export-StartLayout could not start (%lu)", GetLastError());
        return {};
    }
    HANDLE handles[2] = {pi.hProcess, stopEvent};
    const DWORD count = stopEvent ? 2 : 1;
    const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, 20000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        DeleteFileW(file.c_str());
        if (cancelled)
            *cancelled = true;
        Log(wait == WAIT_OBJECT_0 + 1 ? L"catalog: Export-StartLayout cancelled" : L"catalog: Export-StartLayout timed out");
        return {};
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::wstring text;
    if (code == 0)
        text = ReadUtf8File(file);
    else
        Log(L"catalog: Export-StartLayout failed (exit code %lu)", code);
    DeleteFileW(file.c_str());
    return text;
}

std::wstring ReadJsonString(const std::wstring& j, size_t& i)
{
    std::wstring s;
    for (++i; i < j.size() && j[i] != L'"'; ++i) {
        if (j[i] != L'\\' || i + 1 >= j.size()) {
            s += j[i];
            continue;
        }
        const wchar_t e = j[++i];
        if (e == L'u' && i + 4 < j.size()) {
            s += wchar_t(std::wcstoul(j.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
        } else {
            s += e == L'n' ? L'\n' : e == L't' ? L'\t' : e == L'r' ? L'\r' : e == L'b' ? L'\b' : e == L'f' ? L'\f' : e;
        }
    }
    ++i; // the closing quote
    return s;
}

std::vector<std::pair<std::wstring, std::wstring>> JsonStringPairs(const std::wstring& j)
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    auto skipSpace = [&](size_t& i) {
        while (i < j.size() && iswspace(j[i]))
            ++i;
    };
    for (size_t i = 0; i < j.size();) {
        if (j[i] != L'"') {
            ++i;
            continue;
        }
        std::wstring key = ReadJsonString(j, i);
        skipSpace(i);
        if (i >= j.size() || j[i] != L':')
            continue;
        ++i;
        skipSpace(i);
        if (i < j.size() && j[i] == L'"')
            out.emplace_back(std::move(key), ReadJsonString(j, i));
    }
    return out;
}

std::wstring ShellDisplayName(const std::wstring& parsingPath)
{
    std::wstring name;
    winrt::com_ptr<IShellItem> item;
    PWSTR s = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(parsingPath.c_str(), nullptr, IID_PPV_ARGS(item.put()))) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s)
        name = s;
    CoTaskMemFree(s);
    return name;
}

bool ResolveDesktopAppId(const std::wstring& value, const std::vector<CatalogApp>& apps, CatalogApp& out)
{
    auto it = std::ranges::find_if(apps, [&](const CatalogApp& a) { return SameText(a.aumid, value) || SameText(a.id, value); });
    if (it != apps.end()) {
        out = *it;
        return true;
    }
    if (value.starts_with(L"::{")) {
        const std::wstring n = ShellDisplayName(L"shell:" + value);
        out = {n.empty() ? value : n, value, {}, {}, {}};
        return true;
    }
    std::wstring expanded;
    if (IsPath(value)) {
        expanded = value;
    } else if (const size_t slash = value.find(L'\\'); value.starts_with(L"{") && slash != std::wstring::npos) {
        GUID kf{};
        PWSTR p = nullptr;
        if (SUCCEEDED(CLSIDFromString(value.substr(0, slash).c_str(), &kf)) && SUCCEEDED(SHGetKnownFolderPath(kf, 0, nullptr, &p)) && p)
            expanded = std::wstring(p) + value.substr(slash);
        CoTaskMemFree(p);
    }
    if (expanded.empty())
        return false;
    if (auto bt = std::ranges::find_if(apps, [&](const CatalogApp& a) { return SameText(a.target, expanded); }); bt != apps.end()) {
        out = *bt;
        return true;
    }
    if (GetFileAttributesW(expanded.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const std::wstring name = ShellDisplayName(expanded);
    const std::wstring f = expanded.substr(expanded.find_last_of(L"\\/") + 1);
    out = {name.empty() ? (f.size() > 4 ? f.substr(0, f.size() - 4) : f) : name, expanded, {}, expanded, {}};
    return true;
}

bool ResolveDesktopAppLink(const std::wstring& value, const std::vector<CatalogApp>& apps, CatalogApp& out)
{
    std::wstring expanded = ExpandKnownEnvFolders(value);
    // Start layouts exported before the rename still refer to this shortcut.
    constexpr std::wstring_view kLegacy = L"\\Atrium.Core.lnk";
    if (GetFileAttributesW(expanded.c_str()) == INVALID_FILE_ATTRIBUTES && expanded.size() >= kLegacy.size() &&
        SameText(expanded.substr(expanded.size() - kLegacy.size()), std::wstring(kLegacy)))
        expanded.replace(expanded.size() - kLegacy.size(), kLegacy.size(), L"\\Atrium.lnk");
    if (GetFileAttributesW(expanded.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"catalog: start pin %s not found", value.c_str());
        return false;
    }
    CatalogApp link;
    link.id = expanded;
    winrt::com_ptr<IShellItem2> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(expanded.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
        PWSTR s = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &s)) && s)
            link.name = s; // localised ("File Explorer.lnk" shows as 文件资源管理器)
        CoTaskMemFree(s);
        link.aumid = PropString(item.get(), PKEY_AppUserModel_ID);
        link.target = PropString(item.get(), PKEY_Link_TargetParsingPath);
        link.args = PropString(item.get(), PKEY_Link_Arguments);
    }
    if (link.name.empty()) {
        const std::wstring file = link.id.substr(link.id.find_last_of(L"\\/") + 1);
        link.name = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
    }
    auto it = std::ranges::find_if(apps, [&](const CatalogApp& a) { return SameText(a.aumid, link.aumid) || SameText(a.id, link.aumid); });
    if (it == apps.end())
        it = std::ranges::find_if(apps, [&](const CatalogApp& a) { return SameText(a.target, link.target) && SameText(a.name, link.name); });
    out = it != apps.end() ? *it : link;
    return true;
}

std::vector<CatalogApp> ResolvePins(const std::vector<std::pair<std::wstring, std::wstring>>& layout, const std::vector<CatalogApp>& apps)
{
    std::vector<CatalogApp> out;
    auto add = [&](const CatalogApp& a) {
        if (std::ranges::none_of(out, [&](const CatalogApp& o) { return SameText(o.id, a.id); }))
            out.push_back(a);
    };
    for (const auto& [key, value] : layout) {
        if (key == L"packagedAppId") {
            auto it = std::ranges::find_if(apps, [&](const CatalogApp& a) { return SameText(a.aumid, value) || SameText(a.id, value); });
            if (it != apps.end())
                add(*it);
            else
                Log(L"catalog: start pin %s is not installed", value.c_str());
        } else if (key == L"desktopAppId") {
            if (CatalogApp app; ResolveDesktopAppId(value, apps, app))
                add(app);
        } else if (key == L"desktopAppLink") {
            if (CatalogApp app; ResolveDesktopAppLink(value, apps, app))
                add(app);
        }
    }
    return out;
}

std::vector<CatalogFolder> ResolvePinGroups(const std::vector<StartPinGroup>& groups, const std::vector<CatalogApp>& apps)
{
    std::vector<CatalogFolder> out;
    std::vector<std::wstring> seen;
    for (const StartPinGroup& g : groups) {
        std::vector<std::pair<std::wstring, std::wstring>> pairs;
        pairs.reserve(g.items.size());
        for (const StartPinItem& it : g.items) {
            const wchar_t* kind = it.kind == StartPinItem::Kind::PackagedAppId  ? L"packagedAppId" :
                                  it.kind == StartPinItem::Kind::DesktopAppLink ? L"desktopAppLink" :
                                                                                  L"desktopAppId";
            pairs.emplace_back(kind, it.value);
        }
        auto resolved = ResolvePins(pairs, apps);
        std::erase_if(resolved, [&](const CatalogApp& a) {
            if (std::ranges::any_of(seen, [&](const std::wstring& id) { return SameText(id, a.id); }))
                return true;
            seen.push_back(a.id);
            return false;
        });
        const bool isFolder = (g.folder || !g.name.empty()) && resolved.size() >= 2;
        if (!resolved.empty())
            out.push_back({isFolder ? g.name : L"", std::move(resolved), isFolder});
    }
    return out;
}

using FolderLinksList = std::vector<std::pair<std::wstring, std::vector<std::pair<std::wstring, std::wstring>>>>;

void ScanProgramsDir(const KNOWNFOLDERID& id, FolderLinksList& found)
{
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &root)) || !root) {
        CoTaskMemFree(root);
        return;
    }
    const std::wstring programs = root;
    CoTaskMemFree(root);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((programs + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L".." || IsSystemStartFolder(Fold(name)))
            continue;
        const std::wstring dir = programs + L"\\" + name;
        std::vector<std::pair<std::wstring, std::wstring>> links;
        CollectStartLinks(dir, links);
        if (links.empty())
            continue;
        std::wstring shown = ShellDisplayName(dir);
        if (shown.empty())
            shown = name;
        auto it = std::ranges::find_if(found, [&](const auto& f) { return Fold(f.first) == Fold(shown); });
        if (it == found.end())
            found.emplace_back(std::move(shown), std::move(links));
        else
            it->second.insert(it->second.end(), links.begin(), links.end());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

std::wstring ExpandKnownEnvFolders(const std::wstring& value)
{
    static const std::pair<const wchar_t*, const KNOWNFOLDERID*> kFolders[] = {
        {L"%ALLUSERSPROFILE%", &FOLDERID_ProgramData}, {L"%PROGRAMDATA%", &FOLDERID_ProgramData},
        {L"%APPDATA%", &FOLDERID_RoamingAppData},      {L"%LOCALAPPDATA%", &FOLDERID_LocalAppData},
        {L"%USERPROFILE%", &FOLDERID_Profile},         {L"%PUBLIC%", &FOLDERID_Public},
        {L"%PROGRAMFILES%", &FOLDERID_ProgramFiles},   {L"%PROGRAMFILES(X86)%", &FOLDERID_ProgramFilesX86},
        {L"%SYSTEMROOT%", &FOLDERID_Windows},          {L"%WINDIR%", &FOLDERID_Windows},
    };
    std::wstring out = value;
    for (const auto& [token, id] : kFolders) {
        const size_t len = wcslen(token);
        if (out.size() < len || CompareStringOrdinal(out.c_str(), int(len), token, int(len), TRUE) != CSTR_EQUAL)
            continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path)))
            out = std::wstring(path) + out.substr(len);
        CoTaskMemFree(path);
        break;
    }
    wchar_t expanded[MAX_PATH * 2] = {};
    if (out.find(L'%') != std::wstring::npos && ExpandEnvironmentStringsW(out.c_str(), expanded, DWORD(std::size(expanded))))
        out = expanded;
    return out;
}

void CollectStartLinks(const std::wstring& dir, std::vector<std::pair<std::wstring, std::wstring>>& links, int depth)
{
    if (depth >= 8)
        return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::wstring> subdirs;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth + 1 < 8 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                subdirs.push_back(dir + L"\\" + name);
        } else {
            const std::wstring folded = Fold(name);
            if ((name.size() > 4 && folded.ends_with(L".lnk")) || (name.size() > 10 && folded.ends_with(L".appref-ms")))
                links.emplace_back(L"desktopAppLink", dir + L"\\" + name);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const std::wstring& sub : subdirs)
        CollectStartLinks(sub, links, depth + 1);
}

bool IsSystemStartFolder(const std::wstring& foldedName)
{
    return std::ranges::any_of(kSystemFolders, [&](const wchar_t* s) { return foldedName == s; });
}

bool IsToolStartFolder(const std::wstring& foldedName)
{
    return std::ranges::any_of(kToolFolders, [&](const wchar_t* s) { return foldedName == s; });
}

bool IsStartupStartFolder(const std::wstring& foldedName) { return foldedName == L"startup" || foldedName == L"启动"; }

bool NotAnAppShortcut(const CatalogApp& app)
{
    const std::wstring name = Fold(app.name);
    const std::wstring target = Fold(app.target.substr(app.target.find_last_of(L"\\/") + 1));
    if (name.find(L"uninstall") != std::wstring::npos || name.find(L"卸载") != std::wstring::npos || name.find(L"解除安装") != std::wstring::npos ||
        target.starts_with(L"unins"))
        return true;
    if (target.empty())
        return false;
    constexpr const wchar_t* kLaunchExts[] = {L".exe", L".bat", L".cmd", L".com", L".jar", L".appref-ms"};
    return std::ranges::none_of(kLaunchExts, [&](const wchar_t* ext) { return target.ends_with(ext); });
}

std::vector<CatalogFolder> ReadStartMenuFolders(const std::vector<CatalogApp>& apps)
{
    FolderLinksList found;
    ScanProgramsDir(FOLDERID_CommonPrograms, found);
    ScanProgramsDir(FOLDERID_Programs, found);
    std::vector<CatalogFolder> out;
    for (auto& [name, links] : found) {
        CatalogFolder folder{name, {}};
        for (CatalogApp& app : ResolvePins(links, apps)) {
            if (!NotAnAppShortcut(app))
                folder.apps.push_back(std::move(app));
        }
        if (folder.apps.size() >= 2)
            out.push_back(std::move(folder));
    }
    return out;
}

bool AppCatalog::StartMenuUsesXml() const { return !IsWindows11(); }

void AppCatalog::RefreshStartLayout(bool force)
{
    const bool win11 = IsWindows11();
    FILETIME stamp{};
    const bool hasStamp = StartLayoutStamp(win11, stamp);
    if (layoutLoaded_ && !force && (!hasStamp || CompareFileTime(&stamp, &layoutTime_) == 0))
        return;
    const ULONGLONG t1 = GetTickCount64();
    bool cancelled = false;
    const std::wstring text = ExportStartLayout(stopEvent_, !win11, &cancelled);
    if (cancelled)
        return;
    if (text.find(win11 ? L"\"pinnedList\"" : L"LayoutModificationTemplate") != std::wstring::npos) {
        if (win11)
            layout_ = JsonStringPairs(text);
        else
            xmlLayout_ = ParseStartLayoutXml(text);
        layoutTime_ = stamp;
        layoutLoaded_ = true;
    }
    Log(L"catalog: start layout exported (%zu entries) in %llu ms", win11 ? layout_.size() : xmlLayout_.size(), GetTickCount64() - t1);
}

std::vector<CatalogApp> AppCatalog::ResolveStartLayoutPins(const std::vector<CatalogApp>& apps, std::vector<CatalogFolder>& pinGroups) const
{
    if (IsWindows11()) {
        pinGroups.clear();
        return ResolvePins(layout_, apps);
    }
    pinGroups = ResolvePinGroups(xmlLayout_, apps);
    std::vector<CatalogApp> flat;
    for (const CatalogFolder& g : pinGroups)
        flat.insert(flat.end(), g.apps.begin(), g.apps.end());
    return flat;
}

} // namespace atrium
