#include "settings.h"

#include "localization.h"
#include "localization_format.h"
#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <tuple>
#include <vector>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

struct ScopedReadHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ScopedReadHandle(const std::wstring& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr))
    {
    }
    ScopedReadHandle(const ScopedReadHandle&) = delete;
    ScopedReadHandle& operator=(const ScopedReadHandle&) = delete;
    ~ScopedReadHandle()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
};

bool ReadSharedFileBytes(const std::wstring& path, std::string& bytes, DWORD* errorOut = nullptr)
{
    bytes.clear();
    const ScopedReadHandle file(path);
    if (!file) {
        if (errorOut)
            *errorOut = GetLastError();
        return false;
    }
    std::array<char, 8192> buf{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file.handle, buf.data(), DWORD(buf.size()), &read, nullptr)) {
            if (errorOut)
                *errorOut = GetLastError();
            bytes.clear();
            return false;
        }
        if (read == 0)
            break;
        bytes.append(buf.data(), read);
    }
    return true;
}

bool ReadRawFile(const std::wstring& path, std::string& bytes, bool& exists)
{
    bytes.clear();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        exists = err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND;
        return !exists;
    }
    exists = true;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    DWORD err = ERROR_SUCCESS;
    if (ReadSharedFileBytes(path, bytes, &err))
        return true;
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        exists = false;
        return true;
    }
    return false;
}

std::wstring Utf8ToWideText(std::string bytes)
{
    if (bytes.size() >= 3 && bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3); // UTF-8 BOM, e.g. saved by Notepad or Windows PowerShell
    if (bytes.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), w.data(), n);
    return w;
}

void InspectSettingsFile(const std::wstring& path, SettingsFileState& state, double& fileVersion, Settings& parsed, bool& readError)
{
    state = SettingsFileState::Missing;
    fileVersion = 0;
    parsed = Settings{};
    readError = false;
    std::string bytes;
    bool exists = false;
    if (!ReadRawFile(path, bytes, exists)) {
        readError = true;
        state = SettingsFileState::Corrupt;
        return;
    }
    if (!exists)
        return;
    if (bytes.size() >= 3 && bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);
    if (bytes.empty())
        return;
    const std::wstring text = Utf8ToWideText(std::move(bytes));
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o)) {
        state = SettingsFileState::Corrupt;
        return;
    }
    if (o.HasKey(L"version") && o.GetNamedValue(L"version").ValueType() == json::JsonValueType::Number)
        fileVersion = o.GetNamedNumber(L"version");
    Settings::FromJson(text, Settings{}, parsed);
    // The system taskbar is hidden by default for a new installation only: a settings file from before the
    // key existed keeps it shown, as it was.
    if (!o.HasKey(L"hideTaskbar"))
        parsed.hideTaskbar = false;
    state = fileVersion > Settings::kVersion ? SettingsFileState::FutureVersion : SettingsFileState::Valid;
}

bool ParseBackupSuffix(std::wstring_view middle, std::wstring& stamp, int& seq)
{
    if (middle.size() < 15 || middle[8] != L'-')
        return false;
    for (size_t i = 0; i < 15; ++i) {
        if (i == 8)
            continue;
        if (middle[i] < L'0' || middle[i] > L'9')
            return false;
    }
    stamp.assign(middle.substr(0, 15));
    seq = 0;
    if (middle.size() == 15)
        return true;
    if (middle[15] != L'.' || middle.size() == 16)
        return false;
    int value = 0;
    for (size_t i = 16; i < middle.size(); ++i) {
        if (middle[i] < L'0' || middle[i] > L'9' || value > 100000)
            return false;
        value = value * 10 + (middle[i] - L'0');
    }
    seq = value;
    return true;
}

struct CorruptBackupEntry {
    std::wstring stamp;
    int seq = 0;
    std::filesystem::path path;
};

std::vector<CorruptBackupEntry> FindCorruptBackups(const std::filesystem::path& dir, const std::wstring& stem, const std::wstring& ext)
{
    const std::wstring prefix = stem + L".corrupt.";
    std::vector<CorruptBackupEntry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !item.is_regular_file(ec))
            continue;
        const std::wstring name = item.path().filename().wstring();
        if (name.size() <= prefix.size() + ext.size())
            continue;
        if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0)
            continue;
        if (_wcsicmp(name.c_str() + name.size() - ext.size(), ext.c_str()) != 0)
            continue;
        std::wstring stamp;
        int seq = 0;
        const std::wstring_view mid(name.data() + prefix.size(), name.size() - prefix.size() - ext.size());
        if (ParseBackupSuffix(mid, stamp, seq))
            entries.push_back({std::move(stamp), seq, item.path()});
    }
    std::ranges::sort(entries, [](const CorruptBackupEntry& a, const CorruptBackupEntry& b) { return std::tie(a.stamp, a.seq) < std::tie(b.stamp, b.seq); });
    return entries;
}

bool FilesHaveSameBytes(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    const auto sizeA = std::filesystem::file_size(a, ec);
    if (ec)
        return false;
    const auto sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB)
        return false;
    const ScopedReadHandle fa(a.wstring()), fb(b.wstring());
    if (!fa || !fb)
        return false;
    std::array<char, 4096> bufA{}, bufB{};
    for (;;) {
        DWORD nA = 0, nB = 0;
        if (!ReadFile(fa.handle, bufA.data(), DWORD(bufA.size()), &nA, nullptr) || !ReadFile(fb.handle, bufB.data(), DWORD(bufB.size()), &nB, nullptr))
            return false;
        if (nA != nB || std::memcmp(bufA.data(), bufB.data(), size_t(nA)) != 0)
            return false;
        if (nA == 0)
            return true;
    }
}

void PruneCorruptBackups(const std::filesystem::path& dir, const std::wstring& stem, const std::wstring& ext, int maxBackups)
{
    if (maxBackups < 1)
        return;
    const auto entries = FindCorruptBackups(dir, stem, ext);
    if (entries.size() <= size_t(maxBackups))
        return;
    const size_t removeCount = entries.size() - size_t(maxBackups);
    for (size_t i = 0; i < removeCount; ++i)
        DeleteFileW(entries[i].path.c_str());
}

std::wstring FormatSystemError(DWORD code)
{
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring reason = text ? text : L"";
    LocalFree(text);
    while (!reason.empty() && (reason.back() == L'\n' || reason.back() == L'\r' || reason.back() == L' '))
        reason.pop_back();
    return reason;
}

struct DataDirState {
    bool initialized = false;
    std::wstring dir;
    bool overridden = false;
};

std::mutex g_dataDirMutex;
DataDirState g_dataDirState;

DataDirState ResolveDataDirState()
{
    std::lock_guard g(g_dataDirMutex);
    if (g_dataDirState.initialized)
        return g_dataDirState;
    DataDirState state;
    if (IsDevelopmentBuild()) {
        int count = 0;
        auto* argv = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::wstring> args(argv, argv + (argv ? count : 0));
        LocalFree(static_cast<HLOCAL>(argv));
        std::wstring custom;
        bool specified = false;
        if (!ParseDataDirArgument(args, custom, specified)) {
            WriteDebugMessage(L"atrium: invalid --data-dir argument; expected a fully qualified absolute path\n");
            ExitProcess(2);
        }
        if (specified) {
            state.dir = std::move(custom);
            state.overridden = true;
        }
    }
    if (state.dir.empty()) {
        PWSTR base = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
            state.dir = std::wstring(base) + L"\\Atrium";
        CoTaskMemFree(base);
    }
    if (!state.dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(state.dir, ec);
    }
    state.initialized = true;
    g_dataDirState = state;
    return g_dataDirState;
}

} // namespace

bool ParseDataDirArgument(const std::vector<std::wstring>& args, std::wstring& outDir, bool& specified)
{
    outDir.clear();
    specified = false;
    auto fail = [&] {
        outDir.clear();
        return false;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] != L"--data-dir")
            continue;
        specified = true;
        if (i + 1 >= args.size() || args[i + 1].empty())
            return fail();
        const std::filesystem::path rawPath(args[++i]);
        if (!rawPath.is_absolute())
            return fail();
        const std::wstring rootName = rawPath.root_name().wstring();
        if (rootName.starts_with(L"\\\\") && rootName != L"\\\\?" && rootName != L"\\\\." && !rawPath.has_relative_path())
            return fail();
        const std::filesystem::path normalPath = rawPath.lexically_normal();
        std::wstring normalized = normalPath.wstring();
        const std::wstring rootPath = normalPath.root_path().wstring();
        while (normalized.size() > rootPath.size() && (normalized.back() == L'\\' || normalized.back() == L'/'))
            normalized.pop_back();
        outDir = std::move(normalized);
    }
    return true;
}

std::wstring DataDirectory() { return ResolveDataDirState().dir; }

std::wstring DataDirectoryArgs()
{
    const DataDirState state = ResolveDataDirState();
    if (!state.overridden)
        return {};
    std::wstring escaped = state.dir;
    const size_t tail = escaped.find_last_not_of(L'\\');
    escaped.append(tail == std::wstring::npos ? escaped.size() : escaped.size() - 1 - tail, L'\\');
    return L" --development --data-dir \"" + escaped + L"\"";
}

std::wstring SettingsPath() { return DataDirectory() + L"\\settings.json"; }

std::wstring ReadTextFile(const std::wstring& path)
{
    std::string bytes;
    if (!ReadSharedFileBytes(path, bytes))
        return {};
    return Utf8ToWideText(std::move(bytes));
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    static std::atomic<uint64_t> seq{0};
    const std::wstring tmp =
        path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(seq.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    // Win32 rather than a stream: a failure leaves its reason in GetLastError.
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        SetLastError(error != ERROR_SUCCESS ? error : ERROR_WRITE_FAULT);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(tmp.c_str());
        SetLastError(moveError);
        return false;
    }
    return true;
}

bool BackupCorruptSettings(const std::wstring& path, int maxBackups)
{
    const std::filesystem::path filePath(path);
    const std::filesystem::path dir = filePath.has_parent_path() ? filePath.parent_path() : std::filesystem::path(L".");
    std::wstring stem = filePath.stem().wstring();
    if (stem.empty())
        stem = L"settings";
    std::wstring ext = filePath.extension().wstring();
    if (ext.empty())
        ext = L".json";
    const auto existing = FindCorruptBackups(dir, stem, ext);
    if (!existing.empty() && FilesHaveSameBytes(filePath, existing.back().path))
        return true;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (int i = 0; i < 100; ++i) {
        const std::wstring suffix = i == 0 ? L"" : L"." + std::to_wstring(i);
        const std::filesystem::path candidate = dir / (stem + L".corrupt." + stamp + suffix + ext);
        if (CopyFileW(path.c_str(), candidate.c_str(), TRUE)) {
            PruneCorruptBackups(dir, stem, ext, maxBackups);
            return true;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS)
            return false;
    }
    return false;
}

Settings LoadSettingsFrom(const std::wstring& path, SettingsFileState* stateOut, bool* backupOkOut)
{
    SettingsFileState state = SettingsFileState::Missing;
    double fileVersion = 0;
    Settings s;
    bool readError = false;
    InspectSettingsFile(path, state, fileVersion, s, readError);
    bool backupOk = false;
    if (state == SettingsFileState::FutureVersion) {
        Log(L"settings: %s uses newer version %.0f (supported %d); reading compatible fields read-only", path.c_str(), fileVersion, Settings::kVersion);
    } else if (state == SettingsFileState::Corrupt) {
        backupOk = !readError && BackupCorruptSettings(path, 3);
        Log(L"settings: %s is not valid JSON (%s), using defaults", path.c_str(), backupOk ? L"backed up" : L"backup failed");
        s = Settings{};
    }
    if (stateOut)
        *stateOut = state;
    if (backupOkOut)
        *backupOkOut = backupOk;
    return s;
}

Settings LoadSettings() { return LoadSettingsFrom(SettingsPath()); }

bool IsDevelopmentBuild()
{
#ifdef ATRIUM_DEVELOPMENT
    return true;
#else
    static const bool kDevelopment = [] {
        int count = 0;
        auto* args = CommandLineToArgvW(GetCommandLineW(), &count);
        bool explicitDevelopment = false;
        for (int i = 1; args && i < count; ++i)
            explicitDevelopment |= wcscmp(args[i], L"--development") == 0;
        LocalFree(static_cast<HLOCAL>(args));
        wchar_t exe[32768]{};
        GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        std::wstring path(exe);
        path = path.substr(0, path.find_last_of(L'\\')) + L"\\development.txt";
        return explicitDevelopment || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }();
    return kDevelopment;
#endif
}

bool LowIntegrity()
{
    static const bool kLow = [] {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        std::vector<BYTE> buffer(size);
        bool below = false;
        if (size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
            const DWORD rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
            below = rid < SECURITY_MANDATORY_MEDIUM_RID;
        }
        CloseHandle(token);
        return below;
    }();
    return kLow;
}

bool SaveSettingsTo(const std::wstring& path, const Settings& s, std::wstring* error, const std::function<bool(const std::wstring&)>& backupCorrupt,
                    bool* startupChangedOut)
{
    if (error)
        error->clear();
    if (startupChangedOut)
        *startupChangedOut = false;

    SettingsFileState state = SettingsFileState::Missing;
    double fileVersion = 0;
    Settings existing;
    bool readError = false;
    InspectSettingsFile(path, state, fileVersion, existing, readError);
    if (state == SettingsFileState::FutureVersion) {
        Log(L"settings: refused to overwrite %s (version %.0f > %d)", path.c_str(), fileVersion, Settings::kVersion);
        if (error)
            *error = TrFormat(L"Could not save %s: settings file uses a newer format version (v%.0f > v%d). Update Atrium before saving.", path.c_str(),
                              fileVersion, Settings::kVersion);
        return false;
    }
    if (state == SettingsFileState::Corrupt) {
        const bool backedUp = !readError && (backupCorrupt ? backupCorrupt(path) : BackupCorruptSettings(path, 3));
        if (!backedUp) {
            Log(L"settings: could not back up corrupt %s before saving; original kept", path.c_str());
            if (error)
                *error = TrFormat(L"Could not back up corrupt settings file %s before saving; original file was kept.", path.c_str());
            return false;
        }
        existing = Settings{};
    }

    const bool startupChanged = existing.startOnLogin != s.startOnLogin || (s.startOnLogin && existing.startupMode != s.startupMode);
    if (!WriteTextFile(path, s.ToJson())) {
        const DWORD code = GetLastError();
        const std::wstring reason = FormatSystemError(code);
        Log(L"settings: could not write %s (%lu)%s", path.c_str(), code, LowIntegrity() ? L", running at low integrity" : L"");
        if (error) {
            *error = TrFormat(L"Could not write %s: %s (error %lu)", path.c_str(), reason.c_str(), code);
            // Below medium integrity (an executable carrying a Low label) nothing in the profile is writable.
            if (LowIntegrity())
                *error += Tr(
                    L"\nAtrium is running at low integrity and cannot write to your profile. Unzip it to an ordinary folder (such as D:\\\\Atrium), make sure Atrium.exe has no Low integrity label, then start it again.");
        }
        return false;
    }
    if (startupChangedOut)
        *startupChangedOut = startupChanged;
    return true;
}

bool SaveSettings(const Settings& s, std::wstring* error, bool* startupChangedOut) { return SaveSettingsTo(SettingsPath(), s, error, {}, startupChangedOut); }

} // namespace atrium
