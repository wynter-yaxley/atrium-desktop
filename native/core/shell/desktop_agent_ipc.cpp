#include "shell/desktop_agent_ipc.h"

#include "log.h"
#include "win32_path.h"

#include <aclapi.h>
#include <sddl.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr DWORD kPipeChunkBytes = 32 * 1024;
constexpr size_t kMaxIconRawBytes = static_cast<size_t>(kMaxAgentIconDimension) * static_cast<size_t>(kMaxAgentIconDimension) * 4;
constexpr size_t kMaxIconBase64Chars = ((kMaxIconRawBytes + 2) / 3) * 4;

std::wstring Widen(const std::string& s)
{
    const int n = s.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    const int n = w.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string JsonStringify(const json::JsonObject& obj) { return Narrow(std::wstring(obj.Stringify())); }
void SetStr(json::JsonObject& o, const wchar_t* k, const std::wstring& v) { o.SetNamedValue(k, json::JsonValue::CreateStringValue(v)); }
void SetBool(json::JsonObject& o, const wchar_t* k, bool v) { o.SetNamedValue(k, json::JsonValue::CreateBooleanValue(v)); }
template<class T> void SetNum(json::JsonObject& o, const wchar_t* k, T v) { o.SetNamedValue(k, json::JsonValue::CreateNumberValue(static_cast<double>(v))); }

bool ReadBool(const json::JsonObject& o, const wchar_t* k, bool& out, bool req = true)
{
    return !o.HasKey(k) ? !req : (o.GetNamedValue(k).ValueType() == json::JsonValueType::Boolean && (out = o.GetNamedBoolean(k), true));
}

bool ReadString(const json::JsonObject& o, const wchar_t* k, std::wstring& out, size_t maxLen, bool req = true)
{
    if (!o.HasKey(k))
        return !req;
    const auto v = o.GetNamedValue(k);
    if (v.ValueType() != json::JsonValueType::String)
        return false;
    std::wstring s(v.GetString());
    return s.size() <= maxLen && (out = std::move(s), true);
}

bool ReadInt64(const json::JsonObject& o, const wchar_t* k, int64_t& out, bool req = true)
{
    if (!o.HasKey(k))
        return !req;
    const auto v = o.GetNamedValue(k);
    if (v.ValueType() != json::JsonValueType::Number)
        return false;
    const double d = v.GetNumber();
    return std::isfinite(d) && std::floor(d) == d && d >= -9007199254740991.0 && d <= 9007199254740991.0 && (out = static_cast<int64_t>(d), true);
}

template<class T> bool ReadInt(const json::JsonObject& o, const wchar_t* k, T& out, bool req = true)
{
    int64_t v = 0;
    if (!ReadInt64(o, k, v, req))
        return false;
    if (!o.HasKey(k))
        return true;
    return v >= static_cast<int64_t>((std::numeric_limits<T>::min)()) && v <= static_cast<int64_t>((std::numeric_limits<T>::max)()) &&
           (out = static_cast<T>(v), true);
}

bool ReadUint64(const json::JsonObject& o, const wchar_t* k, uint64_t& out, bool req = true)
{
    if (!o.HasKey(k))
        return !req;
    const auto val = o.GetNamedValue(k);
    if (val.ValueType() == json::JsonValueType::Number) {
        const double d = val.GetNumber();
        return std::isfinite(d) && d >= 0.0 && std::floor(d) == d && d <= 9007199254740991.0 && (out = static_cast<uint64_t>(d), true);
    }
    if (val.ValueType() != json::JsonValueType::String)
        return false;
    const std::wstring text(val.GetString());
    if (text.empty() || text.size() > 20 || !std::ranges::all_of(text, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }))
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 10);
    return errno == 0 && end != text.c_str() && *end == L'\0' && (out = static_cast<uint64_t>(parsed), true);
}

bool GetNamedObj(const json::JsonObject& o, const wchar_t* k, json::JsonObject& out)
{
    return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Object && (out = o.GetNamedObject(k), true);
}

bool GetNamedArr(const json::JsonObject& o, const wchar_t* k, json::JsonArray& out)
{
    return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Array && (out = o.GetNamedArray(k), true);
}

json::JsonObject RectToJson(const RECT& r)
{
    json::JsonObject o;
    return SetNum(o, L"left", r.left), SetNum(o, L"top", r.top), SetNum(o, L"right", r.right), SetNum(o, L"bottom", r.bottom), o;
}

bool JsonToRect(const json::JsonObject& o, RECT& out)
{
    return ReadInt(o, L"left", out.left) && ReadInt(o, L"top", out.top) && ReadInt(o, L"right", out.right) && ReadInt(o, L"bottom", out.bottom);
}

json::JsonObject PointToJson(POINT pt)
{
    json::JsonObject o;
    return SetNum(o, L"x", pt.x), SetNum(o, L"y", pt.y), o;
}

bool JsonToPoint(const json::JsonObject& obj, POINT& out) { return ReadInt(obj, L"x", out.x) && ReadInt(obj, L"y", out.y); }

constexpr std::array<std::pair<tray::HostAction, const wchar_t*>, 9> kActionMap{{{tray::HostAction::Left, L"left"},
                                                                                 {tray::HostAction::Right, L"right"},
                                                                                 {tray::HostAction::Middle, L"middle"},
                                                                                 {tray::HostAction::Double, L"double"},
                                                                                 {tray::HostAction::DoubleContinuation, L"double-continuation"},
                                                                                 {tray::HostAction::HoverIn, L"hover-in"},
                                                                                 {tray::HostAction::PopupOpen, L"popup-open"},
                                                                                 {tray::HostAction::PopupClose, L"popup-close"},
                                                                                 {tray::HostAction::Open, L"open"}}};
constexpr std::array<std::pair<HostSendStatus, const wchar_t*>, 3> kSendStatusMap{
    {{HostSendStatus::Sent, L"sent"}, {HostSendStatus::Partial, L"partial"}, {HostSendStatus::NotSent, L"not-sent"}}};
constexpr std::array<std::pair<TrayHostYieldReason, const wchar_t*>, 4> kYieldReasonMap{{{TrayHostYieldReason::None, L"none"},
                                                                                         {TrayHostYieldReason::OtherProcess, L"other-process"},
                                                                                         {TrayHostYieldReason::RapidLoss, L"rapid-loss"},
                                                                                         {TrayHostYieldReason::StartAllBack, L"startallback"}}};

template<class Enum, size_t N> const wchar_t* ToWire(const std::array<std::pair<Enum, const wchar_t*>, N>& map, Enum value)
{
    for (const auto& [k, name] : map)
        if (k == value)
            return name;
    return map.front().second;
}

template<class Enum, size_t N> bool FromWire(const std::array<std::pair<Enum, const wchar_t*>, N>& map, std::wstring_view s, Enum& out)
{
    for (const auto& [k, name] : map)
        if (s == name)
            return out = k, true;
    return false;
}

std::wstring EncodeBase64(const std::vector<uint8_t>& data)
{
    static constexpr wchar_t kAlphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < data.size() ? static_cast<uint32_t>(data[i + 1]) << 8 : 0U) |
                                (i + 2 < data.size() ? static_cast<uint32_t>(data[i + 2]) : 0U);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kAlphabet[(triple >> 6) & 0x3F] : L'=');
        out.push_back((i + 2 < data.size()) ? kAlphabet[triple & 0x3F] : L'=');
    }
    return out;
}

int Base64Val(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') ? ch - L'A' :
           (ch >= L'a' && ch <= L'z') ? 26 + ch - L'a' :
           (ch >= L'0' && ch <= L'9') ? 52 + ch - L'0' :
           ch == L'+'                 ? 62 :
           ch == L'/'                 ? 63 :
                                        -1;
}

bool DecodeBase64(std::wstring_view text, size_t maxDecodedBytes, std::vector<uint8_t>& out)
{
    out.clear();
    if (text.empty())
        return true;
    if (text.size() % 4 != 0 || text.size() > ((maxDecodedBytes + 2) / 3) * 4)
        return false;
    const size_t pad = (text.back() == L'=' ? 1U : 0U) + (text.size() >= 2 && text[text.size() - 2] == L'=' ? 1U : 0U);
    if ((text.size() / 4) * 3 - pad > maxDecodedBytes)
        return false;
    out.reserve((text.size() / 4) * 3 - pad);
    for (size_t i = 0; i < text.size(); i += 4) {
        const int v0 = Base64Val(text[i]), v1 = Base64Val(text[i + 1]);
        const bool p2 = (text[i + 2] == L'='), p3 = (text[i + 3] == L'=');
        if (v0 < 0 || v1 < 0 || (p2 && !p3) || ((p2 || p3) && i + 4 != text.size()))
            return false;
        const int v2 = p2 ? 0 : Base64Val(text[i + 2]), v3 = p3 ? 0 : Base64Val(text[i + 3]);
        if (v2 < 0 || v3 < 0)
            return false;
        const uint32_t t = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);
        out.push_back(static_cast<uint8_t>((t >> 16) & 0xFF));
        if (!p2)
            out.push_back(static_cast<uint8_t>((t >> 8) & 0xFF));
        if (!p3)
            out.push_back(static_cast<uint8_t>(t & 0xFF));
    }
    return true;
}

json::JsonObject TrayIconToJson(const AgentTrayIconDto& icon)
{
    json::JsonObject obj;
    SetStr(obj, L"key", icon.key), SetStr(obj, L"hwnd", std::to_wstring(icon.hwnd)), SetNum(obj, L"uid", icon.uid), SetNum(obj, L"callback", icon.callback);
    SetNum(obj, L"version", icon.version), SetBool(obj, L"showTip", icon.showTip), SetStr(obj, L"tip", icon.tip), SetStr(obj, L"exe", icon.exe);
    SetNum(obj, L"pid", icon.pid), SetStr(obj, L"imageVersion", std::to_wstring(icon.imageVersion)), SetBool(obj, L"hidden", icon.hidden);
    SetStr(obj, L"added", std::to_wstring(icon.added)), SetBool(obj, L"flashing", icon.flashing);
    if (icon.width > 0 && icon.height > 0 && !icon.pixels.empty())
        SetNum(obj, L"width", icon.width), SetNum(obj, L"height", icon.height), SetStr(obj, L"pixels", EncodeBase64(icon.pixels));
    return obj;
}

bool JsonToTrayIcon(const json::JsonObject& obj, AgentTrayIconDto& out)
{
    out = {};
    DWORD uid = 0, cb = 0, ver = 0;
    if (!ReadString(obj, L"key", out.key, kMaxAgentPathLength) || out.key.empty() || !ReadUint64(obj, L"hwnd", out.hwnd) || !ReadInt(obj, L"uid", uid) ||
        !ReadInt(obj, L"callback", cb) || !ReadInt(obj, L"version", ver) || !ReadBool(obj, L"showTip", out.showTip) ||
        !ReadString(obj, L"tip", out.tip, kMaxAgentPathLength) || !ReadString(obj, L"exe", out.exe, kMaxAgentPathLength) || !ReadInt(obj, L"pid", out.pid) ||
        !ReadUint64(obj, L"imageVersion", out.imageVersion) || !ReadBool(obj, L"hidden", out.hidden) || !ReadUint64(obj, L"added", out.added) ||
        !ReadBool(obj, L"flashing", out.flashing)) {
        return false;
    }
    out.uid = static_cast<UINT>(uid), out.callback = static_cast<UINT>(cb), out.version = static_cast<UINT>(ver);
    if (obj.HasKey(L"width") || obj.HasKey(L"height") || obj.HasKey(L"pixels")) {
        std::wstring b64;
        if (!ReadInt(obj, L"width", out.width) || !ReadInt(obj, L"height", out.height) || !ReadString(obj, L"pixels", b64, kMaxIconBase64Chars) ||
            out.width <= 0 || out.height <= 0 || out.width > kMaxAgentIconDimension || out.height > kMaxAgentIconDimension) {
            return false;
        }
        const size_t expectedBytes = static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4;
        if (!DecodeBase64(b64, expectedBytes, out.pixels) || out.pixels.size() != expectedBytes)
            return false;
    }
    return IsValidAgentTrayIconDto(out);
}

bool ParseTrayClickOrHover(const json::JsonObject& o, bool requireAction, AgentCommandMessage& out)
{
    if (!ReadString(o, L"key", out.key, kMaxAgentPathLength))
        return false;
    if (requireAction) {
        std::wstring actionName;
        if (out.key.empty() || !ReadString(o, L"action", actionName, 64) || !FromWire(kActionMap, actionName, out.action))
            return false;
    }
    json::JsonObject atObj{nullptr}, cellObj{nullptr};
    out.type = requireAction ? AgentCommandType::TraySend : AgentCommandType::TrayHover;
    return GetNamedObj(o, L"at", atObj) && GetNamedObj(o, L"cell", cellObj) && JsonToPoint(atObj, out.at) && JsonToRect(cellObj, out.cell);
}

bool ParseSyncDropBars(const json::JsonObject& o, AgentCommandMessage& out)
{
    json::JsonArray arr{nullptr};
    if (!GetNamedArr(o, L"bars", arr) || arr.Size() > kMaxAgentDropBars)
        return false;
    out.type = AgentCommandType::SyncDropBars, out.bars.reserve(arr.Size());
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        const auto v = arr.GetAt(i);
        json::JsonObject rectObj{nullptr};
        AgentBarRect bar;
        if (v.ValueType() != json::JsonValueType::Object || !ReadUint64(v.GetObject(), L"id", bar.id) || !ReadBool(v.GetObject(), L"visible", bar.visible) ||
            !GetNamedObj(v.GetObject(), L"rect", rectObj) || !JsonToRect(rectObj, bar.rect))
            return false;
        out.bars.push_back(bar);
    }
    return true;
}

bool ParseTraySnapshotEvent(const json::JsonObject& o, AgentEventMessage& out)
{
    std::wstring reasonText;
    json::JsonArray arr{nullptr};
    if (!ReadBool(o, L"yielded", out.snapshot.yielded) || !ReadString(o, L"yieldReason", reasonText, 64) ||
        !FromWire(kYieldReasonMap, reasonText, out.snapshot.yieldReason) || !GetNamedArr(o, L"icons", arr) || arr.Size() > kMaxAgentTrayIcons)
        return false;
    out.type = AgentEventType::TraySnapshot, out.snapshot.icons.reserve(arr.Size());
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        AgentTrayIconDto icon;
        if (arr.GetAt(i).ValueType() != json::JsonValueType::Object || !JsonToTrayIcon(arr.GetAt(i).GetObject(), icon))
            return false;
        out.snapshot.icons.push_back(std::move(icon));
    }
    return true;
}

bool ParseDropEvent(const json::JsonObject& o, AgentEventMessage& out)
{
    json::JsonArray arr{nullptr};
    if (!ReadString(o, L"key", out.key, kMaxAgentPathLength) || out.key.empty() || !GetNamedArr(o, L"paths", arr) || arr.Size() == 0 ||
        arr.Size() > kMaxAgentDropPaths)
        return false;
    out.type = AgentEventType::Drop, out.paths.reserve(arr.Size());
    for (uint32_t i = 0; i < arr.Size(); ++i) {
        const auto v = arr.GetAt(i);
        if (v.ValueType() != json::JsonValueType::String)
            return false;
        std::wstring p(v.GetString());
        if (p.empty() || p.size() > kMaxAgentPathLength)
            return false;
        out.paths.push_back(std::move(p));
    }
    return IsValidAgentDropPayload(out.key, out.paths);
}

bool WaitOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, DWORD timeoutMs, DWORD& bytes, DWORD& error)
{
    HANDLE handles[2] = {ov.hEvent, quitEvent};
    const DWORD wait = WaitForMultipleObjects(quitEvent ? 2U : 1U, handles, FALSE, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &bytes, TRUE);
        error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
        return false;
    }
    error = GetOverlappedResult(pipe, &ov, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    return error == ERROR_SUCCESS || error == ERROR_MORE_DATA;
}

DWORD RemainingTimeout(ULONGLONG deadline, DWORD totalTimeoutMs)
{
    if (totalTimeoutMs == INFINITE)
        return INFINITE;
    const ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

bool VerifyImpersonatedClientSid(HANDLE pipe, const std::wstring& expectedSid)
{
    if (!ImpersonateNamedPipeClient(pipe))
        return false;
    winrt::handle token;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, token.put()) != FALSE;
    RevertToSelf();
    DWORD size = 0;
    if (!opened)
        return false;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    wchar_t* sidText = nullptr;
    const bool ok = size > 0 && GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size) &&
                    ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(buffer.data())->User.Sid, &sidText) && sidText &&
                    _wcsicmp(sidText, expectedSid.c_str()) == 0;
    if (sidText)
        LocalFree(sidText);
    return ok;
}

bool VerifyPipeOwnerSameUser(HANDLE pipe, const std::wstring& expectedSid)
{
    PSID ownerSid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &ownerSid, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS || !ownerSid) {
        if (sd)
            LocalFree(sd);
        return false;
    }
    wchar_t* sidText = nullptr;
    const bool ownerOk = ConvertSidToStringSidW(ownerSid, &sidText) && sidText && _wcsicmp(sidText, expectedSid.c_str()) == 0;
    if (sidText)
        LocalFree(sidText);
    LocalFree(sd);
    return ownerOk;
}

bool VerifyConnectedServerSameUser(HANDLE pipe)
{
    const auto& self = SelfSecurityContext();
    if (!self.valid || !VerifyPipeOwnerSameUser(pipe, self.userSid))
        return false;
    ULONG serverPid = 0;
    if (GetNamedPipeServerProcessId(pipe, &serverPid) && serverPid != 0) {
        DWORD err = 0;
        const ProcessSecurityContext server = QueryPidSecurityContext(serverPid, &err);
        return server.valid ? _wcsicmp(server.userSid.c_str(), self.userSid.c_str()) == 0 : (err == ERROR_ACCESS_DENIED);
    }
    return true;
}

std::wstring MakePipeName(const wchar_t* kind, DWORD sessionId, DWORD corePid, uint64_t generation)
{
    return std::wstring(L"\\\\.\\pipe\\Atrium.DesktopAgent.") + kind + L"." + std::to_wstring(sessionId) + L"." + std::to_wstring(corePid) + L"." +
           std::to_wstring(generation);
}

} // namespace

DWORD CurrentProcessSessionId()
{
    DWORD session = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &session), session;
}

std::wstring DesktopAgentCommandPipeName(DWORD sessionId, DWORD corePid, uint64_t generation) { return MakePipeName(L"Cmd", sessionId, corePid, generation); }
std::wstring DesktopAgentEventPipeName(DWORD sessionId, DWORD corePid, uint64_t generation) { return MakePipeName(L"Evt", sessionId, corePid, generation); }

bool IsValidAgentTrayIconDto(const AgentTrayIconDto& icon)
{
    if (icon.key.empty() || icon.key.size() > kMaxAgentPathLength || icon.tip.size() > kMaxAgentPathLength || icon.exe.size() > kMaxAgentPathLength) {
        return false;
    }
    if (icon.width == 0 && icon.height == 0)
        return icon.pixels.empty();
    if (icon.width <= 0 || icon.height <= 0 || icon.width > kMaxAgentIconDimension || icon.height > kMaxAgentIconDimension)
        return false;
    return icon.pixels.size() == static_cast<size_t>(icon.width) * static_cast<size_t>(icon.height) * 4;
}

bool IsValidAgentDropPayload(const std::wstring& key, const std::vector<std::wstring>& paths)
{
    if (key.empty() || key.size() > kMaxAgentPathLength || paths.empty() || paths.size() > kMaxAgentDropPaths)
        return false;
    return std::ranges::all_of(paths, [](const std::wstring& p) { return !p.empty() && p.size() <= kMaxAgentPathLength; });
}

std::string SerializeAgentCommand(const AgentCommandMessage& msg)
{
    json::JsonObject o;
    switch (msg.type) {
    case AgentCommandType::Ping: SetStr(o, L"cmd", L"ping"); break;
    case AgentCommandType::Quit: SetStr(o, L"cmd", L"quit"); break;
    case AgentCommandType::SetTrayHosting: SetStr(o, L"cmd", L"set-tray-hosting"), SetBool(o, L"hosting", msg.hosting); break;
    case AgentCommandType::SetTrayPlaces: SetStr(o, L"cmd", L"set-tray-places"), o.SetNamedValue(L"anchor", RectToJson(msg.anchor)); break;
    case AgentCommandType::TraySend:
    case AgentCommandType::TrayHover:
        SetStr(o, L"cmd", msg.type == AgentCommandType::TraySend ? L"tray-send" : L"tray-hover"), SetStr(o, L"key", msg.key);
        if (msg.type == AgentCommandType::TraySend)
            SetStr(o, L"action", ToWire(kActionMap, msg.action));
        o.SetNamedValue(L"at", PointToJson(msg.at)), o.SetNamedValue(L"cell", RectToJson(msg.cell));
        break;
    case AgentCommandType::SyncDropBars: {
        SetStr(o, L"cmd", L"sync-drop-bars");
        json::JsonArray arr;
        for (size_t i = 0; i < (std::min)(msg.bars.size(), kMaxAgentDropBars); ++i) {
            json::JsonObject item;
            SetStr(item, L"id", std::to_wstring(msg.bars[i].id)), item.SetNamedValue(L"rect", RectToJson(msg.bars[i].rect)),
                SetBool(item, L"visible", msg.bars[i].visible), arr.Append(item);
        }
        o.SetNamedValue(L"bars", arr);
        break;
    }
    }
    if (msg.opId != 0)
        SetStr(o, L"opId", std::to_wstring(msg.opId));
    return JsonStringify(o);
}

bool ValidateAndParseAgentCommand(const std::string& raw, AgentCommandMessage& out)
{
    if (raw.empty() || raw.size() > kMaxAgentMessageBytes)
        return false;
    json::JsonObject o{nullptr};
    std::wstring cmd;
    if (!json::JsonObject::TryParse(Widen(raw), o) || !ReadString(o, L"cmd", cmd, 64) || cmd.empty())
        return false;
    out = {};
    if (!ReadUint64(o, L"opId", out.opId, false))
        return false;
    if (cmd == L"ping")
        return out.type = AgentCommandType::Ping, true;
    if (cmd == L"quit")
        return out.type = AgentCommandType::Quit, true;
    if (cmd == L"set-tray-hosting")
        return out.type = AgentCommandType::SetTrayHosting, ReadBool(o, L"hosting", out.hosting);
    if (cmd == L"set-tray-places") {
        json::JsonObject anchorObj{nullptr};
        return out.type = AgentCommandType::SetTrayPlaces, GetNamedObj(o, L"anchor", anchorObj) && JsonToRect(anchorObj, out.anchor);
    }
    if (cmd == L"tray-send")
        return ParseTrayClickOrHover(o, true, out);
    if (cmd == L"tray-hover")
        return ParseTrayClickOrHover(o, false, out);
    return cmd == L"sync-drop-bars" && ParseSyncDropBars(o, out);
}

std::string SerializeAgentCommandReply(const AgentCommandReply& reply)
{
    json::JsonObject o;
    SetBool(o, L"ok", reply.ok), SetStr(o, L"status", ToWire(kSendStatusMap, reply.sendStatus));
    if (reply.pid != 0)
        SetNum(o, L"pid", reply.pid);
    if (reply.generation != 0)
        SetStr(o, L"generation", std::to_wstring(reply.generation));
    if (reply.higherIntegrity)
        SetBool(o, L"higherIntegrity", true);
    if (!reply.error.empty())
        SetStr(o, L"error", reply.error);
    return JsonStringify(o);
}

bool ValidateAndParseAgentCommandReply(const std::string& raw, AgentCommandReply& out)
{
    if (raw.empty() || raw.size() > kMaxAgentMessageBytes)
        return false;
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(raw), o))
        return false;
    out = {};
    std::wstring statusText;
    if (!ReadBool(o, L"ok", out.ok) || !ReadInt(o, L"pid", out.pid, false) || !ReadUint64(o, L"generation", out.generation, false) ||
        !ReadBool(o, L"higherIntegrity", out.higherIntegrity, false) || !ReadString(o, L"error", out.error, kMaxAgentPathLength, false) ||
        !ReadString(o, L"status", statusText, 64, false)) {
        return false;
    }
    return statusText.empty() || FromWire(kSendStatusMap, statusText, out.sendStatus);
}

std::string SerializeAgentEvent(const AgentEventMessage& msg)
{
    json::JsonObject o;
    switch (msg.type) {
    case AgentEventType::TraySnapshot: {
        SetStr(o, L"evt", L"tray-snapshot"), SetBool(o, L"yielded", msg.snapshot.yielded),
            SetStr(o, L"yieldReason", ToWire(kYieldReasonMap, msg.snapshot.yieldReason));
        json::JsonArray arr;
        for (size_t i = 0; i < (std::min)(msg.snapshot.icons.size(), kMaxAgentTrayIcons); ++i)
            arr.Append(TrayIconToJson(msg.snapshot.icons[i]));
        o.SetNamedValue(L"icons", arr);
        break;
    }
    case AgentEventType::TrayYield:
        SetStr(o, L"evt", L"tray-yield"), SetBool(o, L"yielded", msg.yielded), SetStr(o, L"yieldReason", ToWire(kYieldReasonMap, msg.yieldReason));
        break;
    case AgentEventType::DragOver: SetStr(o, L"evt", L"drag-over"), o.SetNamedValue(L"pt", PointToJson(msg.pt)); break;
    case AgentEventType::DragLeave: SetStr(o, L"evt", L"drag-leave"); break;
    case AgentEventType::Drop: {
        SetStr(o, L"evt", L"drop"), SetStr(o, L"key", msg.key);
        json::JsonArray arr;
        for (size_t i = 0; i < (std::min)(msg.paths.size(), kMaxAgentDropPaths); ++i)
            arr.Append(json::JsonValue::CreateStringValue(msg.paths[i]));
        o.SetNamedValue(L"paths", arr);
        break;
    }
    }
    if (msg.opId != 0)
        SetStr(o, L"opId", std::to_wstring(msg.opId));
    return JsonStringify(o);
}

bool ValidateAndParseAgentEvent(const std::string& raw, AgentEventMessage& out)
{
    if (raw.empty() || raw.size() > kMaxAgentMessageBytes)
        return false;
    json::JsonObject o{nullptr};
    std::wstring evt;
    out = {};
    if (!json::JsonObject::TryParse(Widen(raw), o) || !ReadString(o, L"evt", evt, 64) || evt.empty() || !ReadUint64(o, L"opId", out.opId, false))
        return false;
    if (evt == L"tray-snapshot")
        return ParseTraySnapshotEvent(o, out);
    if (evt == L"tray-yield") {
        std::wstring reasonText;
        return out.type = AgentEventType::TrayYield,
               ReadBool(o, L"yielded", out.yielded) && ReadString(o, L"yieldReason", reasonText, 64) && FromWire(kYieldReasonMap, reasonText, out.yieldReason);
    }
    if (evt == L"drag-over") {
        json::JsonObject ptObj{nullptr};
        return out.type = AgentEventType::DragOver, GetNamedObj(o, L"pt", ptObj) && JsonToPoint(ptObj, out.pt);
    }
    return evt == L"drag-leave" ? (out.type = AgentEventType::DragLeave, true) : (evt == L"drop" && ParseDropEvent(o, out));
}

std::string SerializeAgentEventReply(const AgentEventReply& reply)
{
    json::JsonObject o;
    SetBool(o, L"ok", reply.ok), SetBool(o, L"allow", reply.allow);
    if (!reply.key.empty())
        SetStr(o, L"key", reply.key);
    if (!reply.error.empty())
        SetStr(o, L"error", reply.error);
    return JsonStringify(o);
}

bool ValidateAndParseAgentEventReply(const std::string& raw, AgentEventReply& out)
{
    if (raw.empty() || raw.size() > kMaxAgentMessageBytes)
        return false;
    json::JsonObject o{nullptr};
    if (!json::JsonObject::TryParse(Widen(raw), o))
        return false;
    out = {};
    return ReadBool(o, L"ok", out.ok) && ReadBool(o, L"allow", out.allow, false) && ReadString(o, L"key", out.key, kMaxAgentPathLength, false) &&
           ReadString(o, L"error", out.error, kMaxAgentPathLength, false);
}

HANDLE CreateSameUserNamedPipeServer(const std::wstring& pipeName, DWORD bufferSize)
{
    KernelObjectSecurity security = KernelObjectSecurity::CreateSameUserPipe();
    if (!security.Valid()) {
        Log(L"desktop agent ipc: CreateSameUserPipe failed (%lu); failing closed", GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    return CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, bufferSize, bufferSize, 0, security.Get());
}

bool VerifyNamedPipeClientSameUser(HANDLE pipe)
{
    const auto& self = SelfSecurityContext();
    if (!self.valid)
        return false;
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || !clientPid)
        return false;
    DWORD err = 0;
    const ProcessSecurityContext client = QueryPidSecurityContext(clientPid, &err);
    if (client.valid)
        return _wcsicmp(client.userSid.c_str(), self.userSid.c_str()) == 0;
    return err == ERROR_ACCESS_DENIED && VerifyImpersonatedClientSid(pipe, self.userSid);
}

bool VerifyNamedPipePeerProcess(HANDLE pipe, bool isServerSide, const std::wstring& expectedImagePath, bool requireNonElevated, AgentPeerIdentity& out,
                                winrt::handle* procOut)
{
    out = {};
    if (procOut)
        *procOut = {};
    const auto& self = SelfSecurityContext();
    if (!self.valid || !pipe || pipe == INVALID_HANDLE_VALUE)
        return false;
    ULONG peerPid = 0;
    const BOOL gotPid = isServerSide ? GetNamedPipeClientProcessId(pipe, &peerPid) : GetNamedPipeServerProcessId(pipe, &peerPid);
    if (!gotPid || peerPid == 0)
        return false;
    winrt::handle proc{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, peerPid)};
    if (!proc)
        return false;
    FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
    if (!GetProcessTimes(proc.get(), &creation, &exitTime, &kernelTime, &userTime))
        return false;
    std::wstring imagePath = ProcessImagePath(proc.get());
    if (imagePath.empty() || (!expectedImagePath.empty() && FoldCase(imagePath) != FoldCase(expectedImagePath)))
        return false;
    DWORD secErr = 0;
    ProcessSecurityContext security = QueryProcessSecurityContext(proc.get(), &secErr);
    if (security.valid) {
        if (_wcsicmp(security.userSid.c_str(), self.userSid.c_str()) != 0 || security.sessionId != self.sessionId)
            return false;
        if (requireNonElevated && (security.isElevated || IsElevatedIntegrityRid(security.integrityRid)))
            return false;
    } else {
        if (requireNonElevated || secErr != ERROR_ACCESS_DENIED)
            return false;
        const bool pipeUserOk = isServerSide ? VerifyImpersonatedClientSid(pipe, self.userSid) : VerifyPipeOwnerSameUser(pipe, self.userSid);
        DWORD peerSession = 0;
        if (!pipeUserOk || !ProcessIdToSessionId(peerPid, &peerSession) || peerSession != self.sessionId)
            return false;
    }
    out = AgentPeerIdentity{peerPid, creation, std::move(imagePath), std::move(security), true};
    if (procOut)
        *procOut = std::move(proc);
    return true;
}

bool ConnectNamedPipeOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent)
{
    ResetEvent(ov.hEvent);
    if (ConnectNamedPipe(pipe, &ov))
        return true;
    DWORD bytes = 0, error = GetLastError();
    if (error == ERROR_IO_PENDING)
        return WaitOverlapped(pipe, ov, quitEvent, INFINITE, bytes, error);
    return error == ERROR_PIPE_CONNECTED || error == ERROR_SUCCESS;
}

bool ReadPipeMessageTimed(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, DWORD timeoutMs, std::string& messageOut, bool& disconnectedOut)
{
    messageOut.clear();
    disconnectedOut = false;
    const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
    std::string buffer(kPipeChunkBytes, '\0');
    DWORD error = 0;
    do {
        ResetEvent(ov.hEvent);
        DWORD bytes = 0;
        if (!ReadFile(pipe, buffer.data(), kPipeChunkBytes, &bytes, &ov)) {
            error = GetLastError();
            const bool pendingFail = (error == ERROR_IO_PENDING && !WaitOverlapped(pipe, ov, quitEvent, RemainingTimeout(deadline, timeoutMs), bytes, error));
            if (pendingFail || (error != ERROR_IO_PENDING && error != ERROR_MORE_DATA && error != ERROR_SUCCESS)) {
                disconnectedOut = (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA);
                return false;
            }
        } else {
            error = ERROR_SUCCESS;
        }
        if (messageOut.size() + bytes > kMaxAgentMessageBytes)
            return false;
        messageOut.append(buffer.data(), bytes);
    } while (error == ERROR_MORE_DATA);
    return !messageOut.empty();
}

bool WritePipeMessageTimed(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, DWORD timeoutMs, const std::string& message)
{
    if (message.size() > kMaxAgentMessageBytes)
        return false;
    ResetEvent(ov.hEvent);
    DWORD bytes = 0;
    if (!WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &bytes, &ov)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING || !WaitOverlapped(pipe, ov, quitEvent, timeoutMs, bytes, error))
            return false;
    }
    return bytes == message.size();
}

bool WritePipeMessageOverlapped(HANDLE pipe, OVERLAPPED& ov, HANDLE quitEvent, const std::string& message)
{
    return WritePipeMessageTimed(pipe, ov, quitEvent, INFINITE, message);
}

HANDLE ConnectSameUserPipeClient(const std::wstring& pipeName, DWORD timeoutMs, HANDLE quitEvent)
{
    for (const ULONGLONG deadline = GetTickCount64() + timeoutMs;;) {
        if (quitEvent && WaitForSingleObject(quitEvent, 0) != WAIT_TIMEOUT)
            return INVALID_HANDLE_VALUE;
        HANDLE pipe = CreateFileW(pipeName.c_str(), kPipeClientAccessMask, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) || !VerifyConnectedServerSameUser(pipe))
                return CloseHandle(pipe), INVALID_HANDLE_VALUE;
            return pipe;
        }
        const DWORD err = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline || (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND))
            return INVALID_HANDLE_VALUE;
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(pipeName.c_str(), remaining > 100 ? 100 : remaining);
        } else {
            const DWORD step = (std::min)(DWORD(25), remaining);
            if (quitEvent && WaitForSingleObject(quitEvent, step) != WAIT_TIMEOUT)
                return INVALID_HANDLE_VALUE;
            if (!quitEvent)
                std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
    }
}

bool SendPipeRequestOverlapped(HANDLE pipe, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent)
{
    replyOut.clear();
    const winrt::handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (pipe == INVALID_HANDLE_VALUE || request.empty() || request.size() > kMaxAgentMessageBytes || !event)
        return false;
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    OVERLAPPED ov{};
    ov.hEvent = event.get();
    if (!WritePipeMessageTimed(pipe, ov, quitEvent, RemainingTimeout(deadline, timeoutMs), request))
        return false;
    bool disconnected = false;
    const DWORD remaining = RemainingTimeout(deadline, timeoutMs);
    return ReadPipeMessageTimed(pipe, ov, quitEvent, remaining > 0 ? remaining : 1, replyOut, disconnected);
}

bool SendNamedPipeOneShot(const std::wstring& pipeName, const std::string& request, std::string& replyOut, DWORD timeoutMs, HANDLE quitEvent)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    const winrt::handle pipe{ConnectSameUserPipeClient(pipeName, timeoutMs, quitEvent)};
    if (pipe.get() == INVALID_HANDLE_VALUE || !pipe)
        return false;
    const DWORD remaining = RemainingTimeout(deadline, timeoutMs);
    return SendPipeRequestOverlapped(pipe.get(), request, replyOut, remaining > 0 ? remaining : 1, quitEvent);
}

} // namespace atrium
