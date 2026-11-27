#include "tray_match.h"
#include "win32_path.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <numeric>
#include <string_view>

namespace atrium::tray {
std::wstring Title(const std::wstring& tip)
{
    const auto a = tip.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos)
        return {};
    auto line = tip.substr(a, tip.find_first_of(L"\r\n", a) - a);
    while (!line.empty() && iswspace(line.back()))
        line.pop_back();
    return line;
}
std::wstring Key(const std::wstring& text)
{
    std::wstring out;
    for (wchar_t c : text)
        if (!iswspace(c) && std::wstring_view(L"-_:：·|()（）[]【】.,，。；;").find(c) == std::wstring_view::npos)
            out += c >= L'A' && c <= L'Z' ? wchar_t(c + L'a' - L'A') : c;
    return out;
}
bool IsShellExplorerExe(const std::wstring& exe)
{
    constexpr std::wstring_view kName = L"explorer.exe";
    if (exe.size() < kName.size())
        return false;
    const size_t start = exe.size() - kName.size();
    if (start > 0 && exe[start - 1] != L'\\' && exe[start - 1] != L'/')
        return false;
    for (size_t i = 0; i < kName.size(); ++i) {
        wchar_t c = exe[start + i];
        if (c >= L'A' && c <= L'Z')
            c = wchar_t(c + L'a' - L'A');
        if (c != kName[i])
            return false;
    }
    return true;
}
std::optional<ShellTrayKnown> KnownShellTray(const std::wstring& guidOrId)
{
    std::wstring norm;
    for (wchar_t c : guidOrId) {
        if (c == L'{' || c == L'}' || iswspace(c))
            continue;
        norm += c >= L'a' && c <= L'z' ? wchar_t(c + L'A' - L'a') : c;
    }
    if (norm.starts_with(L"GUID:"))
        norm.erase(0, 5);
    if (norm == L"6DA68F06-00F1-4E6E-8158-7F1FFD4F9DB9")
        return ShellTrayKnown{{L"蓝牙设备", L"Bluetooth Devices", L"Bluetooth"}, L"bthprops.cpl"};
    if (norm == L"7820AE78-23E3-4229-82C1-E41CB67D5B9C")
        return ShellTrayKnown{{L"安全删除硬件并弹出媒体", L"Safely Remove Hardware and Eject Media"}, L"hotplug.dll"};
    if (norm == L"BA82E2DC-F405-47AD-B032-CF0FAA0E3933")
        return ShellTrayKnown{{L"Windows 安全中心", L"Windows Security"}, nullptr};
    if (norm == L"7120A42D-CB02-4A51-B3B5-58E36E8B14E1" || norm == L"D68540CC-E0D8-4298-B55C-E6E324121F77")
        return ShellTrayKnown{{L"管理显示模式", L"管理顯示模式", L"Manage Display Mode", L"NVIDIA Display Mode"}, nullptr};
    if (norm == L"D36B22A6-995E-4340-8339-3DE08C6840DF")
        return ShellTrayKnown{{L"NVIDIA GPU 活动", L"NVIDIA GPU 活動", L"NVIDIA GPU Activity", L"NVIDIA GeForce", L"NVIDIA RTX", L"NVIDIA Quadro",
                               L"NVIDIA TITAN", L"GPU Activity"},
                              nullptr};
    return std::nullopt;
}
std::wstring FallbackRecordTip(const Record& r)
{
    if (!r.tip.empty())
        return r.tip;
    if (const auto known = KnownShellTray(r.identity.empty() ? r.id : r.identity); known && !known->aliases.empty())
        return known->aliases.front();
    if (IsShellExplorerExe(r.exe))
        return {};
    for (size_t i : {size_t(1), size_t(2), size_t(3)})
        if (r.names.size() > i && !r.names[i].empty())
            return r.names[i];
    return {};
}
namespace {
std::wstring RepairMojibakeSegment(const std::wstring& seg) { return RepairUtf8Mojibake(seg, GetACP(), false); }
bool DelimitedPrefix(const std::wstring& text, const std::wstring& name)
{
    // Keep the boundary that Key removes: Steam - syncing is a status suffix,
    // SteamVR / SteamHelper is a different name, even if Steam is the only candidate.
    std::wstring prefix;
    for (size_t i = 0; i < text.size(); ++i) {
        prefix += Key(text.substr(i, 1));
        if (prefix == name)
            return i + 1 < text.size() && Key(text.substr(i + 1, 1)).empty();
        if (!name.starts_with(prefix))
            return false;
    }
    return false;
}
bool DelimitedContains(const std::wstring& haystack, const std::wstring& needleKey)
{
    if (needleKey.size() < 4)
        return false;
    for (size_t start = 0; start < haystack.size(); ++start) {
        if (start > 0 && !Key(haystack.substr(start - 1, 1)).empty())
            continue;
        if (Key(haystack.substr(start, 1)).empty())
            continue;
        std::wstring prefix;
        for (size_t i = start; i < haystack.size(); ++i) {
            prefix += Key(haystack.substr(i, 1));
            if (prefix == needleKey) {
                if (i + 1 == haystack.size() || Key(haystack.substr(i + 1, 1)).empty())
                    return true;
                break;
            }
            if (!needleKey.starts_with(prefix))
                break;
        }
    }
    return false;
}
int Score(const std::wstring& tip, const std::vector<std::wstring>& names, size_t firstTitle)
{
    const auto title = Title(tip), key = Key(title);
    if (key.empty())
        return 0;
    int best = 0;
    for (size_t n = 0; n < names.size(); ++n) {
        const auto nameTitle = Title(names[n]), name = Key(nameTitle);
        if (name.size() < 2)
            continue;
        int score = 0;
        if (key == name)
            score = 100;
        else if (n < firstTitle && DelimitedPrefix(title, name))
            score = 80;
        else if (n < firstTitle && ((name.size() >= 4 && DelimitedContains(title, name)) || (key.size() >= 4 && DelimitedContains(nameTitle, key))))
            score = 60;
        best = std::max(best, score);
    }
    return best;
}
bool ContainsCenter(const Rect& cell, const Rect& icon)
{
    if (!cell.Valid() || !icon.Valid())
        return false;
    const long x = icon.left + (icon.right - icon.left) / 2;
    const long y = icon.top + (icon.bottom - icon.top) / 2;
    return x >= cell.left && x < cell.right && y >= cell.top && y < cell.bottom;
}
void AssignMatch(Result& out, std::vector<bool>& used, size_t b, size_t r, Method method)
{
    out.items[b].record = r;
    out.items[b].method = method;
    used[r] = true;
}
std::vector<size_t> CollectLiveRecords(const std::vector<Record>& records, Result& out)
{
    std::vector<size_t> sorted(records.size());
    std::iota(sorted.begin(), sorted.end(), 0);
    std::ranges::stable_sort(sorted, [&](size_t a, size_t b) {
        if (records[a].order != records[b].order)
            return records[a].order < records[b].order;
        return records[a].id < records[b].id;
    });
    std::map<std::wstring, size_t> identities;
    std::vector<size_t> live;
    for (size_t r : sorted) {
        if (!records[r].live || records[r].identity.empty())
            continue;
        const auto [it, fresh] = identities.try_emplace(records[r].identity, r);
        out.aliases[r] = it->second;
        if (fresh)
            live.push_back(r);
    }
    // Includes multiple identities from the SAME executable. Never consume a shared rectangle.
    for (size_t r : live)
        for (size_t s : live)
            if (r != s && records[r].bounds.Valid() && records[r].bounds == records[s].bounds)
                out.sharedPlace[r] = true;
    return live;
}
void MatchByGeometry(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<size_t>& live, std::vector<bool>& used,
                     Result& out)
{
    // Geometry is usable only when unique in BOTH directions, before any records are consumed.
    for (size_t b = 0; b < buttons.size(); ++b) {
        std::vector<size_t> hits;
        for (size_t r : live)
            if (!out.sharedPlace[r] && ContainsCenter(buttons[b].bounds, records[r].bounds))
                hits.push_back(r);
        if (hits.size() != 1)
            continue;
        size_t r = hits.front(), count = 0;
        for (const auto& button : buttons)
            count += ContainsCenter(button.bounds, records[r].bounds);
        if (count == 1)
            AssignMatch(out, used, b, r, Method::Geometry);
    }
}
void MatchByName(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<size_t>& live, std::vector<bool>& used, Result& out)
{
    // Mutual unique best name. Equal scores never silently select the first executable.
    for (int threshold : {100, 80, 60}) {
        for (size_t b = 0; b < buttons.size(); ++b) {
            if (out.items[b].record != kNone)
                continue;
            int best = threshold - 1;
            size_t chosen = kNone;
            bool tied = false;
            for (size_t r : live) {
                if (used[r])
                    continue;
                int score = Score(buttons[b].tip, records[r].names, records[r].firstTitle);
                if (score > best)
                    best = score, chosen = r, tied = false;
                else if (score == best)
                    tied = true;
            }
            if (chosen == kNone || tied)
                continue;
            bool contested = false;
            for (size_t other = 0; other < buttons.size(); ++other)
                if (other != b && out.items[other].record == kNone && Score(buttons[other].tip, records[chosen].names, records[chosen].firstTitle) >= best)
                    contested = true;
            if (!contested)
                AssignMatch(out, used, b, chosen, Method::Name);
        }
    }
}
void MatchByOrder(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<size_t>& live, std::vector<bool>& used, Result& out)
{
    // Order is a weak hint: require equal unmatched counts per partition and relative order with anchors.
    for (bool promoted : {true, false}) {
        std::vector<size_t> ub, ur;
        for (size_t b = 0; b < buttons.size(); ++b)
            if (buttons[b].promoted == promoted && out.items[b].record == kNone)
                ub.push_back(b);
        for (size_t r : live)
            if (records[r].promoted == promoted && !used[r])
                ur.push_back(r);
        if (ub.empty() || ub.size() != ur.size())
            continue;
        bool agrees = true;
        size_t prevOrder = 0;
        bool hasPrev = false;
        size_t uIdx = 0;
        for (size_t b = 0; b < buttons.size() && agrees; ++b) {
            if (buttons[b].promoted != promoted)
                continue;
            const size_t r = out.items[b].record != kNone ? out.items[b].record : ur[uIdx++];
            if (out.items[b].record != kNone && records[r].promoted != promoted)
                continue;
            if (records[r].order == kNone || (hasPrev && records[r].order <= prevOrder))
                agrees = false;
            prevOrder = records[r].order;
            hasPrev = true;
        }
        if (agrees)
            for (size_t i = 0; i < ub.size(); ++i)
                AssignMatch(out, used, ub[i], ur[i], Method::Order);
    }
}
void MatchRunningPrograms(const std::vector<Button>& buttons, const std::vector<Program>& programs, Result& out)
{
    for (auto& item : out.items) {
        if (item.record != kNone)
            continue;
        int best = 79;
        bool tied = false;
        for (size_t p = 0; p < programs.size(); ++p) {
            int score = Score(buttons[item.button].tip, programs[p].names, programs[p].firstTitle);
            if (score > best)
                best = score, item.program = p, tied = false;
            else if (score == best)
                tied = true;
        }
        if (tied)
            item.program = kNone;
        if (item.program != kNone)
            item.method = Method::Running;
    }
}
void AttachRemaining(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<size_t>& live, std::vector<bool>& used,
                     Result& out)
{
    for (size_t r : live) {
        if (used[r])
            continue;
        size_t hit = kNone;
        int best = 59;
        bool tied = false;
        for (size_t b = 0; b < buttons.size(); ++b) {
            if (out.items[b].record != kNone || out.items[b].program != kNone || buttons[b].promoted != records[r].promoted)
                continue;
            int score = Score(buttons[b].tip, records[r].names, records[r].firstTitle);
            if (score > best)
                best = score, hit = b, tied = false;
            else if (score == best)
                tied = true;
        }
        if (hit != kNone && !tied) {
            const bool contested = std::ranges::any_of(live, [&](size_t otherR) {
                return otherR != r && !used[otherR] && records[otherR].promoted == records[r].promoted &&
                       Score(buttons[hit].tip, records[otherR].names, records[otherR].firstTitle) >= best;
            });
            if (!contested)
                AssignMatch(out, used, hit, r, Method::Name);
        }
    }
    for (bool promoted : {true, false}) {
        size_t ub = kNone, ur = kNone, nb = 0, nr = 0;
        for (size_t b = 0; b < buttons.size(); ++b)
            if (buttons[b].promoted == promoted && out.items[b].record == kNone && out.items[b].program == kNone)
                ub = b, ++nb;
        for (size_t r : live)
            if (records[r].promoted == promoted && !used[r])
                ur = r, ++nr;
        if (nb == 1 && nr == 1 && Title(buttons[ub].tip).empty())
            AssignMatch(out, used, ub, ur, Method::Order);
    }
    for (size_t r : live)
        if (!used[r])
            out.items.push_back({kNone, r, kNone, Method::Record});
}
bool ParseUnsigned(std::wstring_view s, int base, uint64_t& out)
{
    if (s.empty())
        return false;
    if (base == 16 && (s.starts_with(L"0x") || s.starts_with(L"0X"))) {
        s.remove_prefix(2);
        if (s.empty())
            return false;
    }
    uint64_t v = 0;
    for (wchar_t c : s) {
        unsigned d = 16;
        if (c >= L'0' && c <= L'9')
            d = unsigned(c - L'0');
        else if (base == 16 && c >= L'a' && c <= L'f')
            d = unsigned(c - L'a' + 10);
        else if (base == 16 && c >= L'A' && c <= L'F')
            d = unsigned(c - L'A' + 10);
        else
            return false;
        if (d >= unsigned(base) || v > (~0ull - d) / unsigned(base))
            return false;
        v = v * unsigned(base) + d;
    }
    out = v;
    return true;
}
bool ParseExplorerUidKey(std::wstring_view recordId, uint32_t& pidOut, uint64_t& hwndOut, uint64_t& uidOut)
{
    if (!recordId.starts_with(L"uid:"))
        return false;
    const std::wstring_view rest = recordId.substr(4);
    const size_t c1 = rest.find(L':');
    const size_t c2 = c1 == std::wstring_view::npos ? std::wstring_view::npos : rest.find(L':', c1 + 1);
    if (c1 == std::wstring_view::npos || c2 == std::wstring_view::npos || rest.find(L':', c2 + 1) != std::wstring_view::npos)
        return false;
    uint64_t pid = 0, hwnd = 0, uid = 0;
    if (!ParseUnsigned(rest.substr(0, c1), 10, pid) || pid > UINT32_MAX || !ParseUnsigned(rest.substr(c1 + 1, c2 - c1 - 1), 10, hwnd) ||
        !ParseUnsigned(rest.substr(c2 + 1), 10, uid))
        return false;
    pidOut = uint32_t(pid);
    hwndOut = hwnd;
    uidOut = uid;
    return true;
}
bool ParseHostHwndKey(std::wstring_view hostKey, uint64_t& hwndOut, uint64_t& uidOut)
{
    if (!hostKey.starts_with(L"hwnd:"))
        return false;
    const std::wstring_view rest = hostKey.substr(5);
    const size_t colon = rest.find(L':');
    if (colon == std::wstring_view::npos || rest.find(L':', colon + 1) != std::wstring_view::npos)
        return false;
    uint64_t hwnd = 0, uid = 0;
    if (!ParseUnsigned(rest.substr(0, colon), 16, hwnd) || !ParseUnsigned(rest.substr(colon + 1), 10, uid))
        return false;
    hwndOut = hwnd;
    uidOut = uid;
    return true;
}
uint64_t UidOf(std::wstring_view id)
{
    const size_t colon = id.rfind(L':');
    uint64_t uid = 0;
    if (colon == std::wstring_view::npos || (!id.starts_with(L"uid:") && !id.starts_with(L"hwnd:")) || !ParseUnsigned(id.substr(colon + 1), 10, uid))
        return ~0ull;
    return uid;
}
std::wstring ExeStem(const std::wstring& exe)
{
    if (exe.empty())
        return {};
    const size_t slash = exe.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
    if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos)
        name.resize(dot);
    return name;
}
bool TipMatches(const std::wstring& expTip, const std::wstring& hostTip)
{
    const auto et = Title(expTip), ek = Key(et);
    const auto ht = Title(hostTip), hk = Key(ht);
    if (ek.empty() || hk.empty())
        return false;
    return ek == hk || DelimitedPrefix(et, hk) || DelimitedPrefix(ht, ek);
}
bool TipContainsTooltipSegment(const std::wstring& expTip, const std::wstring& hostTip)
{
    const auto et = Title(expTip), ht = Title(hostTip), hk = Key(ht);
    return hk.size() >= 4 && DelimitedContains(et, hk);
}
bool HasNonTrivialHostTip(const std::wstring& hostTip)
{
    const auto hk = Key(Title(hostTip));
    return hk.size() >= 6 || std::ranges::any_of(hk, [](wchar_t ch) { return ch >= 0x80; });
}
bool UniqueUnpairedUid(const std::vector<HostedEntry>& hosted, const std::vector<ExplorerEntry>& explorer, const std::vector<size_t>& hostedFallback,
                       const std::vector<bool>& usedExp, size_t hi)
{
    const auto& h = hosted[hi];
    if (h.exe.empty() || !h.hasUid)
        return false;
    size_t hostedCount = 0;
    for (size_t i = 0; i < hosted.size(); ++i)
        if (hostedFallback[i] == kNone && hosted[i].exe == h.exe && hosted[i].hasUid && hosted[i].uid == h.uid)
            ++hostedCount;
    size_t expCount = 0;
    for (size_t i = 0; i < explorer.size(); ++i)
        if (!usedExp[i] && explorer[i].exe == h.exe && UidOf(explorer[i].recordId) == h.uid)
            ++expCount;
    return hostedCount == 1 && expCount == 1;
}
bool HostedMatchesTier(const HostedEntry& h, const ExplorerEntry& e, int tier, bool uniqueUid)
{
    const bool sameExe = !h.exe.empty() && e.exe == h.exe;
    const bool compatExe = e.exe.empty() || h.exe.empty() || e.exe == h.exe;
    switch (tier) {
    case 0: {
        uint32_t ePid = 0;
        uint64_t eHwnd = 0, eUid = 0, hHwnd = 0, hUid = 0;
        const bool hwndUidMatch = compatExe && ParseExplorerUidKey(e.recordId, ePid, eHwnd, eUid) && ParseHostHwndKey(h.key, hHwnd, hUid) && eHwnd != 0 &&
                                  hHwnd != 0 && eHwnd == hHwnd && eUid == hUid && (ePid == 0 || h.pid == 0 || ePid == h.pid);
        return (compatExe && h.key.starts_with(L"guid:") && e.recordId == h.key) || (sameExe && !e.recordId.empty() && e.recordId == h.key) || hwndUidMatch;
    }
    case 1: return sameExe && h.hasUid && uniqueUid && UidOf(e.recordId) == h.uid;
    case 2: return sameExe && !Key(Title(h.tip)).empty() && Key(Title(h.tip)) == Key(Title(e.tip));
    case 3: {
        const auto ek = Key(Title(e.tip));
        return compatExe && !ek.empty() && (ek == Key(Title(h.tip)) || ek == Key(Title(h.fallbackTip)));
    }
    case 4: return compatExe && (TipMatches(e.tip, h.tip) || TipMatches(e.tip, h.fallbackTip));
    case 5:
        return compatExe && ((TipContainsTooltipSegment(e.tip, h.tip) && (!h.exe.empty() || HasNonTrivialHostTip(h.tip))) ||
                             (TipContainsTooltipSegment(e.tip, h.fallbackTip) && (!h.exe.empty() || HasNonTrivialHostTip(h.fallbackTip))));
    case 6: return sameExe;
    default: return false;
    }
}
bool StrictActionTargetCompatible(const HostedEntry& h, const ExplorerEntry& e, int tier)
{
    if (tier > 2 || e.recordId.empty())
        return false;
    const bool compatExe = e.exe.empty() || h.exe.empty() || e.exe == h.exe;
    if (h.key.starts_with(L"guid:"))
        return compatExe && e.recordId == h.key;
    if (e.recordId.starts_with(L"guid:"))
        return false;
    if (tier > 0 && (h.exe.empty() || e.exe != h.exe))
        return false;
    uint32_t ePid = 0;
    uint64_t eHwnd = 0, eUid = 0;
    if (ParseExplorerUidKey(e.recordId, ePid, eHwnd, eUid)) {
        if (ePid != 0 && h.pid != 0 && ePid != h.pid)
            return false;
        if (h.hasUid && eUid != h.uid)
            return false;
        uint64_t hHwnd = 0, hUid = 0;
        if (ParseHostHwndKey(h.key, hHwnd, hUid) && eHwnd != 0 && hHwnd != 0 && eHwnd != hHwnd)
            return false;
    } else if (h.hasUid && UidOf(e.recordId) != h.uid) {
        return false;
    }
    return true;
}
size_t FallbackHostedMatch(const HostedEntry& h, const std::vector<ExplorerEntry>& explorer)
{
    if (h.exe.empty())
        return kNone;
    size_t match = kNone;
    for (size_t ei = 0; ei < explorer.size(); ++ei) {
        if (explorer[ei].exe != h.exe)
            continue;
        uint32_t expPid = 0;
        uint64_t expHwnd = 0, expUid = 0;
        if (ParseExplorerUidKey(explorer[ei].recordId, expPid, expHwnd, expUid) && expPid != 0 && h.pid != 0 && expPid != h.pid)
            continue;
        if (h.hasUid && UidOf(explorer[ei].recordId) == h.uid)
            return ei;
        match = match == kNone ? ei : kNone - 1;
    }
    return match < explorer.size() ? match : kNone;
}
bool HostedClaimsUnknown(const std::vector<HostedEntry>& hosted, const ExplorerEntry& exp)
{
    if (hosted.empty() || exp.hasImage)
        return false;
    if (Key(Title(exp.tip)).empty())
        return true;
    return std::ranges::any_of(hosted, [&](const HostedEntry& h) {
        return TipMatches(exp.tip, h.tip) || TipMatches(exp.tip, h.fallbackTip) || TipMatches(exp.tip, h.productName) || TipMatches(exp.tip, ExeStem(h.exe)) ||
               TipContainsTooltipSegment(exp.tip, h.tip) || TipContainsTooltipSegment(exp.tip, h.fallbackTip);
    });
}
bool HostedSuppressesDuplicateExe(const std::vector<HostedEntry>& hosted, const std::vector<ExplorerEntry>& explorer, const std::vector<size_t>& hostedFallback,
                                  const ExplorerEntry& exp)
{
    if (exp.recordId.starts_with(L"guid:")) {
        for (size_t hi = 0; hi < hosted.size(); ++hi) {
            if (hosted[hi].key == exp.recordId)
                return true;
            if (hostedFallback[hi] != kNone && explorer[hostedFallback[hi]].recordId == exp.recordId)
                return true;
        }
        return false;
    }
    if (exp.exe.empty() || std::ranges::none_of(hosted, [&](const HostedEntry& h) { return !h.exe.empty() && h.exe == exp.exe; }))
        return false;
    uint32_t expPid = 0;
    uint64_t expHwnd = 0, expUid = 0;
    if (!ParseExplorerUidKey(exp.recordId, expPid, expHwnd, expUid) || expPid == 0)
        return true;
    return std::ranges::any_of(hosted, [&](const HostedEntry& h) { return !h.exe.empty() && h.exe == exp.exe && h.pid == expPid; });
}
} // namespace
std::wstring RepairMojibakeTip(const std::wstring& tip)
{
    if (tip.empty() || std::ranges::all_of(tip, [](wchar_t ch) { return ch < 0x80; }))
        return tip;
    std::wstring out;
    out.reserve(tip.size());
    size_t start = 0;
    while (start <= tip.size()) {
        const size_t end = tip.find(L'\n', start);
        const size_t len = (end == std::wstring::npos ? tip.size() : end) - start;
        std::wstring seg = tip.substr(start, len);
        const bool cr = !seg.empty() && seg.back() == L'\r';
        if (cr)
            seg.pop_back();
        out += RepairMojibakeSegment(seg);
        if (cr)
            out += L'\r';
        if (end == std::wstring::npos)
            break;
        out += L'\n';
        start = end + 1;
    }
    return out;
}
const wchar_t* MethodName(Method m)
{
    switch (m) {
    case Method::Geometry: return L"geometry";
    case Method::Name: return L"name";
    case Method::Order: return L"order";
    case Method::Running: return L"running";
    case Method::Record: return L"record";
    default: return L"unknown";
    }
}
Result Match(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<Program>& programs)
{
    Result out;
    out.aliases.resize(records.size(), kNone);
    out.sharedPlace.resize(records.size(), false);
    const auto live = CollectLiveRecords(records, out);
    std::vector<bool> used(records.size(), false);
    for (size_t b = 0; b < buttons.size(); ++b)
        out.items.push_back({b});
    MatchByGeometry(buttons, records, live, used, out);
    MatchByName(buttons, records, live, used, out);
    MatchByOrder(buttons, records, live, used, out);
    MatchRunningPrograms(buttons, programs, out);
    AttachRemaining(buttons, records, live, used, out);
    return out;
}
MergedTrayView MergeHostedIcons(const std::vector<HostedEntry>& hosted, const std::vector<ExplorerEntry>& explorer)
{
    MergedTrayView view;
    view.hostedFallback.assign(hosted.size(), kNone);
    view.hostedTarget.assign(hosted.size(), kNone);
    std::vector<bool> usedExp(explorer.size(), false);
    for (int tier = 0; tier < 7; ++tier)
        for (size_t hi = 0; hi < hosted.size(); ++hi) {
            if (view.hostedFallback[hi] != kNone)
                continue;
            const bool uniqueUid = tier != 1 || UniqueUnpairedUid(hosted, explorer, view.hostedFallback, usedExp, hi);
            for (size_t ei = 0; ei < explorer.size(); ++ei)
                if (!usedExp[ei] && HostedMatchesTier(hosted[hi], explorer[ei], tier, uniqueUid)) {
                    view.hostedFallback[hi] = ei;
                    if (StrictActionTargetCompatible(hosted[hi], explorer[ei], tier))
                        view.hostedTarget[hi] = ei;
                    usedExp[ei] = true;
                    break;
                }
        }
    for (size_t hi = 0; hi < hosted.size(); ++hi)
        if (view.hostedFallback[hi] == kNone)
            view.hostedFallback[hi] = FallbackHostedMatch(hosted[hi], explorer);
    for (size_t ei = 0; ei < explorer.size(); ++ei) {
        if (usedExp[ei])
            continue;
        if (HostedClaimsUnknown(hosted, explorer[ei]) || HostedSuppressesDuplicateExe(hosted, explorer, view.hostedFallback, explorer[ei]))
            continue;
        view.extraExplorer.push_back(ei);
    }
    return view;
}
} // namespace atrium::tray
