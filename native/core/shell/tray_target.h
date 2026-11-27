// Value-only click identity checks. UIA runtime IDs are not permanent icon identities.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

namespace atrium::tray {
struct ShellSession {
    uint32_t pid = 0;
    uint64_t started = 0, window = 0;
    bool operator==(const ShellSession&) const = default;
    bool Valid() const { return pid && started && window; }
};
struct TargetKey {
    ShellSession shell;
    std::vector<int> runtime;
    std::wstring identity, exe, tip; // adapter supplies normalized full exe paths
    bool operator==(const TargetKey&) const = default;
};
enum class Resolution { Runtime, Record, Program, Missing, Ambiguous, Conflict, ShellChanged };
inline const wchar_t* ResolutionName(Resolution value)
{
    switch (value) {
    case Resolution::Runtime: return L"runtime";
    case Resolution::Record: return L"record";
    case Resolution::Program: return L"program";
    case Resolution::Ambiguous: return L"ambiguous";
    case Resolution::Conflict: return L"identity-conflict";
    case Resolution::ShellChanged: return L"shell-changed";
    default: return L"missing";
    }
}
struct ResolvedTarget {
    size_t index = static_cast<size_t>(-1);
    Resolution reason = Resolution::Missing;
};
inline ResolvedTarget ResolveTarget(const TargetKey& request, const ShellSession& current, const std::vector<TargetKey>& candidates)
{
    const bool runtime = !request.runtime.empty(), record = !request.identity.empty();
    if ((runtime || record) && (!current.Valid() || request.shell != current))
        return {static_cast<size_t>(-1), Resolution::ShellChanged};
    ResolvedTarget result;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.shell != current)
            continue;
        const bool same = runtime ? candidate.runtime == request.runtime :
                          record  ? candidate.identity == request.identity :
                                    !request.exe.empty() && candidate.exe == request.exe;
        if (!same)
            continue;
        if (result.index != static_cast<size_t>(-1))
            return {static_cast<size_t>(-1), Resolution::Ambiguous};
        result = {i, runtime ? Resolution::Runtime : record ? Resolution::Record : Resolution::Program};
    }
    if (result.index == static_cast<size_t>(-1))
        return result;
    const auto& candidate = candidates[result.index];
    // Runtime and record evidence must agree. Never jump to a different runtime because
    // name/order matching has moved a cached registration there during this click.
    if ((record && candidate.identity != request.identity) || (!request.exe.empty() && candidate.exe != request.exe) ||
        (runtime && !record && candidate.tip != request.tip))
        return {static_cast<size_t>(-1), Resolution::Conflict};
    return result;
}
inline bool ShouldAllowExeForegroundFallback(bool allowProgramWake) { return allowProgramWake; }
inline bool SameGestureTarget(const TargetKey& a, const TargetKey& b)
{
    if (!a.shell.Valid() || a.shell != b.shell || a.exe != b.exe || a.identity != b.identity)
        return false;
    if (!a.runtime.empty() || !b.runtime.empty())
        return !a.runtime.empty() && a.runtime == b.runtime && (!a.identity.empty() || a.tip == b.tip);
    return !a.identity.empty();
}
inline std::vector<size_t> StableOrder(const std::vector<TargetKey>& previous, const std::vector<TargetKey>& fresh)
{
    std::vector<size_t> order;
    std::vector<bool> used(fresh.size());
    for (const auto& old : previous) {
        size_t found = static_cast<size_t>(-1), count = 0;
        for (size_t i = 0; i < fresh.size(); ++i) {
            const auto& next = fresh[i];
            if (used[i] || !old.shell.Valid() || old.shell != next.shell)
                continue;
            const bool same =
                !old.identity.empty() && !next.identity.empty() ? old.identity == next.identity : !old.runtime.empty() && old.runtime == next.runtime;
            if (same) {
                found = i;
                ++count;
            }
        }
        if (count == 1) {
            used[found] = true;
            order.push_back(found);
        }
    }
    for (size_t i = 0; i < fresh.size(); ++i)
        if (!used[i])
            order.push_back(i);
    return order;
}
} // namespace atrium::tray
