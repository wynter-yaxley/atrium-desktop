#include "shell/update_checker.h"

#include "localization.h"
#include "log.h"
#include "settings.h"
#include "notice.h"
#include "win32_path.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include <cwctype>

namespace atrium {
namespace {
namespace json = winrt::Windows::Data::Json;
constexpr int64_t kIntervalSeconds = 6 * 60 * 60;
constexpr wchar_t kLatest[] = L"https://api.github.com/repos/wynter-yaxley/atrium-desktop/releases/latest";
using Version = std::array<unsigned, 4>;

std::optional<Version> ParseVersion(std::wstring_view tag)
{
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V'))
        tag.remove_prefix(1);
    tag = tag.substr(0, tag.find(L'+'));
    Version version{};
    size_t part = 0;
    bool digit = false;
    for (wchar_t c : tag) {
        if (c == L'.' && digit && part < 3) {
            ++part;
            digit = false;
        } else if (c >= L'0' && c <= L'9') {
            version[part] = version[part] * 10 + (c - L'0');
            if (version[part] > 65535)
                return {};
            digit = true;
        } else
            return {};
    }
    return digit && part >= 1 ? std::optional<Version>(version) : std::nullopt;
}

Version CurrentVersion()
{
    const std::wstring exe = ModulePath();
    DWORD unused = 0;
    std::vector<BYTE> data(GetFileVersionInfoSizeW(exe.c_str(), &unused));
    VS_FIXEDFILEINFO* info = nullptr;
    UINT size = 0;
    if (data.empty() || !GetFileVersionInfoW(exe.c_str(), 0, DWORD(data.size()), data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &size))
        throw winrt::hresult_error(E_FAIL, L"Missing application version");
    return {HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

DWORD Delay(int64_t last, int64_t now)
{
    // A clock correction must not postpone checks indefinitely.
    return last > 0 && last <= now && now - last < kIntervalSeconds ? DWORD((kIntervalSeconds - (now - last)) * 1000) : 0;
}

std::wstring ExeDirectory()
{
    const std::wstring path = ModulePath();
    const size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring UpdateCache()
{
    using namespace winrt::Windows::Security::Cryptography;
    const std::wstring directory = ExeDirectory();
    if (directory.empty())
        return {};
    int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, directory.data(), int(directory.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring upper(size_t(count), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, directory.data(), int(directory.size()), upper.data(), count, nullptr, nullptr, 0);
    auto algorithm = Core::HashAlgorithmProvider::OpenAlgorithm(Core::HashAlgorithmNames::Sha256());
    std::wstring key =
        CryptographicBuffer::EncodeToHexString(algorithm.HashData(CryptographicBuffer::ConvertStringToBinary(upper, BinaryStringEncoding::Utf8))).c_str();
    key.resize(16);
    for (wchar_t& ch : key)
        ch = wchar_t(std::towupper(ch));
    std::wstring root = SettingsPath();
    root = root.substr(0, root.find_last_of(L'\\')) + L"\\Updates";
    CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\" + key;
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

void StartWorker(const std::wstring& arguments)
{
    const std::wstring dir = ExeDirectory();
    if (dir.empty())
        return;
    std::wstring exe = dir + L"\\Settings\\Atrium.Settings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return; // native-only development build
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else
        Log(L"update: could not start background queue (%lu)", GetLastError());
}

void DownloadRelease(const json::JsonObject& release, const Version& current, const std::wstring& tag,
                     const std::function<bool(const std::wstring&)>& launchDownload)
{
    auto latest = ParseVersion(tag);
    if (!latest)
        throw winrt::hresult_invalid_argument(L"Invalid release version");
    if (*latest <= current || release.GetNamedBoolean(L"draft", false) || release.GetNamedBoolean(L"prerelease", false))
        return;
    std::wstring number = tag.substr(tag[0] == L'v' || tag[0] == L'V' ? 1 : 0);
    number = number.substr(0, number.find(L'+'));
    std::wstring assetName = L"Atrium-" + number + L"-win-x64.zip";
    for (const auto& value : release.GetNamedArray(L"assets")) {
        auto asset = value.GetObject();
        if (asset.GetNamedString(L"name") != assetName)
            continue;
        json::JsonObject payload;
        std::wstring version;
        for (unsigned part : *latest)
            version += (version.empty() ? L"" : L".") + std::to_wstring(part);
        payload.SetNamedValue(L"Version", json::JsonValue::CreateStringValue(version));
        payload.SetNamedValue(L"Tag", json::JsonValue::CreateStringValue(tag));
        payload.SetNamedValue(L"Page", release.GetNamedValue(L"html_url"));
        payload.SetNamedValue(L"Download", asset.GetNamedValue(L"browser_download_url"));
        payload.SetNamedValue(L"Digest", asset.HasKey(L"digest") ? asset.GetNamedValue(L"digest") : json::JsonValue::CreateNullValue());
        using namespace winrt::Windows::Security::Cryptography;
        const std::wstring args =
            L"--queue-update " +
            std::wstring(
                CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::ConvertStringToBinary(payload.Stringify(), BinaryStringEncoding::Utf8)).c_str());
        if (launchDownload && launchDownload(args))
            Log(L"update: queued background download for %s", tag.c_str());
        else
            Log(L"update: skipped automatic download for %s because autoUpdate was disabled", tag.c_str());
        return;
    }
}

bool CheckLatestRelease(HANDLE stop, const Version& current, bool autoCheck, const std::function<bool(const std::wstring&)>& launchDownload)
{
    try {
        winrt::Windows::Web::Http::HttpClient http;
        http.DefaultRequestHeaders().UserAgent().TryParseAdd(L"Atrium-Desktop/1.0");
        http.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
        auto request = http.GetStringAsync(winrt::Windows::Foundation::Uri(kLatest));
        const auto start = GetTickCount64();
        while (request.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
            if (WaitForSingleObject(stop, 100) == WAIT_OBJECT_0 || GetTickCount64() - start >= 15000) {
                request.Cancel();
                break;
            }
        }
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
            return false;
        auto release = json::JsonObject::Parse(request.GetResults());
        std::wstring tag = release.GetNamedString(L"tag_name").c_str();
        DownloadRelease(release, current, tag, launchDownload);
        Log(L"update: checked latest release %s", tag.c_str());
    } catch (const winrt::hresult_error& error) {
        if (autoCheck)
            Log(L"update: check failed (0x%08X); retry in 6 hours", unsigned(error.code().value));
        else
            Log(L"update: check failed (0x%08X)", unsigned(error.code().value));
    }
    return true;
}
} // namespace

UpdateChecker::UpdateChecker(HWND notify, UINT message, bool autoCheck)
    : notify_(notify),
      message_(message),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      cache_(UpdateCache()),
      autoCheck_(autoCheck)
{
    if (!stop_ || !wake_) {
        const DWORD err = GetLastError();
        if (wake_)
            CloseHandle(wake_);
        if (stop_)
            CloseHandle(stop_);
        winrt::throw_hresult(HRESULT_FROM_WIN32(err ? err : ERROR_OUTOFMEMORY));
    }
    if (autoCheck && !IsDevelopmentBuild() && !cache_.empty() && GetFileAttributesW((cache_ + L"\\download.json").c_str()) != INVALID_FILE_ATTRIBUTES)
        StartWorker(L"--resume-update");
    PostMessageW(notify_, message_, 0, 0);
    worker_ = std::thread([this] { Run(); });
}

UpdateChecker::~UpdateChecker()
{
    if (stop_)
        SetEvent(stop_);
    if (worker_.joinable())
        worker_.join();
    if (wake_)
        CloseHandle(wake_);
    if (stop_)
        CloseHandle(stop_);
}

void UpdateChecker::SetAutoCheck(bool on)
{
    std::lock_guard lock(mutex_);
    if (autoCheck_ == on)
        return;
    autoCheck_ = on;
    SetEvent(wake_);
}

void UpdateChecker::CheckNow()
{
    std::lock_guard lock(mutex_);
    manualRequested_ = true;
    SetEvent(wake_);
}

void UpdateChecker::OnPowerResume()
{
    std::lock_guard lock(mutex_);
    if (autoCheck_)
        SetEvent(wake_);
}

void UpdateChecker::Run()
{
    if (cache_.empty() || IsDevelopmentBuild())
        return;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        const Version current = CurrentVersion();
        const std::wstring path = cache_ + L"\\update-state.json";
        json::JsonObject state;
        if (!json::JsonObject::TryParse(ReadTextFile(path), state))
            state = json::JsonObject();
        int64_t checked = int64_t(state.GetNamedNumber(L"checked", 0));
        const HANDLE handles[2] = {stop_, wake_};
        for (;;) {
            bool runCheck = false;
            bool isAuto = false;
            DWORD waitMs = INFINITE;
            {
                std::lock_guard lock(mutex_);
                if (manualRequested_) {
                    manualRequested_ = false;
                    runCheck = true;
                } else if (autoCheck_) {
                    waitMs = Delay(checked, Now());
                    if (waitMs == 0) {
                        runCheck = true;
                        isAuto = true;
                    }
                }
            }
            if (!runCheck) {
                const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, waitMs);
                if (wait == WAIT_OBJECT_0)
                    break;
                if (wait == WAIT_OBJECT_0 + 1)
                    continue;
                if (wait != WAIT_TIMEOUT)
                    break;
                std::lock_guard lock(mutex_);
                if (!autoCheck_ && !manualRequested_)
                    continue;
                isAuto = !manualRequested_;
                manualRequested_ = false;
            }
            checked = Now();
            auto launchDownload = [this, isAuto](const std::wstring& args) {
                std::lock_guard lock(mutex_);
                if (isAuto && !autoCheck_)
                    return false;
                StartWorker(args);
                return true;
            };
            if (!CheckLatestRelease(stop_, current, isAuto, launchDownload))
                break;
            state.SetNamedValue(L"checked", json::JsonValue::CreateNumberValue(double(checked)));
            if (!WriteTextFile(path, state.Stringify().c_str()))
                Log(L"update: could not save check state");
        }
    } catch (const winrt::hresult_error& error) {
        Log(L"update: checker stopped (0x%08X)", unsigned(error.code().value));
    }
    winrt::uninit_apartment();
}

void UpdateChecker::NotifyPrepared(const std::function<void(std::optional<Notice> card, bool silent)>& show)
{
    if (cache_.empty()) {
        show(std::nullopt, false);
        return;
    }
    try {
        const auto text = ReadTextFile(cache_ + L"\\ready.json");
        if (text.empty()) {
            show(std::nullopt, false);
            return;
        }
        auto ready = json::JsonObject::Parse(text);
        auto version = ParseVersion(ready.GetNamedString(L"Version").c_str());
        const std::wstring tag = ready.GetNamedString(L"Tag").c_str();
        if (!version || *version <= CurrentVersion()) {
            show(std::nullopt, false);
            return;
        }
        const bool silent = ReadTextFile(cache_ + L"\\ready-notified.txt") == tag;
        Notice notice;
        notice.source = L"atrium-update";
        notice.externalId = 1; // one current update card, independent of ordinary notices
        notice.app = L"Atrium";
        notice.title = Tr(L"Update ready");
        notice.body = TrFormat(L"%s has been downloaded. Restart from About to install it.", tag.c_str());
        notice.action = L"settings:updates";
        show(std::move(notice), silent);
        // This runs synchronously on the UI thread: the card is now accepted,
        // rather than merely having a message queued for a possibly dying UI.
        if (!silent && !WriteTextFile(cache_ + L"\\ready-notified.txt", tag))
            Log(L"update: could not save the banner acknowledgement");
    } catch (const winrt::hresult_error& error) {
        Log(L"update: could not read prepared update (0x%08X)", unsigned(error.code().value));
    }
}
} // namespace atrium
