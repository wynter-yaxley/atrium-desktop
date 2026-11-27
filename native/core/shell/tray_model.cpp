// Pipeline and evidence contracts: docs/TRAY.md. All shell/UIA work stays on Worker.
#include "shell/tray_model.h"

#include "log.h"
#include "shell/shell_icon.h"
#include "shell/shell_taskbar.h"
#include "shell/tray_image.h"
#include "shell/tray_match.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace atrium {

namespace {

constexpr int kIconPx = 64;
using InkCache = std::map<uint64_t, std::pair<std::shared_ptr<const IconImage>, bool>>;
using HostNameMap = std::map<std::wstring, std::pair<std::wstring, std::wstring>>;

std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), int(s.size()), s.data(), int(s.size()), nullptr, nullptr, 0);
    return s;
}

std::wstring VersionString(const std::wstring& path, const wchar_t* field)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size)
        return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return {};
    WORD* lang = nullptr;
    UINT len = 0;
    wchar_t block[16]{};
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lang), &len) && len >= 4)
        swprintf_s(block, L"%04x%04x", lang[0], lang[1]);
    for (const wchar_t* cp : {block[0] ? static_cast<const wchar_t*>(block) : L"040904B0", L"040904B0", L"040904E4"}) {
        wchar_t sub[96];
        swprintf_s(sub, L"\\StringFileInfo\\%s\\%s", cp, field);
        wchar_t* value = nullptr;
        if (VerQueryValueW(data.data(), sub, reinterpret_cast<void**>(&value), &len) && value && len && *value)
            return value;
    }
    return {};
}

// "{6D809377-...}\Tailscale\tailscale-ipn.exe": a known folder, then the rest.
std::wstring ResolveKnownFolder(const std::wstring& path)
{
    GUID id{};
    PWSTR base = nullptr;
    if (path.size() < 39 || path[0] != L'{' || FAILED(CLSIDFromString(path.substr(0, 38).c_str(), &id)))
        return path;
    std::wstring out = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &base)) ? std::wstring(base) + path.substr(38) : path;
    return CoTaskMemFree(base), out;
}

bool DecodePng(const std::vector<BYTE>& bytes, IconImage& out)
{
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    winrt::com_ptr<IStream> stream;
    stream.attach(SHCreateMemStream(bytes.data(), UINT(bytes.size())));
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())) || FAILED(decoder->GetFrame(0, frame.put())) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    std::vector<uint8_t> px(w && h && w <= 512 && h <= 512 ? size_t(w) * h * 4 : 0);
    if (px.empty() || FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data())))
        return false;
    for (size_t i = 3; i < px.size(); i += 4)
        if (px[i])
            return (out = IconImage::FromBgra(int(w), int(h), std::move(px))), true;
    return false;
}

int GlyphTone(const IconImage& img)
{
    size_t opaque = 0, light = 0, dark = 0;
    const auto& data = img.Bytes();
    for (size_t i = 0; i + 3 < data.size(); i += 4) {
        const int a = data[i + 3];
        if (a < 64)
            continue;
        ++opaque;
        const int b = data[i] * 255 / a, g = data[i + 1] * 255 / a, r = data[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma < 40 && lum > 200)
            ++light;
        else if (chroma < 40 && lum < 60)
            ++dark;
    }
    if (opaque < 8 || opaque * 4 > data.size() / 4 * 3)
        return 0;
    return light * 10 >= opaque * 6 ? 1 : dark * 10 >= opaque * 6 ? -1 : 0;
}

IconImage InkedGlyph(const IconImage& img, int tone)
{
    std::vector<uint8_t> px = img.Bytes();
    const int ink[3] = {tone > 0 ? 0x17 : 255, tone > 0 ? 0x13 : 255, tone > 0 ? 0x11 : 255};
    const int strength = tone > 0 ? 235 : 245;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const int a = px[i + 3];
        if (!a)
            continue;
        const int b = px[i] * 255 / a, g = px[i + 1] * 255 / a, r = px[i + 2] * 255 / a;
        const int chroma = std::max({r, g, b}) - std::min({r, g, b}), lum = (54 * r + 183 * g + 19 * b) >> 8;
        if (chroma >= 48 || (tone > 0 ? lum < 128 : lum >= 128))
            continue;
        const int na = a * strength / 255;
        for (int k = 0; k < 3; ++k)
            px[i + size_t(k)] = uint8_t(ink[k] * na / 255);
        px[i + 3] = uint8_t(na);
    }
    return IconImage::FromBgra(img.width, img.height, std::move(px));
}

std::vector<tray_detail::Record> ReadRecords(bool& ordered)
{
    std::vector<tray_detail::Record> out;
    ordered = false;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return out;
    std::map<unsigned long long, size_t> order;
    DWORD bytes = 0;
    if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes >= 8 && bytes < (1u << 16)) {
        std::vector<unsigned long long> ids(bytes / 8);
        DWORD got = DWORD(ids.size() * 8);
        if (RegGetValueW(root, nullptr, L"UIOrderList", RRF_RT_REG_BINARY, nullptr, ids.data(), &got) == ERROR_SUCCESS)
            for (size_t i = 0; i < ids.size(); ++i)
                order.try_emplace(ids[i], i);
    }
    ordered = !order.empty();
    auto readStr = [&](const wchar_t* sub, const wchar_t* val, wchar_t* buf, DWORD cap) {
        return RegGetValueW(root, sub, val, RRF_RT_REG_SZ, nullptr, buf, &cap) == ERROR_SUCCESS;
    };
    auto readDword = [&](const wchar_t* sub, const wchar_t* val, DWORD& outVal) {
        DWORD sz = sizeof(outVal);
        return RegGetValueW(root, sub, val, RRF_RT_REG_DWORD, nullptr, &outVal, &sz) == ERROR_SUCCESS;
    };
    for (DWORD i = 0;; ++i) {
        wchar_t name[128], buf[1024];
        DWORD len = 128, size = 0, uid = 0, promoted = 0;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        tray_detail::Record r;
        r.id = name;
        if (readStr(name, L"ExecutablePath", buf, sizeof(buf)))
            r.exe = Lower(AppExecutable(ResolveKnownFolder(buf)));
        if (readStr(name, L"InitialTooltip", buf, sizeof(buf)))
            r.tip = tray::RepairMojibakeTip(buf);
        if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS && size && size < (1u << 20)) {
            r.png.resize(size);
            if (RegGetValueW(root, name, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, r.png.data(), &size) != ERROR_SUCCESS || size > r.png.size())
                r.png.clear();
            else
                r.png.resize(size);
        }
        if (readDword(name, L"UID", uid))
            r.hasUid = true, r.uid = uid;
        if (readStr(name, L"IconGuid", buf, sizeof(buf)))
            CLSIDFromString(buf, &r.guid);
        if (readDword(name, L"IsPromoted", promoted))
            r.promoted = promoted != 0;
        if (auto it = order.find(wcstoull(name, nullptr, 10)); it != order.end())
            r.order = it->second;
        if (!r.exe.empty())
            out.push_back(std::move(r));
    }
    return RegCloseKey(root), out;
}

struct Program {
    std::vector<std::wstring> titles;
    std::vector<HWND> windows;
};

std::map<std::wstring, Program> RunningPrograms()
{
    std::map<DWORD, std::wstring> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    if (snap != INVALID_HANDLE_VALUE) {
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            paths[pe.th32ProcessID] = L"|" + Lower(pe.szExeFile);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH * 2];
                DWORD len = DWORD(std::size(buf));
                if (QueryFullProcessImageNameW(p, 0, buf, &len))
                    paths[pe.th32ProcessID] = Lower(buf);
                CloseHandle(p);
            }
        }
        CloseHandle(snap);
    }
    std::map<std::wstring, Program> out;
    for (const auto& [pid, path] : paths)
        out[path];
    struct Ctx {
        std::map<DWORD, std::wstring>* paths;
        std::map<std::wstring, Program>* out;
        bool titles;
    } ctx{&paths, &out, true};
    auto each = [](HWND h, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto it = c->paths->find(pid);
        if (it == c->paths->end())
            return TRUE;
        Program& p = (*c->out)[it->second];
        p.windows.push_back(h);
        wchar_t title[256];
        if (c->titles && IsWindowVisible(h) && InternalGetWindowText(h, title, 256) > 0)
            p.titles.emplace_back(title);
        return TRUE;
    };
    EnumWindows(each, reinterpret_cast<LPARAM>(&ctx));
    ctx.titles = false;
    for (HWND h = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); h; h = FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr))
        each(h, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

bool HasIcon(const std::vector<HWND>& windows, UINT uid, RECT& bounds, std::wstring& identity, ShellProbe& probe, std::set<std::pair<HWND, UINT>>& claimed)
{
    for (HWND h : windows) {
        if (!probe.responsive)
            return false;
        NOTIFYICONIDENTIFIER id{sizeof(id), h, uid, GUID_NULL};
        RECT r{};
        if (claimed.contains({h, uid}) || FAILED(ShellNotifyIconRect(id, r, probe)))
            continue;
        bounds = r;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        identity = L"uid:" + std::to_wstring(pid) + L":" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L":" + std::to_wstring(uid);
        return claimed.emplace(h, uid), true;
    }
    return false;
}

const Program* ProbeLiveIcon(const tray_detail::Record& record, const std::map<std::wstring, Program>& running, RECT& bounds, std::wstring& identity,
                             ShellProbe& probe, std::set<std::pair<HWND, UINT>>& claimed)
{
    static const Program kEmpty;
    const auto exact = running.find(record.exe);
    const Program& primary = exact == running.end() ? kEmpty : exact->second;
    if (record.guid != GUID_NULL) {
        NOTIFYICONIDENTIFIER id{sizeof(id), nullptr, 0, record.guid};
        wchar_t guid[40]{};
        if (FAILED(ShellNotifyIconRect(id, bounds, probe)) || !StringFromGUID2(record.guid, guid, 40))
            return nullptr;
        return (identity = L"guid:" + std::wstring(guid)), &primary;
    }
    if (!record.hasUid)
        return nullptr;
    if (HasIcon(primary.windows, record.uid, bounds, identity, probe, claimed))
        return &primary;
    const auto elevated = running.find(L"|" + Lower(PathFindFileNameW(record.exe.c_str())));
    const bool present = elevated != running.end() && (GetFileAttributesW(record.exe.c_str()) != INVALID_FILE_ATTRIBUTES ||
                                                       (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND));
    return present && HasIcon(elevated->second.windows, record.uid, bounds, identity, probe, claimed) ? &elevated->second : nullptr;
}

std::wstring GuidString(const GUID& guid)
{
    wchar_t buf[40]{};
    return guid != GUID_NULL && StringFromGUID2(guid, buf, 40) ? buf : std::wstring{};
}

std::pair<std::wstring, std::wstring> ExeVersionNames(const std::wstring& path, HostNameMap& cache, HostNameMap* added = nullptr)
{
    if (path.empty() || tray::IsShellExplorerExe(path))
        return {};
    if (cache.size() > 512)
        cache.clear();
    auto [it, fresh] = cache.try_emplace(path);
    if (fresh) {
        it->second = {VersionString(path, L"FileDescription"), VersionString(path, L"ProductName")};
        if (added)
            added->emplace(path, it->second);
    }
    return it->second;
}

void FillRecordNames(const std::wstring& path, const Program& program, const std::wstring& tip, const std::wstring& guidOrId,
                     const std::map<std::wstring, std::vector<std::wstring>>& labels, HostNameMap& version, std::vector<std::wstring>& out, size_t& firstTitle)
{
    if (tray::IsShellExplorerExe(path)) {
        out = {tip};
        if (const auto known = tray::KnownShellTray(guidOrId))
            out.insert(out.end(), known->aliases.begin(), known->aliases.end());
        firstTitle = out.size();
        return;
    }
    auto [desc, prod] = ExeVersionNames(path, version);
    std::wstring stem = PathFindFileNameW(path.c_str());
    if (auto dot = stem.rfind(L'.'); dot != std::wstring::npos)
        stem.resize(dot);
    out = {tip, std::move(desc), std::move(prod), stem};
    if (const auto known = tray::KnownShellTray(guidOrId))
        out.insert(out.end(), known->aliases.begin(), known->aliases.end());
    if (auto it = labels.find(path); it != labels.end())
        out.insert(out.end(), it->second.begin(), it->second.end());
    firstTitle = out.size();
    out.insert(out.end(), program.titles.begin(), program.titles.end());
}

bool HasUnmatchedLive(const std::vector<tray::Record>& rs, const tray::Result& initial, const std::wstring& path)
{
    return std::ranges::any_of(rs, [&](const tray::Record& r) {
        const size_t idx = size_t(&r - rs.data());
        return r.live && r.exe == path && std::ranges::none_of(initial.items, [&](const tray::Pair& item) { return item.record == idx; });
    });
}

void FillDiagnostics(const std::vector<tray::Record>& rs, const tray::Result& result, tray_detail::ReadDiagnostics* d)
{
    if (!d)
        return;
    *d = {rs.size()};
    for (size_t r = 0; r < rs.size(); ++r)
        d->live += rs[r].live, d->aliases += result.aliases[r] != tray::kNone && result.aliases[r] != r, d->shared += result.sharedPlace[r];
    for (const auto& item : result.items)
        d->unknown += item.method == tray::Method::Unknown;
}

std::vector<TrayIcon> InWindowsOrder(std::vector<TrayIcon> list)
{
    std::ranges::stable_sort(list, [](const TrayIcon& a, const TrayIcon& b) { return a.order < b.order; });
    std::map<std::wstring, int> counts;
    for (size_t i = 0; i < list.size(); ++i)
        list[i].index = int(i), list[i].exeOrdinal = list[i].exe.empty() ? -1 : counts[list[i].exe]++;
    return list;
}

std::pair<std::shared_ptr<const IconImage>, bool> MakeInked(const IconImage& img)
{
    const int tone = img.Empty() ? 0 : GlyphTone(img);
    return {tone ? std::make_shared<const IconImage>(InkedGlyph(img, tone)) : nullptr, tone > 0};
}

TrayIcon BuildHostedIcon(const HostIcon& h, const tray::HostedEntry& entry, const TrayIcon* displayFb, const TrayIcon* targetFb,
                         const std::map<std::wstring, size_t>& order, InkCache& ink, InkCache& newInk, ULONGLONG now)
{
    static const tray::ShellSession kHostShell{1, 1, 1};
    TrayIcon t{entry.tip, !h.exe.empty() ? h.exe : (displayFb ? displayFb->exe : std::wstring{}), h.image, h.imageVersion};
    t.hostKey = h.key, t.pid = h.pid, t.appTooltip = h.version >= 4 && !h.showTip, t.flashing = h.flashUntil > now;
    t.order = displayFb && displayFb->order != SIZE_MAX ? displayFb->order : order.contains(L"exe:" + t.exe) ? order.at(L"exe:" + t.exe) : SIZE_MAX;
    for (const auto& k : {L"uid:" + t.exe + L"|" + std::to_wstring(h.uid), h.key})
        if (auto o = order.find(k); o != order.end())
            t.order = o->second;
    if (t.tip.empty())
        t.tip = displayFb && !displayFb->tip.empty() ? displayFb->tip : entry.fallbackTip;
    t.recordId = L"host:" + h.key, t.shell = kHostShell, t.matchMethod = L"host";
    if (targetFb)
        t.explorerTarget = targetFb->Target();
    if (!t.image && displayFb && displayFb->image)
        t.image = displayFb->image, t.imageVersion = displayFb->imageVersion, t.inked = displayFb->inked, t.inkedOnLight = displayFb->inkedOnLight;
    else if (t.image) {
        if (auto [it, fresh] = ink.try_emplace(h.imageVersion); fresh) {
            it->second = MakeInked(*t.image);
            newInk.try_emplace(h.imageVersion, it->second);
        }
        t.inked = ink[h.imageVersion].first, t.inkedOnLight = ink[h.imageVersion].second;
    }
    return t;
}

struct Picture {
    uint64_t version = 0;
    std::wstring exe;
    std::vector<BYTE> snapshot;
    std::shared_ptr<const IconImage> image, inked;
    bool inkedOnLight = false;
};

void ResolveRecordPicture(const tray_detail::Record& r, const std::wstring& recordId, Picture& pic, uint64_t& nextImageVersion)
{
    if (pic.image && pic.exe == r.exe && pic.snapshot == r.png)
        return;
    IconImage img;
    const auto art = tray::LoadArtwork([&] { return !r.png.empty() && DecodePng(r.png, img); },
                                       [&] {
                                           if (tray::IsShellExplorerExe(r.exe)) {
                                               const auto known = tray::KnownShellTray(recordId.empty() ? GuidString(r.guid) : recordId);
                                               return known && known->iconDll && LoadShellImage(known->iconDll, kIconPx, img);
                                           }
                                           return LoadAtriumIcon(r.exe, kIconPx, img) || LoadShellImage(r.exe, kIconPx, img);
                                       });
    if (art == tray::Artwork::Missing)
        img = {};
    pic.version = ++nextImageVersion, pic.exe = r.exe, pic.snapshot = r.png;
    Log(L"tray: artwork identity=%s source=%s version=%llu bytes=%zu", recordId.c_str(),
        art == tray::Artwork::Snapshot    ? L"snapshot" :
        art == tray::Artwork::Application ? L"application" :
                                            L"missing",
        pic.version, r.png.size());
    std::tie(pic.inked, pic.inkedOnLight) = MakeInked(img);
    if (pic.inked)
        Log(L"tray: %s draws a %s glyph: it follows the ink", PathFindFileNameW(r.exe.c_str()), pic.inkedOnLight ? L"white" : L"black");
    pic.image = img.Empty() ? nullptr : std::make_shared<const IconImage>(std::move(img));
}

std::map<std::wstring, size_t> BuildWindowsOrder(const std::vector<tray_detail::Record>& records)
{
    std::map<std::wstring, size_t> out;
    for (const tray_detail::Record& r : records) {
        if (r.order == SIZE_MAX)
            continue;
        if (const std::wstring guid = GuidString(r.guid); !guid.empty())
            out.try_emplace(L"guid:" + guid, r.order);
        if (r.hasUid)
            out.try_emplace(L"uid:" + r.exe + L"|" + std::to_wstring(r.uid), r.order);
        if (auto [it, fresh] = out.try_emplace(L"exe:" + r.exe, r.order); !fresh)
            it->second = std::min(it->second, r.order);
    }
    return out;
}

bool SameTrayIcon(const TrayIcon& a, const TrayIcon& b)
{
    return a.tip == b.tip && a.exe == b.exe && a.image == b.image && a.imageVersion == b.imageVersion && a.inked == b.inked &&
           a.inkedOnLight == b.inkedOnLight && a.promoted == b.promoted && a.index == b.index && a.runtimeId == b.runtimeId && a.shell == b.shell &&
           a.matchMethod == b.matchMethod && a.fromRecord == b.fromRecord && a.recordId == b.recordId && a.hostKey == b.hostKey && a.pid == b.pid &&
           a.appTooltip == b.appTooltip && a.flashing == b.flashing && a.order == b.order && a.exeOrdinal == b.exeOrdinal &&
           a.explorerTarget == b.explorerTarget;
}

void LogTrayRefresh(bool changed, size_t iconCount, int recognised, bool ordered, const std::vector<tray_detail::Found>& buttons,
                    const std::vector<tray_detail::Record>& records, const std::vector<int>& match, ULONGLONG elapsedMs, ULONGLONG readMs, bool available)
{
    const bool classic = std::ranges::any_of(buttons, [](const tray_detail::Found& f) { return f.toolbar != nullptr; });
    if (changed) {
        Log(L"tray: %zu icons (%d recognised %s%s) in %llu ms%s", iconCount, recognised, ordered ? L"in Windows' order" : L"by name",
            classic ? L", classic tray" : L"", elapsedMs, available ? L"" : L", the shell's tray was not found");
        for (size_t b = 0; b < buttons.size(); ++b) {
            const wchar_t* exe = match[b] >= 0 ? records[size_t(match[b])].exe.c_str() : L"-";
            Log(L"tray: item=%zu source=%s match=%s promoted=%d index=%d identity=%s tip='%s' exe='%s'", b, buttons[b].element ? L"uia" : L"record",
                tray::MethodName(buttons[b].method), buttons[b].promoted, buttons[b].index, buttons[b].recordId.c_str(), tray::Title(buttons[b].tip).c_str(),
                exe);
        }
    } else if (elapsedMs > 500)
        Log(L"tray: slow unchanged refresh totalMs=%llu uiaMs=%llu", elapsedMs, readMs);
}

void InsertCapped(auto& dst, const auto& src)
{
    if (src.empty())
        return;
    if (dst.size() > 256)
        dst.clear();
    dst.insert(src.begin(), src.end());
}

std::vector<TrayIcon> MergeSnapshotIcons(const std::vector<TrayIcon>& explorer, std::vector<HostIcon>& hosted, const std::map<std::wstring, size_t>& order,
                                         HostNameMap& names, HostNameMap& newNames, InkCache& ink, InkCache& newInk)
{
    std::erase_if(hosted, [](const HostIcon& h) { return h.hidden; });
    std::vector<tray::HostedEntry> hostEntries;
    hostEntries.reserve(hosted.size());
    for (const auto& h : hosted) {
        auto [desc, prod] = ExeVersionNames(h.exe, names, &newNames);
        if (desc.empty() && !h.exe.empty() && !tray::IsShellExplorerExe(h.exe))
            desc = prod.empty() ? PathFindFileNameW(h.exe.c_str()) : prod;
        hostEntries.push_back({h.key, h.exe, tray::RepairMojibakeTip(h.tip), std::move(desc), std::move(prod), h.uid, h.uid != 0 || h.key.starts_with(L"hwnd:"),
                               uint32_t(h.pid)});
    }
    std::vector<tray::ExplorerEntry> expEntries;
    expEntries.reserve(explorer.size());
    for (const auto& e : explorer)
        expEntries.push_back({e.recordId, e.exe, e.tip, e.image != nullptr});
    const auto merged = tray::MergeHostedIcons(hostEntries, expEntries);
    const ULONGLONG now = GetTickCount64();
    std::vector<TrayIcon> next;
    next.reserve(hosted.size() + merged.extraExplorer.size());
    for (size_t i = 0; i < hosted.size(); ++i) {
        const TrayIcon* displayFb = merged.hostedFallback[i] != tray::kNone ? &explorer[merged.hostedFallback[i]] : nullptr;
        const TrayIcon* targetFb = merged.hostedTarget[i] != tray::kNone ? &explorer[merged.hostedTarget[i]] : nullptr;
        next.push_back(BuildHostedIcon(hosted[i], hostEntries[i], displayFb, targetFb, order, ink, newInk, now));
    }
    for (size_t ei : merged.extraExplorer)
        next.push_back(explorer[ei]);
    return InWindowsOrder(std::move(next));
}

} // namespace

namespace tray_detail {

std::vector<int> MatchButtons(std::vector<Found>& buttons, std::vector<Record>& records, bool& ordered, const std::vector<PinnedApp>& apps,
                              ReadDiagnostics* diagnostics)
{
    records = ReadRecords(ordered);
    const auto running = RunningPrograms();
    std::map<std::wstring, std::vector<std::wstring>> labels;
    for (const auto& app : apps)
        if (!app.path.empty() && !app.name.empty())
            labels[Lower(AppExecutable(app.path))].push_back(app.name);
    static HostNameMap version;
    auto rect = [](const RECT& r) { return tray::Rect{r.left, r.top, r.right, r.bottom}; };
    std::vector<tray::Button> bs;
    bs.reserve(buttons.size());
    for (const auto& b : buttons)
        bs.push_back({b.tip, b.promoted, rect(b.bounds)});
    std::vector<tray::Record> rs;
    ShellProbe probe;
    std::set<std::pair<HWND, UINT>> claimed;
    for (const auto& r : records) {
        tray::Record value{r.id, r.exe, r.tip, {}, false, r.promoted, r.order};
        RECT bounds{};
        const Program* matched = ProbeLiveIcon(r, running, bounds, value.identity, probe, claimed);
        value.live = matched != nullptr, value.bounds = rect(bounds);
        if (matched) {
            const std::wstring guid = GuidString(r.guid);
            FillRecordNames(r.exe, *matched, r.tip, guid.empty() ? r.id : guid, labels, version, value.names, value.firstTitle);
        }
        rs.push_back(std::move(value));
    }
    std::vector<tray::Program> ps;
    const auto initial = tray::Match(bs, rs);
    const bool needPrograms = std::ranges::any_of(initial.items, [](const auto& item) { return item.button != tray::kNone && item.record == tray::kNone; });
    for (const auto& [path, program] : running) {
        if (!needPrograms || path.empty() || path[0] == L'|' || tray::IsShellExplorerExe(path) || program.windows.empty() ||
            HasUnmatchedLive(rs, initial, path))
            continue;
        tray::Program value{path};
        FillRecordNames(path, program, {}, {}, labels, version, value.names, value.firstTitle);
        ps.push_back(std::move(value));
    }
    const auto result = tray::Match(bs, rs, ps);
    FillDiagnostics(rs, result, diagnostics);
    std::vector<int> match;
    for (const auto& item : result.items) {
        if (item.button == tray::kNone) {
            const auto& r = rs[item.record];
            Found f;
            f.tip = tray::FallbackRecordTip(r), f.promoted = r.promoted;
            f.index = int(std::count_if(buttons.begin(), buttons.end(), [&](const Found& b) { return b.promoted == f.promoted; }));
            buttons.push_back(std::move(f));
        }
        Found& f = buttons[match.size()];
        f.method = item.method;
        if (item.record != tray::kNone) {
            if (f.tip.empty())
                f.tip = tray::FallbackRecordTip(rs[item.record]);
            f.recordId = rs[item.record].identity;
            match.push_back(int(item.record));
        } else if (item.program != tray::kNone) {
            records.push_back({L"run:" + ps[item.program].exe, ps[item.program].exe}), match.push_back(int(records.size() - 1));
        } else
            match.push_back(-1);
    }
    return match;
}

} // namespace tray_detail

std::wstring TrayModel::Title(const std::wstring& tip) { return tray::Title(tip); }

TrayModel::TrayModel(HWND notify, UINT msg, std::function<std::vector<PinnedApp>()> appNames)
    : notify_(notify),
      msg_(msg),
      appNames_(std::move(appNames)),
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      snapshot_(std::make_shared<const std::vector<TrayIcon>>()),
      thread_([this] { Worker(); })
{
}

TrayModel::~TrayModel()
{
    std::unique_ptr<TrayHost> host;
    {
        std::lock_guard g(mutex_);
        stop_ = true, std::swap(host, host_);
    }
    if (stopEvent_)
        SetEvent(stopEvent_);
    cv_.notify_all(), host.reset();
    if (thread_.joinable())
        thread_.join();
    if (stopEvent_)
        CloseHandle(stopEvent_);
}

void TrayModel::Refresh()
{
    std::lock_guard g(mutex_);
    if (std::ranges::none_of(jobs_, [](const Job& j) { return j.refresh; }))
        jobs_.push_back({true, {}});
    cv_.notify_all();
}

void TrayModel::SetHosting(bool on)
{
    if (on == (host_ != nullptr))
        return;
    auto onYield = [this] { RebuildSnapshot(true), Refresh(); };
    auto onIcons = [this] { RebuildSnapshot(false); };
    std::unique_ptr<TrayHost> host = on ? std::make_unique<TrayHost>(notify_, msg_, 3000, 0, onYield, onIcons) : nullptr;
    if (host && !host->Running())
        host.reset();
    {
        std::lock_guard g(mutex_);
        std::swap(host, host_), hostInk_.clear();
    }
    host.reset(), RebuildSnapshot(true), Refresh();
}

void TrayModel::SetHostPlaces(RECT anchor)
{
    if (host_)
        host_->SetPlaces(anchor);
}

std::vector<TrayIcon> TrayModel::Icons() const
{
    std::lock_guard g(mutex_);
    return snapshot_ ? *snapshot_ : std::vector<TrayIcon>{};
}

void TrayModel::RebuildSnapshot(bool forceNotify)
{
    std::lock_guard buildGuard(snapshotBuildMutex_);
    std::vector<HostIcon> hosted;
    std::vector<TrayIcon> explorer;
    std::map<std::wstring, size_t> order;
    HostNameMap names, newNames;
    InkCache ink, newInk;
    const TrayHost* hostPtr = nullptr;
    bool hostedActive = false;
    {
        std::lock_guard g(mutex_);
        explorer = icons_, hostedActive = host_ && !host_->Yielded();
        if (hostedActive)
            hostPtr = host_.get(), hosted = host_->Icons(), order = windowsOrder_, names = hostNames_, ink = hostInk_;
    }
    std::vector<TrayIcon> next = hostedActive ? MergeSnapshotIcons(explorer, hosted, order, names, newNames, ink, newInk) : std::move(explorer);
    bool changed = false;
    {
        std::lock_guard g(mutex_);
        InsertCapped(hostNames_, newNames);
        if (host_.get() == hostPtr)
            InsertCapped(hostInk_, newInk);
        changed = !snapshot_ || next.size() != snapshot_->size() || !std::equal(next.begin(), next.end(), snapshot_->begin(), SameTrayIcon);
        if (changed)
            snapshot_ = std::make_shared<const std::vector<TrayIcon>>(std::move(next));
    }
    if ((changed || forceNotify) && notify_)
        PostMessageW(notify_, msg_, 0, 0);
}

struct TrayModel::WorkerState {
    winrt::com_ptr<IUIAutomation> uia;
    uint64_t nextImageVersion = 0;
    std::map<std::wstring, Picture> pictures;
    std::chrono::steady_clock::time_point retryAt = std::chrono::steady_clock::time_point::max();
    tray::ShellSession recoverySession;
    bool recoveryStarted = false;
    unsigned retries = 0;
};

bool TrayModel::DequeueWorkerJob(WorkerState& ws, Job& job)
{
    std::unique_lock g(mutex_);
    if (ws.retryAt == std::chrono::steady_clock::time_point::max())
        cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
    else
        cv_.wait_until(g, ws.retryAt, [&] { return stop_ || !jobs_.empty(); });
    if (stop_)
        return false;
    return jobs_.empty() ? (job.refresh = true, ws.retryAt = std::chrono::steady_clock::time_point::max(), true) :
                           (job = jobs_.front(), jobs_.pop_front(), true);
}

void TrayModel::RunRefreshJob(WorkerState& ws, const tray::ShellSession& session, ULONGLONG findStart)
{
    bool available = false, ordered = false;
    std::vector<tray_detail::Found> buttons = tray_detail::FindButtons(ws.uia.get(), available);
    const ULONGLONG readMs = GetTickCount64() - findStart;
    const auto appNames = appNames_ ? appNames_() : std::vector<PinnedApp>{};
    std::vector<tray_detail::Record> records;
    tray_detail::ReadDiagnostics diagnostics;
    std::vector<int> match = tray_detail::MatchButtons(buttons, records, ordered, appNames, &diagnostics);
    if (session != tray_detail::ReadShellSession()) {
        Log(L"tray: discarded refresh spanning shell replacement");
        Refresh();
        return;
    }
    if (!session.Valid())
        buttons.clear(), match.clear(), available = false;
    std::vector<TrayIcon> icons;
    std::set<std::wstring> livePictures;
    int recognised = 0;
    for (size_t b = 0; b < buttons.size(); ++b) {
        TrayIcon t;
        t.tip = buttons[b].tip, t.promoted = buttons[b].promoted, t.index = buttons[b].index, t.recordId = buttons[b].recordId;
        t.runtimeId = buttons[b].runtimeId, t.shell = session, t.matchMethod = tray::MethodName(buttons[b].method), t.fromRecord = !buttons[b].element;
        if (match[b] >= 0) {
            const tray_detail::Record& r = records[size_t(match[b])];
            t.exe = r.exe, t.order = r.order, ++recognised, livePictures.insert(r.id);
            auto& pic = ws.pictures[r.id];
            ResolveRecordPicture(r, t.recordId, pic, ws.nextImageVersion);
            t.imageVersion = pic.version, t.image = pic.image, t.inked = pic.inked, t.inkedOnLight = pic.inkedOnLight;
        }
        icons.push_back(std::move(t));
    }
    icons = InWindowsOrder(std::move(icons));
    std::erase_if(ws.pictures, [&](const auto& item) { return !livePictures.contains(item.first); });
    std::map<std::wstring, size_t> windowsOrder = BuildWindowsOrder(records);
    bool explorerChanged = false;
    {
        std::lock_guard g(mutex_);
        const bool orderChanged = windowsOrder != windowsOrder_;
        explorerChanged = !loaded_ || available != available_ || orderChanged || icons.size() != icons_.size() ||
                          !std::equal(icons.begin(), icons.end(), icons_.begin(), SameTrayIcon);
        windowsOrder_ = std::move(windowsOrder);
        icons_ = std::move(icons), loaded_ = true, available_ = available;
    }
    if (explorerChanged || session != lastReconcileSession_ || diagnostics.records != lastReconcileRecords_ || diagnostics.live != lastReconcileLive_) {
        lastReconcileSession_ = session, lastReconcileRecords_ = diagnostics.records, lastReconcileLive_ = diagnostics.live;
        Log(L"tray: reconcile records=%zu live=%zu aliases=%zu sharedPlace=%zu unknown=%zu uiaMs=%llu", diagnostics.records, diagnostics.live,
            diagnostics.aliases, diagnostics.shared, diagnostics.unknown, readMs);
    }
    if (diagnostics.records > 0 && diagnostics.live == 0)
        LogShellTrayWindows();
    LogTrayRefresh(explorerChanged, buttons.size(), recognised, ordered, buttons, records, match, GetTickCount64() - findStart, readMs, available);
    RebuildSnapshot(explorerChanged);
}

void TrayModel::Worker()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    WorkerState ws;
    HRESULT created = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(ws.uia.put()));
    if (FAILED(created))
        created = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(ws.uia.put()));
    if (FAILED(created))
        Log(L"tray: UI Automation unavailable");
    if (auto options = ws.uia.try_as<IUIAutomation2>())
        options->put_ConnectionTimeout(250), options->put_TransactionTimeout(250);
    for (Job job; DequeueWorkerJob(ws, job);) {
        if (!ws.uia) {
            if (!job.refresh) {
                PostMessageW(notify_, msg_, tray_detail::ShowOverflow(nullptr, stopEvent_) ? 2 : 3, 0);
            } else {
                std::unique_lock g(mutex_);
                const bool firstLoad = !loaded_;
                loaded_ = true, available_ = false;
                g.unlock();
                if (firstLoad)
                    RebuildSnapshot(true);
            }
            continue;
        }
        const ULONGLONG findStart = GetTickCount64();
        if (job.systemTray) {
            PostMessageW(notify_, msg_, tray_detail::ShowOverflow(ws.uia.get(), stopEvent_) ? 5 : 3, 0);
            continue;
        }
        const auto session = tray_detail::ReadShellSession();
        if (!ws.recoveryStarted || session != ws.recoverySession)
            ws.recoveryStarted = true, ws.recoverySession = session, ws.retries = 5;
        if (job.refresh && ws.retries) {
            const unsigned delay = 500u << (5 - ws.retries--);
            ws.retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
            Log(L"tray: shell recovery refresh scheduled delayMs=%u remaining=%u", delay, ws.retries);
        }
        job.refresh ? RunRefreshJob(ws, session, findStart) : RunClickJob(ws.uia.get(), job, session, findStart);
    }
    ws.uia = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
