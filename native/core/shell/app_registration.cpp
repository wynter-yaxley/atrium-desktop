#include "shell/app_registration.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>

#include <winrt/base.h>

namespace atrium {

namespace {

std::wstring ReadRegString(HKEY key, const wchar_t* name)
{
    wchar_t value[1024]{};
    DWORD bytes = sizeof(value);
    return key && RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS ? std::wstring(value) : std::wstring();
}

std::wstring SyncUninstallRegistry(const std::wstring& directory, const std::wstring& exe)
{
    constexpr auto kRegistration = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Atrium";
    const std::pair<const wchar_t*, std::wstring> fields[] = {
        {L"DisplayName", L"Atrium"},
        {L"Publisher", L"Atrium contributors"},
        {L"InstallLocation", directory},
        {L"DisplayIcon", L"\"" + exe + L"\",0"},
        {L"UninstallString", L"\"" + exe + L"\" --uninstall"},
        {L"URLInfoAbout", L"https://github.com/wynter-yaxley/atrium-desktop"},
    };
    HKEY key = nullptr;
    bool changed = RegOpenKeyExW(HKEY_CURRENT_USER, kRegistration, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS;
    std::wstring previousDirectory = ReadRegString(key, L"InstallLocation");
    if (key) {
        for (const auto& [name, value] : fields)
            changed |= ReadRegString(key, name) != value;
        for (const wchar_t* name : {L"NoModify", L"NoRepair"}) {
            DWORD value = 0, bytes = sizeof(value);
            changed |= RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS || value != 1;
        }
        DWORD bytes = 0;
        changed |= RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS;
        RegCloseKey(key);
        key = nullptr;
    }
    if (!changed)
        return previousDirectory;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistration, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        bool ok = true;
        for (const auto& [name, value] : fields)
            ok &= RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t))) ==
                  ERROR_SUCCESS;
        RegDeleteValueW(key, L"DisplayVersion");
        const DWORD one = 1;
        for (const wchar_t* name : {L"NoModify", L"NoRepair"})
            ok &= RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one)) == ERROR_SUCCESS;
        RegCloseKey(key);
        Log(ok ? L"app: registered Atrium in Installed apps (%s)" : L"app: could not write all application information (%s)", directory.c_str());
    } else {
        Log(L"app: could not register Atrium in Installed apps");
    }
    return previousDirectory;
}

void MigrateLegacyShortcut(IShellLinkW& link, const std::wstring& legacy, const std::wstring& directory, const std::wstring& exe)
{
    // Migrate the old shortcut only when it belongs to this same distribution.
    winrt::com_ptr<IPersistFile> file;
    if (FAILED(link.QueryInterface(IID_PPV_ARGS(file.put()))) || FAILED(file->Load(legacy.c_str(), STGM_READ)))
        return;
    wchar_t target[MAX_PATH]{};
    link.GetPath(target, MAX_PATH, nullptr, SLGP_RAWPATH);
    if (_wcsicmp(target, (directory + L"\\Atrium.Core.exe").c_str()) == 0 || _wcsicmp(target, exe.c_str()) == 0) {
        if (DeleteFileW(legacy.c_str()))
            SHChangeNotify(SHCNE_DELETE, SHCNF_PATHW, legacy.c_str(), nullptr);
    }
}

void SyncStartMenuShortcut(const std::wstring& directory, const std::wstring& exe, const std::wstring& previousDirectory)
{
    PWSTR programs = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs);
    std::wstring shortcut = programs ? std::wstring(programs) + L"\\Atrium.lnk" : L"";
    std::wstring legacy = programs ? std::wstring(programs) + L"\\Atrium.Core.lnk" : L"";
    CoTaskMemFree(programs);
    winrt::com_ptr<IShellLinkW> link;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()));
    bool existing = SUCCEEDED(hr) && SUCCEEDED(link.as<IPersistFile>()->Load(shortcut.c_str(), STGM_READ));
    wchar_t oldTarget[MAX_PATH]{}, workingDirectory[MAX_PATH]{}, iconPath[MAX_PATH]{};
    int iconIndex = 0;
    if (existing) {
        link->GetPath(oldTarget, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (_wcsicmp(oldTarget, exe.c_str()) == 0)
            return; // leave the user's shortcut exactly as it is
        bool ours = (!previousDirectory.empty() && (_wcsicmp(oldTarget, (previousDirectory + L"\\Atrium.exe").c_str()) == 0 ||
                                                    _wcsicmp(oldTarget, (previousDirectory + L"\\Atrium.Core.exe").c_str()) == 0)) ||
                    _wcsicmp(oldTarget, (directory + L"\\Atrium.Core.exe").c_str()) == 0;
        if (!ours)
            return;
        hr = link.as<IPersistFile>()->Load(shortcut.c_str(), STGM_READWRITE);
        link->GetWorkingDirectory(workingDirectory, MAX_PATH);
        link->GetIconLocation(iconPath, MAX_PATH, &iconIndex);
    }
    if (SUCCEEDED(hr))
        hr = link->SetPath(exe.c_str());
    if (SUCCEEDED(hr) && (!existing || !*workingDirectory || _wcsicmp(workingDirectory, previousDirectory.c_str()) == 0))
        hr = link->SetWorkingDirectory(directory.c_str());
    if (SUCCEEDED(hr) && (!existing || !*iconPath || _wcsicmp(iconPath, oldTarget) == 0))
        hr = link->SetIconLocation(exe.c_str(), existing ? iconIndex : 0);
    if (SUCCEEDED(hr)) {
        auto properties = link.as<IPropertyStore>();
        PROPVARIANT id{};
        hr = InitPropVariantFromString(kAppId, &id);
        if (SUCCEEDED(hr))
            hr = properties->SetValue(PKEY_AppUserModel_ID, id);
        PropVariantClear(&id);
        if (SUCCEEDED(hr))
            hr = properties->Commit();
    }
    if (SUCCEEDED(hr))
        hr = link.as<IPersistFile>()->Save(shortcut.c_str(), TRUE);
    if (FAILED(hr)) {
        Log(L"app: could not register the start menu shortcut (0x%08X)", unsigned(hr));
        return;
    }
    SHChangeNotify(existing ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    MigrateLegacyShortcut(*link, legacy, directory, exe);
}

} // namespace

bool IsRegisteredApplication(const std::wstring& directory)
{
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Atrium", L"InstallLocation", RRF_RT_REG_SZ, nullptr,
                        value, &bytes) == ERROR_SUCCESS &&
           _wcsicmp(value, directory.c_str()) == 0;
}

void RegisterApplication(const std::wstring& directory)
{
    if (IsDevelopmentBuild())
        return;
    const std::wstring exe = directory + L"\\Atrium.exe";
    const std::wstring uninstall = directory + L"\\Uninstall.ps1";
    if (GetFileAttributesW(uninstall.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((directory + L"\\Settings\\Atrium.Settings.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // a native-only development build is not an installed app

    const std::wstring previousDirectory = SyncUninstallRegistry(directory, exe);
    SyncStartMenuShortcut(directory, exe, previousDirectory);
}

} // namespace atrium
