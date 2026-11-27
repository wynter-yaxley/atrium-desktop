// Jump list files (see jump_list.h): the DestList of an automatic destinations
// file and the categories of a custom destinations file.

#include "shell/jump_list.h"

#include "shell/shell_verbs.h"
#include "log.h"
#include "settings.h"
#include "win32_path.h"

#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace atrium {
namespace {

constexpr BYTE kFooter[4] = {0xAB, 0xFB, 0xBF, 0xBA};
constexpr BYTE kLnkHead[20] = {0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

std::vector<BYTE> ReadFileBytes(const std::wstring& path)
{
    std::vector<BYTE> bytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (const DWORD err = GetLastError(); err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            Log(L"jump: CreateFileW failed (%lu)", err);
        return bytes;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 24)) {
        bytes.resize(size_t(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &got, nullptr)) {
            Log(L"jump: ReadFile failed (%lu)", GetLastError());
            bytes.clear();
        } else
            bytes.resize(got);
    }
    CloseHandle(file);
    return bytes;
}

std::vector<BYTE> ReadStream(IStorage* storage, const wchar_t* name)
{
    std::vector<BYTE> bytes;
    winrt::com_ptr<IStream> stream;
    if (const HRESULT hr = storage->OpenStream(name, nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, stream.put()); FAILED(hr) || !stream) {
        if (FAILED(hr) && hr != STG_E_FILENOTFOUND)
            Log(L"jump: OpenStream(%s) failed (0x%08X)", name, unsigned(hr));
        return bytes;
    }
    STATSTG stat{};
    if (const HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME); FAILED(hr) || stat.cbSize.QuadPart <= 0 || stat.cbSize.QuadPart > (1 << 24)) {
        if (FAILED(hr))
            Log(L"jump: stream Stat(%s) failed (0x%08X)", name, unsigned(hr));
        return bytes;
    }
    // A stream can return less than the requested size: keep reading.
    bytes.resize(size_t(stat.cbSize.QuadPart));
    size_t filled = 0;
    while (filled < bytes.size()) {
        ULONG got = 0;
        if (const HRESULT hr = stream->Read(bytes.data() + filled, ULONG(bytes.size() - filled), &got); FAILED(hr) || got == 0) {
            if (FAILED(hr))
                Log(L"jump: stream Read(%s) failed (0x%08X)", name, unsigned(hr));
            break;
        }
        filled += got;
    }
    bytes.resize(filled);
    return bytes;
}

template<typename T> T Read(const std::vector<BYTE>& bytes, size_t at)
{
    T value{};
    if (at + sizeof(T) <= bytes.size())
        std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

size_t FindBytes(const std::vector<BYTE>& data, size_t from, const BYTE* pattern, size_t size)
{
    if (size == 0 || data.size() < size)
        return std::wstring::npos;
    for (size_t i = from; i + size <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, pattern, size) == 0)
            return i;
    }
    return std::wstring::npos;
}

// PKEY_AppUserModel_ID; the shell's property key headers are kept out of this file.
std::wstring ShellDisplayName(const std::wstring& parsingName, SIGDN kind)
{
    winrt::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(item.put()))) || !item)
        return {};
    PWSTR text = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(kind, &text)) && text) {
        out = text;
        CoTaskMemFree(text);
    }
    return out;
}

std::wstring BaseName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool IsRemoteShare(const std::wstring& path)
{
    size_t offset = 0;
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) {
        if (path.starts_with(L"\\\\?\\") && path.size() >= 8 && _wcsnicmp(path.c_str() + 4, L"UNC\\", 4) == 0)
            return true;
        offset = 4;
    } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return true; // \\server\share
    }
    if (path.size() >= offset + 3 && path[offset + 1] == L':' && (path[offset + 2] == L'\\' || path[offset + 2] == L'/')) {
        const wchar_t drive = path[offset];
        if ((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) {
            const wchar_t root[4] = {drive, L':', L'\\', 0};
            const UINT type = GetDriveTypeW(root);
            return type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR;
        }
    }
    return false;
}

bool IsDirectory(const std::wstring& path)
{
    if (IsRemoteShare(path))
        return false; // never wait on an unreachable server
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace

// The strings a shell link carries (the menu shows its name, not the file name).
bool ParseLinkStrings(const BYTE* data, size_t size, LinkStrings& out)
{
    if (!data || size < 0x4C)
        return false;
    uint32_t headerSize = 0;
    std::memcpy(&headerSize, data, 4);
    if (headerSize != 0x4C)
        return false;
    uint32_t flags = 0;
    std::memcpy(&flags, data + 20, 4);
    size_t at = 0x4C;
    if (flags & 0x1) { // an item id list comes first, with its own size
        if (at + 2 > size)
            return false;
        uint16_t length = 0;
        std::memcpy(&length, data + at, 2);
        at += 2 + length;
    }
    if (flags & 0x2) { // then the link info block
        if (at + 4 > size)
            return false;
        uint32_t length = 0;
        std::memcpy(&length, data + at, 4);
        at += length;
    }
    const bool unicode = (flags & 0x80) != 0;
    const auto read = [&](std::wstring& into) -> bool {
        if (at + 2 > size)
            return false;
        uint16_t count = 0;
        std::memcpy(&count, data + at, 2);
        at += 2;
        if (unicode) {
            if (at + size_t(count) * 2 > size)
                return false;
            into.assign(reinterpret_cast<const wchar_t*>(data + at), count);
            at += size_t(count) * 2;
        } else {
            if (at + count > size)
                return false;
            into.clear();
            if (count > 0) {
                const char* src = reinterpret_cast<const char*>(data + at);
                if (const int need = MultiByteToWideChar(CP_ACP, 0, src, int(count), nullptr, 0); need > 0) {
                    into.resize(size_t(need));
                    MultiByteToWideChar(CP_ACP, 0, src, int(count), into.data(), need);
                }
            }
            at += count;
        }
        return true;
    };
    return (!(flags & 0x4) || read(out.name)) && (!(flags & 0x8) || read(out.relative)) && (!(flags & 0x10) || read(out.workDir)) &&
           (!(flags & 0x20) || read(out.args));
}

// One entry of the DestList stream (Windows 10/11 layout, version 2 and later).
std::vector<DestEntry> ParseDestList(const std::vector<BYTE>& raw)
{
    std::vector<DestEntry> entries;
    if (raw.size() < 32)
        return entries;
    size_t at = 32;
    while (at + 130 <= raw.size()) {
        short pathChars = Read<short>(raw, at + 128);
        if (pathChars <= 0)
            break;
        const size_t recordSize = 128 + 2 + size_t(pathChars) * 2;
        if (at + recordSize + 4 > raw.size()) {
            Log(L"jump: DestList stops at %zu (chars=%d, %zu bytes)", at, int(pathChars), raw.size());
            break;
        }
        DestEntry entry;
        entry.path.assign(reinterpret_cast<const wchar_t*>(raw.data() + at + 130), size_t(pathChars));
        entry.real = entry.path; // "knownfolder:{GUID}" and "::{GUID}" are shell names
        entry.pin = Read<int>(raw, at + 108);
        entry.last = Read<long long>(raw, at + 100);
        const int store = Read<int>(raw, at + recordSize);
        if (store < 0 || at + recordSize + 4 + size_t(store) > raw.size()) {
            Log(L"jump: DestList stops at %zu (store=%d, %zu bytes)", at, store, raw.size());
            break;
        }
        if (!entry.path.empty())
            entries.push_back(std::move(entry));
        at += recordSize + 4 + size_t(store);
    }
    int pinnedCount = 0;
    for (const DestEntry& entry : entries)
        if (entry.pin > 0)
            ++pinnedCount;
    Log(L"jump: destlist %zu entries, %d pinned", entries.size(), pinnedCount);
    return entries;
}

namespace {

// One category of a custom destinations file.
struct CustomCategory {
    int type = 0; // 0 = custom (named), 1 = known (frequent/recent), 2 = tasks
    int known = 0;
    std::wstring title;
    std::vector<JumpItem> items;
};

// PKEY_Title, declared here so the shell's property key headers stay out.
const PROPERTYKEY kTitle = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};

std::wstring CacheFolder()
{
    const std::wstring dir = DataDirectory();
    return dir.empty() ? std::wstring{} : dir + L"\\cache\\jumplist";
}

void PruneLinkCache(const std::wstring& folder, const std::wstring& keepName)
{
    std::vector<std::pair<ULONGLONG, std::wstring>> files;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((folder + L"\\*.lnk").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            files.emplace_back((static_cast<ULONGLONG>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime, found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (files.size() <= 128)
        return;
    std::ranges::sort(files, [](const auto& a, const auto& b) { return a.first < b.first; });
    size_t toDelete = files.size() - 128;
    for (const auto& [time, name] : files) {
        if (_wcsicmp(name.c_str(), keepName.c_str()) != 0 && DeleteFileW((folder + L"\\" + name).c_str()) && --toDelete == 0)
            break;
    }
}

// The title the system menu shows for a link that carries no name string. The
// link's own property store holds it; the shell reads that format, so the link
// is written once to a cache file and asked for its title.
std::wstring LinkTitleFromShell(const std::vector<BYTE>& bytes)
{
    if (bytes.empty())
        return {};
    const std::wstring folder = CacheFolder();
    if (folder.empty())
        return {};
    ULONGLONG hash = 0xcbf29ce484222325ull;
    for (BYTE byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    wchar_t name[32]{};
    swprintf_s(name, L"%016llX.lnk", hash);
    const std::wstring file = folder + L"\\" + name;
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return {};
        DWORD written = 0;
        WriteFile(handle, bytes.data(), DWORD(bytes.size()), &written, nullptr);
        CloseHandle(handle);
    }
    PruneLinkCache(folder, name);
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(file.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(store.put()))) || !store)
        return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring title;
    if (SUCCEEDED(store->GetValue(kTitle, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        title = value.pwszVal;
    PropVariantClear(&value);
    return title;
}

bool LoadShellLink(const BYTE* data, size_t size, JumpItem& item, bool needTitle = true)
{
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(data, UINT(size)));
    if (!stream)
        return false;
    winrt::com_ptr<IPersistStream> persist;
    winrt::com_ptr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(persist.put()));
    if (SUCCEEDED(hr) && persist)
        hr = persist->Load(stream.get());
    if (SUCCEEDED(hr))
        hr = persist->QueryInterface(IID_PPV_ARGS(link.put()));
    if (FAILED(hr) || !link) {
        Log(L"jump: loading shell link failed (0x%08X)", unsigned(hr));
        return false;
    }
    wchar_t target[1024]{};
    wchar_t args[1024]{};
    if (const HRESULT pathHr = link->GetPath(target, 1024, nullptr, SLGP_RAWPATH); FAILED(pathHr))
        Log(L"jump: ShellLink GetPath failed (0x%08X)", unsigned(pathHr));
    if (const HRESULT argsHr = link->GetArguments(args, 1024); FAILED(argsHr))
        Log(L"jump: ShellLink GetArguments failed (0x%08X)", unsigned(argsHr));
    item.program = target;
    item.args = args;
    LinkStrings strings;
    ParseLinkStrings(data, size, strings);
    item.name = strings.name;
    if (item.name.empty() && needTitle) {
        const std::vector<BYTE> blob(data, data + size);
        item.name = LinkTitleFromShell(blob);
    }
    return true;
}

std::vector<CustomCategory> ParseCustom(const std::vector<BYTE>& raw, bool needTitle = true)
{
    std::vector<CustomCategory> categories;
    if (raw.size() < 24) // 12 byte header + an empty category and its footer
        return categories;
    size_t chunkStart = 0;
    while (chunkStart < raw.size()) {
        const size_t footer = FindBytes(raw, chunkStart, kFooter, 4);
        if (footer == std::wstring::npos)
            break;
        const size_t chunkSize = footer - chunkStart + 4;
        const size_t categoryAt = chunkStart == 0 ? 12 : 0;
        CustomCategory category;
        category.type = Read<int>(raw, chunkStart + categoryAt);
        size_t cursor = categoryAt + 4;
        if (category.type == 0) {
            const short nameChars = Read<short>(raw, chunkStart + cursor);
            cursor += 2;
            if (nameChars > 0 && chunkStart + cursor + size_t(nameChars) * 2 <= raw.size())
                category.title.assign(reinterpret_cast<const wchar_t*>(raw.data() + chunkStart + cursor), size_t(nameChars));
        } else if (category.type == 1) {
            category.known = Read<int>(raw, chunkStart + cursor);
        }
        // The links are shell link records; a 16 byte class id precedes each one.
        size_t at = chunkStart + categoryAt;
        while (true) {
            const size_t start = FindBytes(raw, at, kLnkHead, sizeof(kLnkHead));
            if (start == std::wstring::npos || start >= footer)
                break;
            const size_t next = FindBytes(raw, start + 4, kLnkHead, sizeof(kLnkHead));
            size_t end = (next == std::wstring::npos || next > footer) ? footer : next;
            if (next != std::wstring::npos && next <= footer && end - 16 > start)
                end -= 16;
            JumpItem item;
            if (LoadShellLink(raw.data() + start, end - start, item, needTitle) && !item.program.empty()) {
                if (item.name.empty())
                    item.name = BaseName(item.program);
                item.target = item.program;
                item.folder = false;
                category.items.push_back(std::move(item));
            }
            at = start + 4;
        }
        categories.push_back(std::move(category));
        chunkStart += chunkSize;
    }
    return categories;
}

std::wstring RecentFolder()
{
    PWSTR path = nullptr;
    std::wstring out;
    if (const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &path); SUCCEEDED(hr) && path) {
        out = path;
        CoTaskMemFree(path);
    } else {
        Log(L"jump: SHGetKnownFolderPath(FOLDERID_Recent) failed (0x%08X)", unsigned(hr));
    }
    return out;
}

bool ResolveShellPath(std::wstring& path)
{
    if (!path.starts_with(L"knownfolder:") && !path.starts_with(L"::"))
        return true;
    const std::wstring resolved = ShellDisplayName(path, SIGDN_DESKTOPABSOLUTEPARSING);
    if (resolved.empty())
        return false;
    path = resolved;
    return true;
}

JumpItem ItemForPath(const std::wstring& parsingName)
{
    JumpItem item;
    std::wstring path = parsingName;
    if (path.starts_with(L"knownfolder:") || path.starts_with(L"::")) {
        // A known folder or shell namespace item: the shell knows its real path and name.
        item.name = ShellDisplayName(parsingName, SIGDN_NORMALDISPLAY);
        ResolveShellPath(path);
    }
    if (item.name.empty())
        item.name = BaseName(path);
    item.target = path;
    item.folder = IsDirectory(path);
    return item;
}

// ---------------------------------------------------------------------------
// File index
//
// A jump list file is named after a hash of the AppUserModelID that the shell
// never publishes, so the only way to know which file belongs to which app is
// to read them. That is done once, off the UI thread: the click then looks the
// program up in the index instead of walking the profile.

struct FileIndex {
    std::mutex lock;
    bool ready = false;
    bool building = false;
    ULONGLONG builtAt = 0;
    uint64_t generation = 0;
    std::map<std::wstring, std::wstring> byExe;  // lower-case program -> file hash (empty when ambiguous)
    std::map<std::wstring, std::wstring> byPath; // lower-case item path -> file hash (empty when ambiguous)
};

FileIndex& Index()
{
    static FileIndex index;
    return index;
}

bool IsGenericShellHost(const std::wstring& foldedExe)
{
    const std::wstring name = BaseName(foldedExe);
    static constexpr const wchar_t* kHosts[] = {
        L"explorer.exe", L"cmd.exe", L"powershell.exe", L"pwsh.exe", L"rundll32.exe", L"conhost.exe", L"wscript.exe", L"cscript.exe",
    };
    return std::ranges::any_of(kHosts, [&name](const wchar_t* host) { return name == host; });
}

void RecordIndexHash(std::map<std::wstring, std::wstring>& map, const std::wstring& key, const std::wstring& hash)
{
    if (key.empty() || hash.empty())
        return;
    const auto [it, inserted] = map.emplace(key, hash);
    if (!inserted && !it->second.empty() && it->second != hash)
        it->second.clear(); // multiple jump lists share this key; refuse to guess
}

void IndexCustomDestinations(const std::wstring& recent, const std::atomic<bool>& stop, std::map<std::wstring, std::wstring>& byExe)
{
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((recent + L"\\CustomDestinations\\*.customDestinations-ms").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (stop.load(std::memory_order_relaxed))
            break;
        const std::wstring hash = std::wstring(found.cFileName).substr(0, 16);
        const auto bytes = ReadFileBytes(recent + L"\\CustomDestinations\\" + found.cFileName);
        for (const CustomCategory& category : ParseCustom(bytes, false))
            for (const JumpItem& item : category.items)
                if (!item.program.empty()) {
                    const std::wstring folded = FoldCase(item.program);
                    if (!IsGenericShellHost(folded))
                        RecordIndexHash(byExe, folded, hash);
                }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

void BuildIndex(const std::atomic<bool>& stop)
{
    const std::wstring recent = RecentFolder();
    if (recent.empty())
        return;
    std::map<std::wstring, std::wstring> byExe;
    std::map<std::wstring, std::wstring> byPath;

    IndexCustomDestinations(recent, stop, byExe);
    if (stop.load(std::memory_order_relaxed))
        return;

    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW((recent + L"\\AutomaticDestinations\\*.automaticDestinations-ms").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (stop.load(std::memory_order_relaxed))
                break;
            const std::wstring hash = std::wstring(found.cFileName).substr(0, 16);
            winrt::com_ptr<IStorage> storage;
            const std::wstring fullPath = recent + L"\\AutomaticDestinations\\" + found.cFileName;
            if (const HRESULT hr = StgOpenStorage(fullPath.c_str(), nullptr, STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, storage.put());
                FAILED(hr) || !storage) {
                Log(L"jump list: StgOpenStorage(%s) failed (0x%08X)", hash.c_str(), unsigned(hr));
                continue;
            }
            for (const DestEntry& entry : ParseDestList(ReadStream(storage.get(), L"DestList"))) {
                std::wstring path = entry.real;
                if (!ResolveShellPath(path))
                    continue;
                RecordIndexHash(byPath, FoldCase(path), hash);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (stop.load(std::memory_order_relaxed))
        return;

    FileIndex& index = Index();
    std::lock_guard<std::mutex> guard(index.lock);
    index.byExe = std::move(byExe);
    index.byPath = std::move(byPath);
    index.ready = true;
    index.builtAt = GetTickCount64();
    ++index.generation;
    if (!stop.load(std::memory_order_relaxed))
        Log(L"jump list: indexed %zu programs, %zu items", index.byExe.size(), index.byPath.size());
}

// The app's file, from the index alone: recent documents by AppUserModelID first,
// then unambiguous program path. Empty while the index is still being built.
std::wstring FindIndexedHash(const std::wstring& appId, const std::wstring& exePath)
{
    FileIndex& index = Index();
    {
        std::lock_guard<std::mutex> guard(index.lock);
        if (!index.ready)
            return {};
    }
    // ShellDocumentList invokes COM; query outside index.lock so lookups do not block the index.
    const std::vector<ShellRecent> recentDocs = appId.empty() ? std::vector<ShellRecent>{} : ShellDocumentList(appId, 0 /*ADLT_RECENT*/, 16);
    std::lock_guard<std::mutex> guard(index.lock);
    if (!index.ready)
        return {};
    std::map<std::wstring, size_t> pathHits;
    std::wstring bestHash;
    size_t bestCount = 0;
    for (const ShellRecent& entry : recentDocs) {
        if (const auto it = index.byPath.find(FoldCase(entry.path)); it != index.byPath.end() && !it->second.empty()) {
            if (const size_t count = ++pathHits[it->second]; count > bestCount) {
                bestCount = count;
                bestHash = it->second;
            }
        }
    }
    if (!bestHash.empty())
        return bestHash;
    if (!exePath.empty()) {
        if (const auto it = index.byExe.find(FoldCase(exePath)); it != index.byExe.end() && !it->second.empty())
            return it->second;
    }
    return {};
}

struct IndexRunner {
    std::mutex mutex;
    std::atomic<bool> stop{false};
    std::thread worker;

    IndexRunner() = default;
    IndexRunner(const IndexRunner&) = delete;
    IndexRunner& operator=(const IndexRunner&) = delete;
    ~IndexRunner()
    {
        stop.store(true, std::memory_order_relaxed);
        if (std::thread toJoin = TakeWorker(); toJoin.joinable())
            toJoin.join();
    }
    std::thread TakeWorker()
    {
        std::lock_guard<std::mutex> guard(mutex);
        return std::move(worker);
    }
};

std::wstring ResolveJumpHash(const std::wstring& appId, const std::wstring& exePath)
{
    static std::map<std::wstring, std::wstring> hashByAppId;
    static uint64_t cachedGeneration = 0;
    uint64_t generation = 0;
    bool stale = false;
    {
        FileIndex& index = Index();
        std::lock_guard<std::mutex> guard(index.lock);
        generation = index.generation;
        stale = index.ready && GetTickCount64() - index.builtAt > 60000;
    }
    if (generation != cachedGeneration || hashByAppId.size() >= 256) {
        hashByAppId.clear();
        cachedGeneration = generation;
    }
    const std::wstring cacheKey = appId + L"|" + exePath;
    if (const auto cached = hashByAppId.find(cacheKey); cached != hashByAppId.end())
        return cached->second;
    const std::wstring hash = FindIndexedHash(appId, exePath);
    Log(L"jump list: '%s' -> %s", exePath.c_str(), hash.empty() ? L"(none)" : hash.c_str());
    if (!hash.empty())
        hashByAppId[cacheKey] = hash;
    else if (stale)
        StartJumpListIndex(true); // index may be older than the app's first jump list
    return hash;
}

std::vector<DestEntry> LoadAutomaticEntries(const std::wstring& recent, const std::wstring& hash, std::vector<JumpItem>& outPinned)
{
    std::vector<DestEntry> recentEntries;
    winrt::com_ptr<IStorage> storage;
    const std::wstring path = recent + L"\\AutomaticDestinations\\" + hash + L".automaticDestinations-ms";
    const HRESULT hr = StgOpenStorage(path.c_str(), nullptr, STGM_READ | STGM_SHARE_DENY_WRITE, nullptr, 0, storage.put());
    if (FAILED(hr) || !storage) {
        if (FAILED(hr) && hr != STG_E_FILENOTFOUND)
            Log(L"jump: StgOpenStorage(%s) failed (0x%08X)", hash.c_str(), unsigned(hr));
        return recentEntries;
    }
    std::vector<DestEntry> pinnedEntries;
    std::vector<DestEntry> entries = ParseDestList(ReadStream(storage.get(), L"DestList"));
    Log(L"jump: parsed %zu entries from %s", entries.size(), hash.c_str());
    for (DestEntry& entry : entries) {
        if (entry.path.empty() || (entry.pin <= 0 && recentEntries.size() >= 24))
            continue;
        if (!ResolveShellPath(entry.real)) {
            Log(L"jump: shell name '%s' does not resolve", entry.real.c_str());
            continue;
        }
        // A network item is kept without asking: an offline share answers in
        // tens of seconds, and the entry is the system's own recent item.
        if (!entry.real.starts_with(L"::") && !IsRemoteShare(entry.real) && GetFileAttributesW(entry.real.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (entry.pin > 0)
                Log(L"jump: pinned %d dropped '%s'", entry.pin, entry.real.c_str());
            continue; // a drive that is gone: opening it would land on Documents
        }
        if (entry.pin > 0)
            Log(L"jump: pinned %d '%s' -> '%s'", entry.pin, entry.path.c_str(), entry.real.c_str());
        (entry.pin > 0 ? pinnedEntries : recentEntries).push_back(std::move(entry));
    }
    std::ranges::stable_sort(pinnedEntries, [](const DestEntry& a, const DestEntry& b) { return a.pin < b.pin; });
    for (const DestEntry& entry : pinnedEntries)
        outPinned.push_back(ItemForPath(entry.path));
    return recentEntries;
}

std::vector<JumpItem> BuildRecentItems(const std::vector<DestEntry>& entries, const std::wstring& exePath, size_t max)
{
    std::vector<JumpItem> items;
    for (const DestEntry& entry : entries) {
        if (items.size() >= max)
            break;
        JumpItem item = ItemForPath(entry.path);
        if (!item.folder && !exePath.empty()) {
            item.program = exePath; // the app opened it, so the app reopens it
            item.args = L"\"" + item.target + L"\"";
        }
        items.push_back(std::move(item));
    }
    return items;
}

bool LoadCustomCategories(const std::wstring& recent, const std::wstring& hash, const std::vector<DestEntry>& recentEntries, const std::wstring& exePath,
                          std::vector<JumpCategory>& outCategories)
{
    bool recentCovered = false;
    const auto bytes = ReadFileBytes(recent + L"\\CustomDestinations\\" + hash + L".customDestinations-ms");
    std::vector<JumpCategory> named, known, tasks;
    for (CustomCategory& category : ParseCustom(bytes)) {
        JumpCategory out{category.title, std::move(category.items)};
        if (category.type == 1) {
            out.title = category.known == 1 ? L"Frequent" : L"Recent";
            recentCovered = true;
            // A known category is not stored in the file: its rows are the automatic list.
            if (out.items.empty())
                out.items = BuildRecentItems(recentEntries, exePath, 5);
        }
        if (out.items.empty())
            continue;
        if (category.type == 2 && out.title.empty())
            out.title = L"Tasks";
        (category.type == 2 ? tasks : category.type == 1 ? known : named).push_back(std::move(out));
    }
    for (auto* group : {&named, &known, &tasks})
        for (JumpCategory& category : *group)
            outCategories.push_back(std::move(category));
    return recentCovered;
}

} // namespace

bool IsRemotePath(const std::wstring& path) { return IsRemoteShare(path); }

void StartJumpListIndex(bool enabled)
{
    if (!enabled)
        return; // the menus that read the index are off
    Index();
    [[maybe_unused]] static const bool kLogReady = (Log(L"jump list: index ready"), true);
    static IndexRunner runner;
    {
        FileIndex& index = Index();
        std::lock_guard<std::mutex> guard(index.lock);
        if (index.building)
            return;
        index.building = true;
    }
    if (std::thread previous = runner.TakeWorker(); previous.joinable())
        previous.join();
    std::lock_guard<std::mutex> guard(runner.mutex);
    if (runner.stop.load(std::memory_order_relaxed)) {
        FileIndex& index = Index();
        std::lock_guard<std::mutex> indexGuard(index.lock);
        index.building = false;
        return;
    }
    runner.worker = std::thread([] {
        // A worker without a message pump must not own an STA: the UI thread's
        // shell calls would have to marshal into it and wait for a timeout.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE)
            BuildIndex(runner.stop);
        else if (!runner.stop.load(std::memory_order_relaxed))
            Log(L"jump list: CoInitializeEx failed (0x%08X)", unsigned(com));
        if (SUCCEEDED(com))
            CoUninitialize();
        FileIndex& index = Index();
        std::lock_guard<std::mutex> indexGuard(index.lock);
        index.building = false; // also after a failed build
    });
}

std::shared_ptr<const JumpListData> LoadJumpList(const std::wstring& appId, const std::wstring& exePath)
{
    const std::wstring hash = ResolveJumpHash(appId, exePath);
    if (hash.empty())
        return nullptr;
    const std::wstring recent = RecentFolder();
    auto data = std::make_shared<JumpListData>();
    if (recent.empty())
        return data;
    const std::vector<DestEntry> recentEntries = LoadAutomaticEntries(recent, hash, data->pinned);
    const bool recentCovered = LoadCustomCategories(recent, hash, recentEntries, exePath, data->categories);
    // An app that registered categories opted out of the automatic recent list:
    // Windows then shows only what it registered (Chrome lists closed tabs and
    // tasks, not the files it was asked to open).
    if (!recentCovered && data->categories.empty())
        data->recent = BuildRecentItems(recentEntries, exePath, 5);
    return data;
}

} // namespace atrium
