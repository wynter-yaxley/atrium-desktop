// Long-path Win32 module/process image queries and Unicode path case folding.
#pragma once

#include <windows.h>
#include <appmodel.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>

namespace atrium {

inline std::wstring ModulePath(HMODULE module = nullptr)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
        if (len == 0)
            return {};
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        if (path.size() >= 32768)
            return {};
        const size_t next = path.size() * 2;
        path.resize(next > 32768 ? 32768 : next);
    }
}

inline std::wstring ProcessImagePath(HANDLE process)
{
    if (!process)
        return {};
    wchar_t stack[MAX_PATH * 2];
    DWORD len = DWORD(sizeof(stack) / sizeof(stack[0]));
    if (QueryFullProcessImageNameW(process, 0, stack, &len))
        return std::wstring(stack, len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring path(32768, L'\0');
    len = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &len))
        return {};
    path.resize(len);
    return path;
}

inline std::wstring ProcessImagePath(DWORD pid)
{
    struct ScopedHandle {
        HANDLE handle;
        explicit ScopedHandle(HANDLE h) : handle(h) {}
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        ~ScopedHandle()
        {
            if (handle)
                CloseHandle(handle);
        }
    };
    const ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    return ProcessImagePath(process.handle);
}

inline std::wstring FoldCase(const std::wstring& text)
{
    if (text.empty())
        return text;
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0)
        return text;
    std::wstring folded(size_t(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()), folded.data(), count, nullptr, nullptr, 0) <= 0)
        return text;
    return folded;
}

inline std::wstring ExeStemFolded(const std::wstring& path)
{
    std::wstring folded = FoldCase(path);
    const size_t slash = folded.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        folded = folded.substr(slash + 1);
    if (folded.size() > 4 && folded.ends_with(L".exe"))
        folded.resize(folded.size() - 4);
    return folded;
}

inline std::wstring ProcessAumid(HANDLE process)
{
    if (!process)
        return {};
    UINT32 len = 0;
    if (GetApplicationUserModelId(process, &len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return {};
    std::wstring aumid(len, L'\0');
    if (GetApplicationUserModelId(process, &len, aumid.data()) != ERROR_SUCCESS)
        return {};
    while (!aumid.empty() && aumid.back() == L'\0')
        aumid.pop_back();
    return aumid;
}

inline bool SameExecutableWindow(HWND window, const std::wstring& exePath = ModulePath())
{
    if (!window || exePath.empty())
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
        return false;
    const std::wstring other = ProcessImagePath(pid);
    return !other.empty() && FoldCase(other) == FoldCase(exePath);
}

inline HWND FindWindowForExecutable(const wchar_t* className, const std::wstring& exePath = ModulePath())
{
    if (!className || exePath.empty())
        return nullptr;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, className, nullptr)) != nullptr;) {
        if (SameExecutableWindow(w, exePath))
            return w;
    }
    return nullptr;
}

inline bool IsInvisibleOrFormatChar(wchar_t ch)
{
    if (ch < 0x20)
        return ch != L'\t' && ch != L'\r' && ch != L'\n';
    if ((ch >= 0x0080 && ch <= 0x009F) || (ch >= 0x200B && ch <= 0x200F) || (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2060 && ch <= 0x2069))
        return true;
    return ch == 0xFEFF;
}

// Strips BiDi isolate/embedding marks (which bundled MiSans gives non-zero width) and control chars.
inline std::wstring StripInvisibleUiChars(std::wstring_view text)
{
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text)
        if (!IsInvisibleOrFormatChar(ch))
            out.push_back(ch);
    return out;
}

inline std::wstring TrimAsciiWhitespace(std::wstring_view s)
{
    const size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos)
        return {};
    const size_t last = s.find_last_not_of(L" \t\r\n");
    return std::wstring(s.substr(first, last - first + 1));
}

inline bool TryDecodeUtf8Validated(const char* bytes, int len, const std::wstring& original, std::wstring& decodedOut)
{
    if (len <= 0)
        return false;
    const int utf16Chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, nullptr, 0);
    if (utf16Chars <= 0)
        return false;
    std::wstring decoded(size_t(utf16Chars), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, decoded.data(), utf16Chars) != utf16Chars || decoded == original)
        return false;
    const bool inputHasEastAsian = std::ranges::any_of(original, [](wchar_t ch) { return ch >= 0x2E80; });
    const bool inputHasPua = std::ranges::any_of(original, [](wchar_t ch) { return ch >= 0xE000 && ch <= 0xF8FF; });
    bool decodedHasHigh = false;
    for (wchar_t ch : decoded) {
        if ((ch >= 0x0080 && ch <= 0x009F) || ch == 0xFFFD || (ch < 0x20 && ch != L'\t') || (!inputHasPua && ch >= 0xE000 && ch <= 0xF8FF))
            return false;
        if (ch >= 0x0800)
            decodedHasHigh = true;
    }
    if (inputHasEastAsian && !decodedHasHigh)
        return false;
    decodedOut = std::move(decoded);
    return true;
}

// Reverses single-pass `MultiByteToWideChar(codepage -> UTF-16)` decoding of UTF-8 bytes back to UTF-16.
inline std::wstring RepairUtf8Mojibake(const std::wstring& seg, UINT codepage, bool allowTruncatedFat11 = false)
{
    if (seg.empty() || codepage == 0 || codepage == CP_UTF8 || std::ranges::all_of(seg, [](wchar_t ch) { return ch < 0x80; }))
        return seg;
    BOOL usedDefault = FALSE;
    const int byteCount = WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, seg.data(), int(seg.size()), nullptr, 0, nullptr, &usedDefault);
    if (byteCount <= 0 || usedDefault)
        return seg;
    std::string bytes(size_t(byteCount), '\0');
    if (WideCharToMultiByte(codepage, WC_NO_BEST_FIT_CHARS, seg.data(), int(seg.size()), bytes.data(), byteCount, nullptr, &usedDefault) != byteCount ||
        usedDefault)
        return seg;
    const int cpChars = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), byteCount, nullptr, 0);
    if (cpChars != int(seg.size()))
        return seg;
    std::wstring roundTrip(size_t(cpChars), L'\0');
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, bytes.data(), byteCount, roundTrip.data(), cpChars) != cpChars || roundTrip != seg)
        return seg;
    if (std::ranges::none_of(bytes, [](unsigned char b) { return b >= 0x80; }))
        return seg;

    std::wstring decoded;
    if (TryDecodeUtf8Validated(bytes.data(), byteCount, seg, decoded))
        return decoded;
    if (allowTruncatedFat11 && byteCount == 11) {
        for (int trim = 1; trim <= 2 && byteCount - trim >= 2; ++trim) {
            bool hasHighPrefix = false;
            for (int i = 0; i < byteCount - trim; ++i)
                hasHighPrefix |= static_cast<unsigned char>(bytes[size_t(i)]) >= 0x80;
            if (hasHighPrefix && TryDecodeUtf8Validated(bytes.data(), byteCount - trim, seg, decoded))
                return decoded;
        }
    }
    return seg;
}

inline std::wstring RepairMojibakeText(const std::wstring& text, UINT primaryCp, UINT secondaryCp = 0, bool allowTruncatedFat11 = false)
{
    if (std::wstring repaired = RepairUtf8Mojibake(text, primaryCp, allowTruncatedFat11); repaired != text)
        return repaired;
    if (secondaryCp != 0 && secondaryCp != primaryCp)
        if (std::wstring repaired = RepairUtf8Mojibake(text, secondaryCp, allowTruncatedFat11); repaired != text)
            return repaired;
    return text;
}

inline std::wstring SanitizeUsbVolumeLabel(const std::wstring& raw, UINT oemCp = GetOEMCP(), UINT acp = GetACP())
{
    const std::wstring clean = TrimAsciiWhitespace(StripInvisibleUiChars(raw));
    if (clean.empty())
        return {};
    std::wstring repaired = TrimAsciiWhitespace(RepairMojibakeText(clean, oemCp, acp, true));
    return repaired.find(L'\uFFFD') != std::wstring::npos ? std::wstring{} : repaired;
}

inline std::wstring SanitizeAutoPlayText(const std::wstring& raw, UINT oemCp = GetOEMCP(), UINT acp = GetACP())
{
    std::wstring clean = StripInvisibleUiChars(raw);
    if (clean.empty())
        return clean;
    if (clean.size() >= 5 && clean[clean.size() - 4] == L'(' && iswalpha(clean[clean.size() - 3]) && clean[clean.size() - 2] == L':' && clean.back() == L')') {
        const size_t suffixAt = clean.size() - 4;
        const std::wstring prefix = TrimAsciiWhitespace(clean.substr(0, (suffixAt > 0 && clean[suffixAt - 1] == L' ') ? suffixAt - 1 : suffixAt));
        const std::wstring fixed = prefix.empty() ? prefix : RepairMojibakeText(prefix, oemCp, acp, true);
        return fixed.empty() ? clean.substr(suffixAt) : fixed + L" " + clean.substr(suffixAt);
    }
    std::wstring out;
    for (size_t start = 0; start <= clean.size();) {
        const size_t nl = clean.find(L'\n', start);
        const std::wstring line = nl == std::wstring::npos ? clean.substr(start) : clean.substr(start, nl - start);
        if (!out.empty())
            out.push_back(L'\n');
        out += RepairMojibakeText(line, oemCp, acp, false);
        if (nl == std::wstring::npos)
            break;
        start = nl + 1;
    }
    return out;
}

} // namespace atrium
