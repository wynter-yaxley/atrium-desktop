// Value-only notification-area reconciliation. No Windows headers or calls.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atrium::tray {
inline constexpr size_t kNone = static_cast<size_t>(-1);
struct Rect {
    long left = 0, top = 0, right = 0, bottom = 0;
    bool operator==(const Rect&) const = default;
    bool Valid() const { return right > left && bottom > top; }
};
struct Button {
    std::wstring tip;
    bool promoted = false;
    Rect bounds;
};
struct Record {
    std::wstring id, exe, tip;
    // GUID, or the HWND/PID/uID that actually passed the shell probe. Never exe/rect.
    std::wstring identity;
    bool live = false, promoted = false;
    size_t order = kNone;
    Rect bounds;
    std::vector<std::wstring> names;
    size_t firstTitle = kNone;
};
struct Program {
    std::wstring exe;
    std::vector<std::wstring> names;
    size_t firstTitle = kNone;
};
enum class Method { Unknown, Geometry, Name, Order, Running, Record };
const wchar_t* MethodName(Method method);
struct Pair {
    size_t button = kNone, record = kNone, program = kNone;
    Method method = Method::Unknown;
};
struct Result {
    std::vector<Pair> items; // UIA order, then unmatched live records in UIOrderList order
    std::vector<size_t> aliases; // canonical record index; none for dead/unprobeable records
    std::vector<bool> sharedPlace;
};
struct ShellTrayKnown {
    std::vector<std::wstring> aliases;
    const wchar_t* iconDll = nullptr;
};
struct HostedEntry {
    std::wstring key;
    std::wstring exe;
    std::wstring tip;
    std::wstring fallbackTip;
    std::wstring productName;
    uint64_t uid = 0;
    bool hasUid = false;
    uint32_t pid = 0;
};
struct ExplorerEntry {
    std::wstring recordId;
    std::wstring exe;
    std::wstring tip;
    bool hasImage = false;
};
struct MergedTrayView {
    std::vector<size_t> hostedFallback; // display metadata (icon/tip/order); may be weak/shared
    std::vector<size_t> hostedTarget;   // 1-to-1 strict identity-compatible Explorer action target
    std::vector<size_t> extraExplorer;
};
bool IsShellExplorerExe(const std::wstring& exe);
std::optional<ShellTrayKnown> KnownShellTray(const std::wstring& guidOrId);
std::wstring RepairMojibakeTip(const std::wstring& tip);
std::wstring FallbackRecordTip(const Record& r);
std::wstring Title(const std::wstring& tip);
std::wstring Key(const std::wstring& text);
Result Match(const std::vector<Button>& buttons, const std::vector<Record>& records, const std::vector<Program>& programs = {});
MergedTrayView MergeHostedIcons(const std::vector<HostedEntry>& hosted, const std::vector<ExplorerEntry>& explorer);
} // namespace atrium::tray
