#include "shell/weather.h"

#include "localization.h"

#include "log.h"
#include "settings.h"

#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr auto kEvery = std::chrono::minutes(30);
constexpr auto kRetry = std::chrono::minutes(3);

std::wstring Wide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

bool TryParseJson(const std::string& text, json::JsonObject& out) { return !text.empty() && json::JsonObject::TryParse(Wide(text), out) && out; }

std::wstring JsonString(const json::JsonObject& o, const wchar_t* key, const wchar_t* fallback = L"")
{
    if (!o.HasKey(key))
        return fallback;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : std::wstring(fallback);
}

double JsonNumber(const json::JsonObject& o, const wchar_t* key, double fallback = 0)
{
    if (!o.HasKey(key))
        return fallback;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == json::JsonValueType::Number ? v.GetNumber() : fallback;
}

bool JsonBool(const json::JsonObject& o, const wchar_t* key, bool fallback = false)
{
    if (!o.HasKey(key))
        return fallback;
    const auto v = o.GetNamedValue(key);
    return v.ValueType() == json::JsonValueType::Boolean ? v.GetBoolean() : fallback;
}

bool NamedArr(const json::JsonObject& o, const wchar_t* k, json::JsonArray& out)
{
    return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Array ? (out = o.GetNamedArray(k), true) : false;
}

struct AsyncHttpContext {
    HANDLE stepEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE closedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> failed{false};
    std::atomic<DWORD> bytesAvailable{0};
    std::atomic<DWORD> bytesRead{0};
    std::string readBuf;

    AsyncHttpContext() = default;
    AsyncHttpContext(const AsyncHttpContext&) = delete;
    AsyncHttpContext& operator=(const AsyncHttpContext&) = delete;
    ~AsyncHttpContext()
    {
        if (stepEvent)
            CloseHandle(stepEvent);
        if (closedEvent)
            CloseHandle(closedEvent);
    }
};

std::mutex g_httpContextLock;
std::unordered_map<void*, std::shared_ptr<AsyncHttpContext>> g_httpContexts;

std::shared_ptr<AsyncHttpContext> LookupHttpContext(DWORD_PTR contextKey)
{
    std::lock_guard g(g_httpContextLock);
    const auto it = g_httpContexts.find(reinterpret_cast<void*>(contextKey));
    return it != g_httpContexts.end() ? it->second : nullptr;
}

void CALLBACK WeatherHttpCallback(HINTERNET, DWORD_PTR dwContext, DWORD status, LPVOID statusInfo, DWORD statusLen)
{
    const std::shared_ptr<AsyncHttpContext> ctx = LookupHttpContext(dwContext);
    if (!ctx)
        return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        SetEvent(ctx->closedEvent);
        std::lock_guard g(g_httpContextLock);
        g_httpContexts.erase(reinterpret_cast<void*>(dwContext));
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        ctx->failed.store(true, std::memory_order_release);
        SetEvent(ctx->stepEvent);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && statusInfo && statusLen >= sizeof(DWORD))
        ctx->bytesAvailable.store(*static_cast<const DWORD*>(statusInfo), std::memory_order_release);
    else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        ctx->bytesRead.store(statusLen, std::memory_order_release);
    if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
        status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE || status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        SetEvent(ctx->stepEvent);
}

bool WaitAsyncStep(const AsyncHttpContext& ctx, HANDLE quitEvent)
{
    if (!ctx.stepEvent || !quitEvent)
        return false;
    HANDLE handles[2] = {ctx.stepEvent, quitEvent};
    return WaitForMultipleObjects(2, handles, FALSE, 8000) == WAIT_OBJECT_0 && !ctx.failed.load(std::memory_order_acquire);
}

std::string ReadAsyncResponse(HINTERNET request, AsyncHttpContext& ctx, HANDLE quitEvent)
{
    const auto rawCtx = reinterpret_cast<DWORD_PTR>(&ctx);
    ResetEvent(ctx.stepEvent);
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, rawCtx) || !WaitAsyncStep(ctx, quitEvent))
        return {};
    ResetEvent(ctx.stepEvent);
    if (!WinHttpReceiveResponse(request, nullptr) || !WaitAsyncStep(ctx, quitEvent))
        return {};
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return {};
    std::string body;
    for (;;) {
        ctx.bytesAvailable.store(0, std::memory_order_release);
        ResetEvent(ctx.stepEvent);
        if (!WinHttpQueryDataAvailable(request, nullptr) || !WaitAsyncStep(ctx, quitEvent))
            return {};
        const DWORD n = ctx.bytesAvailable.load(std::memory_order_acquire);
        if (n == 0)
            return body;
        ctx.readBuf.resize(n);
        ctx.bytesRead.store(0, std::memory_order_release);
        ResetEvent(ctx.stepEvent);
        if (!WinHttpReadData(request, ctx.readBuf.data(), n, nullptr) || !WaitAsyncStep(ctx, quitEvent))
            return {};
        const DWORD got = ctx.bytesRead.load(std::memory_order_acquire);
        if (got == 0)
            return body;
        body.append(ctx.readBuf.data(), got);
        if (body.size() > 4 * 1024 * 1024)
            return body;
    }
}

} // namespace

// A place on the globe: what a service or the cache says is only used if it is one (the URLs have room for no more).
bool IsValidWeatherCoords(double lat, double lon) { return std::isfinite(lat) && std::isfinite(lon) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180; }

struct WeatherSource::State : std::enable_shared_from_this<WeatherSource::State> {
    State() : quitEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State()
    {
        if (quitEvent_)
            CloseHandle(quitEvent_);
    }

    std::string Get(const std::wstring& url, bool direct);
    std::string GetAny(const std::wstring& url);
    std::wstring PlaceName(double lat, double lon, bool english);
    bool Locate(double& lat, double& lon, std::wstring& city, bool english);
    bool LocateByIp(double& lat, double& lon, std::wstring& city, bool english, bool located);
    bool Fetch(bool english);
    void Run();

private:
    friend class WeatherSource;
    std::mutex lock_;
    std::condition_variable wake_;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    bool active_ = false, dirty_ = true, lastEnglish_ = false;
    std::atomic_bool quit_{false};
    HANDLE quitEvent_ = nullptr;
    std::wstring cachePath_, lastName_;
    Weather latest_;
    double lastLat_ = 1000, lastLon_ = 1000;
    Fetcher fetcher_;
};

// A GET over async WinHTTP (single-owner handle close on worker thread); direct: not through the system's proxy.
std::string WeatherSource::State::Get(const std::wstring& url, bool direct)
{
    if (quit_.load() || !quitEvent_)
        return {};
    URL_COMPONENTS u{sizeof(u)};
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    u.lpszHostName = host;
    u.dwHostNameLength = 256;
    u.lpszUrlPath = path;
    u.dwUrlPathLength = 2048;
    u.lpszExtraInfo = extra;
    u.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u))
        return {};
    const HINTERNET session = WinHttpOpen(L"Atrium", direct ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!session)
        return {};
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    const HINTERNET connect = WinHttpConnect(session, host, u.nPort, 0);
    const std::wstring target = std::wstring(path) + extra;
    const HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                           u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) :
                                        nullptr;
    auto ctx = std::make_shared<AsyncHttpContext>();
    struct HandleGuard {
        HINTERNET session = nullptr, connect = nullptr, request = nullptr;
        std::shared_ptr<AsyncHttpContext> ctx;
        bool registered = false;
        HandleGuard(HINTERNET s, HINTERNET c, HINTERNET r, std::shared_ptr<AsyncHttpContext> cx) : session(s), connect(c), request(r), ctx(std::move(cx)) {}
        ~HandleGuard()
        {
            if (request)
                WinHttpCloseHandle(request);
            if (registered && ctx && ctx->closedEvent)
                WaitForSingleObject(ctx->closedEvent, 250);
            if (connect)
                WinHttpCloseHandle(connect);
            if (session)
                WinHttpCloseHandle(session);
        }
        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;
    } guard{session, connect, request, ctx};

    if (!request || !ctx->stepEvent || !ctx->closedEvent)
        return {};
    {
        std::lock_guard g(g_httpContextLock);
        g_httpContexts.emplace(ctx.get(), ctx);
    }
    auto rawCtx = reinterpret_cast<DWORD_PTR>(ctx.get());
    if (!WinHttpSetOption(request, WINHTTP_OPTION_CONTEXT_VALUE, &rawCtx, sizeof(rawCtx)) ||
        WinHttpSetStatusCallback(request, WeatherHttpCallback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK) {
        std::lock_guard g(g_httpContextLock);
        g_httpContexts.erase(ctx.get());
        return {};
    }
    guard.registered = true;
    const std::string body = ReadAsyncResponse(request, *ctx, quitEvent_);
    return quit_.load() ? std::string{} : body;
}

// Directly first; then through the system's proxy (a network that only reaches out through it).
std::string WeatherSource::State::GetAny(const std::wstring& url)
{
    std::string body = Get(url, true);
    return !body.empty() || quit_.load() ? body : Get(url, false);
}

// A place's name for coordinates (BigDataCloud's free reverse geocoding, no account): the city,
// else the locality; "南京市" is shown as "南京", as the network's place is. Asked again only once the
// computer has moved a few kilometres (the worker thread alone calls it).
std::wstring WeatherSource::State::PlaceName(double lat, double lon, bool english)
{
    if (!lastName_.empty() && lastEnglish_ == english && std::fabs(lat - lastLat_) < 0.03 && std::fabs(lon - lastLon_) < 0.03)
        return lastName_;
    wchar_t url[256];
    swprintf_s(url, L"https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=%.4f&longitude=%.4f&localityLanguage=%s", lat, lon,
               english ? L"en" : L"zh");
    std::string body = GetAny(url);
    if (quit_.load())
        return {};
    json::JsonObject o{nullptr};
    if (!TryParseJson(body, o))
        return {};
    std::wstring name = JsonString(o, L"city");
    if (name.empty())
        name = JsonString(o, L"locality");
    if (!english && name.size() > 2 && name.back() == L'市')
        name.pop_back();
    if (!name.empty()) {
        lastLat_ = lat;
        lastLon_ = lon;
        lastName_ = name;
        lastEnglish_ = english;
    }
    return name;
}

// Where the computer is: Windows' location service (when it lets desktop apps use it), else the
// network's place. The network's city also names a Windows position that has no name.
bool WeatherSource::State::Locate(double& lat, double& lon, std::wstring& city, bool english)
{
    namespace geo = winrt::Windows::Devices::Geolocation;
    bool located = false;
    try {
        // Bounded: on some systems (#67) the access request never completes, which left the
        // widget waiting for good.
        auto access = geo::Geolocator::RequestAccessAsync();
        if (!WaitWeatherAsync(access, shared_from_this(), lock_, wake_, quit_, std::chrono::seconds(5)))
            return false;
        if (access.GetResults() == geo::GeolocationAccessStatus::Allowed) {
            geo::Geolocator locator;
            auto position = locator.GetGeopositionAsync(std::chrono::minutes(30), std::chrono::seconds(15));
            if (!WaitWeatherAsync(position, shared_from_this(), lock_, wake_, quit_, std::chrono::seconds(20)))
                return false;
            auto at = position.GetResults().Coordinate().Point().Position();
            lat = at.Latitude;
            lon = at.Longitude;
            located = true;
        } else if (!quit_.load()) {
            Log(L"weather: Windows' location is off for desktop apps; using the network's place");
        }
    } catch (const winrt::hresult_error& e) {
        if (!quit_.load())
            Log(L"weather: Windows' location failed (0x%08X); using the network's place", static_cast<unsigned>(e.code()));
    }
    if (quit_.load())
        return false;
    if (located) {
        city = PlaceName(lat, lon, english);
        if (quit_.load())
            return false;
        Log(L"weather: located by Windows (%s)", city.c_str());
    }
    if (located && !city.empty())
        return true;
    return LocateByIp(lat, lon, city, english, located);
}

bool ParseWeatherLocationJson(const std::string& body, int serviceIndex, double& lat, double& lon, std::wstring& city)
{
    static constexpr const wchar_t* kF[3][5] = {{L"status", L"success", L"city", L"lat", L"lon"},
                                                {L"success", nullptr, L"city", L"latitude", L"longitude"},
                                                {nullptr, nullptr, L"city", L"latitude", L"longitude"}};
    json::JsonObject o{nullptr};
    if (serviceIndex < 0 || serviceIndex >= 3 || !TryParseJson(body, o))
        return false;
    const auto* f = kF[serviceIndex];
    if (f[0] && !(f[1] ? JsonString(o, f[0]) == f[1] : JsonBool(o, f[0])))
        return false;
    if (!o.HasKey(f[3]) || !o.HasKey(f[4]) || o.GetNamedValue(f[3]).ValueType() != json::JsonValueType::Number ||
        o.GetNamedValue(f[4]).ValueType() != json::JsonValueType::Number)
        return false;
    const double pLat = o.GetNamedNumber(f[3], 0), pLon = o.GetNamedNumber(f[4], 0);
    return IsValidWeatherCoords(pLat, pLon) ? (city = JsonString(o, f[2]), lat = pLat, lon = pLon, true) : false;
}

bool WeatherSource::State::LocateByIp(double& lat, double& lon, std::wstring& city, bool english, bool located)
{
    // The network's place: several services, each directly (the system's proxy would give the
    // proxy's place), then through the proxy (a network that only reaches out through it): one
    // service blocked or over its limit (#67) is not the end of it.
    struct Service {
        const wchar_t* url;
        const wchar_t *ok, *okValue, *city, *lat, *lon;
    };
    const Service services[] = {
        {english ? L"http://ip-api.com/json/?lang=en&fields=status,city,lat,lon" : L"http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon", L"status",
         L"success", L"city", L"lat", L"lon"},
        {english ? L"https://ipwho.is/?lang=en" : L"https://ipwho.is/?lang=zh-CN", L"success", nullptr, L"city", L"latitude", L"longitude"},
        {L"https://ipapi.co/json/", nullptr, nullptr, L"city", L"latitude", L"longitude"},
    };
    for (bool direct : {true, false})
        for (const Service& s : services) {
            if (quit_.load())
                return false;
            std::string body = Get(s.url, direct);
            if (quit_.load())
                return false;
            double pLat = 0, pLon = 0;
            if (!ParseWeatherLocationJson(body, int(&s - services), pLat, pLon, city))
                continue;
            if (!located)
                lat = pLat, lon = pLon;
            if (quit_.load())
                return false;
            if (s.url != services[0].url || !direct)
                Log(L"weather: placed by %s%s", s.url, direct ? L"" : L" (through the proxy)");
            return true;
        }
    return located;
}

namespace {

// "2026-11-09T14:00" -> minutes since 2000 (only compared with each other), hour and minute.
int64_t Minutes(const std::wstring& t, int* hour = nullptr, int* minute = nullptr, int* weekday = nullptr)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (swscanf_s(t.c_str(), L"%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) < 3)
        return 0;
    if (hour)
        *hour = h;
    if (minute)
        *minute = mi;
    SYSTEMTIME st{WORD(y), WORD(mo), 0, WORD(d), WORD(h), WORD(mi), 0, 0};
    FILETIME f{};
    SystemTimeToFileTime(&st, &f);
    int64_t m = int64_t((ULARGE_INTEGER{f.dwLowDateTime, f.dwHighDateTime}.QuadPart) / 600000000ULL);
    if (weekday) {
        FileTimeToSystemTime(&f, &st);
        *weekday = st.wDayOfWeek; // 0 Sunday
    }
    return m;
}

const wchar_t* const kWeek[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
const wchar_t* const kWeekEn[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
const wchar_t* WeekName(int wd, bool english) { return english ? Tr(kWeekEn[wd]) : kWeek[wd]; }

json::JsonObject ToJson(const Weather& w)
{
    json::JsonObject o;
    o.SetNamedValue(L"city", json::JsonValue::CreateStringValue(w.city));
    o.SetNamedValue(L"temp", json::JsonValue::CreateNumberValue(w.temp));
    o.SetNamedValue(L"hi", json::JsonValue::CreateNumberValue(w.hi));
    o.SetNamedValue(L"lo", json::JsonValue::CreateNumberValue(w.lo));
    o.SetNamedValue(L"code", json::JsonValue::CreateNumberValue(w.code));
    o.SetNamedValue(L"day", json::JsonValue::CreateBooleanValue(w.day));
    if (w.placed) {
        o.SetNamedValue(L"lat", json::JsonValue::CreateNumberValue(w.lat));
        o.SetNamedValue(L"lon", json::JsonValue::CreateNumberValue(w.lon));
    }
    json::JsonArray hours, days;
    for (const WeatherHour& h : w.hours) {
        json::JsonObject x;
        x.SetNamedValue(L"label", json::JsonValue::CreateStringValue(h.label));
        x.SetNamedValue(L"code", json::JsonValue::CreateNumberValue(h.code));
        x.SetNamedValue(L"day", json::JsonValue::CreateBooleanValue(h.day));
        x.SetNamedValue(L"temp", json::JsonValue::CreateNumberValue(h.temp));
        x.SetNamedValue(L"event", json::JsonValue::CreateNumberValue(h.event));
        hours.Append(x);
    }
    for (const WeatherDay& d : w.days) {
        json::JsonObject x;
        x.SetNamedValue(L"label", json::JsonValue::CreateStringValue(d.label));
        x.SetNamedValue(L"code", json::JsonValue::CreateNumberValue(d.code));
        x.SetNamedValue(L"lo", json::JsonValue::CreateNumberValue(d.lo));
        x.SetNamedValue(L"hi", json::JsonValue::CreateNumberValue(d.hi));
        days.Append(x);
    }
    o.SetNamedValue(L"hours", hours);
    o.SetNamedValue(L"days", days);
    return o;
}

bool FromJson(const json::JsonObject& o, Weather& w)
{
    w = Weather{};
    if (!o)
        return false;
    w.city = JsonString(o, L"city");
    w.temp = float(JsonNumber(o, L"temp"));
    w.hi = float(JsonNumber(o, L"hi"));
    w.lo = float(JsonNumber(o, L"lo"));
    w.code = int(JsonNumber(o, L"code"));
    w.day = JsonBool(o, L"day", true);
    w.lat = JsonNumber(o, L"lat");
    w.lon = JsonNumber(o, L"lon");
    w.placed = o.HasKey(L"lat") && o.HasKey(L"lon") && o.GetNamedValue(L"lat").ValueType() == json::JsonValueType::Number &&
               o.GetNamedValue(L"lon").ValueType() == json::JsonValueType::Number && IsValidWeatherCoords(w.lat, w.lon);
    json::JsonArray hours{nullptr}, days{nullptr};
    if (NamedArr(o, L"hours", hours)) {
        for (const auto& v : hours) {
            if (v.ValueType() != json::JsonValueType::Object) {
                w = Weather{};
                return false;
            }
            const auto x = v.GetObject();
            w.hours.push_back(
                {JsonString(x, L"label"), int(JsonNumber(x, L"code")), JsonBool(x, L"day", true), float(JsonNumber(x, L"temp")), int(JsonNumber(x, L"event"))});
        }
    }
    if (NamedArr(o, L"days", days)) {
        for (const auto& v : days) {
            if (v.ValueType() != json::JsonValueType::Object) {
                w = Weather{};
                return false;
            }
            const auto x = v.GetObject();
            w.days.push_back({JsonString(x, L"label"), int(JsonNumber(x, L"code")), float(JsonNumber(x, L"lo")), float(JsonNumber(x, L"hi"))});
        }
    }
    w.ok = !w.hours.empty() && !w.days.empty();
    if (!w.ok)
        w = Weather{};
    return w.ok;
}

} // namespace

std::wstring WeatherToJsonString(const Weather& w) { return std::wstring(ToJson(w).Stringify()); }
bool WeatherFromJsonString(const std::wstring& text, Weather& out)
{
    json::JsonObject o{nullptr};
    return !(out = Weather{}, text.empty()) && json::JsonObject::TryParse(text, o) && FromJson(o, out);
}

std::wstring WeatherCondition(int code)
{
    switch (code) {
    case 0: return Tr(L"Clear");
    case 1: return Tr(L"Mostly clear");
    case 2: return Tr(L"Partly cloudy");
    case 3: return Tr(L"Overcast");
    case 45:
    case 48: return Tr(L"Fog");
    case 51:
    case 53:
    case 55: return Tr(L"Drizzle");
    case 56:
    case 57:
    case 66:
    case 67: return Tr(L"Freezing rain");
    case 61: return Tr(L"Light rain");
    case 63: return Tr(L"Moderate rain");
    case 65: return Tr(L"Heavy rain");
    case 71: return Tr(L"Light snow");
    case 73: return Tr(L"Moderate snow");
    case 75: return Tr(L"Heavy snow");
    case 77: return Tr(L"Snow grains");
    case 80:
    case 81: return Tr(L"Rain showers");
    case 82: return Tr(L"Violent rain showers");
    case 85:
    case 86: return Tr(L"Snow showers");
    case 95: return Tr(L"Thunderstorm");
    case 96:
    case 99: return Tr(L"Thunderstorm with hail");
    default: return Tr(L"Cloudy");
    }
}

std::wstring WeatherCachePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\')) + L"\\weather.json";
}

WeatherSource::WeatherSource(HWND notify, UINT message) : WeatherSource(notify, message, nullptr) {}

WeatherSource::WeatherSource(HWND notify, UINT message, Fetcher fetcher) : state_(std::make_shared<State>())
{
    state_->notify_ = notify;
    state_->message_ = message;
    state_->fetcher_ = std::move(fetcher);
    if (!state_->fetcher_) {
        state_->cachePath_ = WeatherCachePath();
        // The last forecast at once (stale is better than nothing while the first fetch runs).
        json::JsonObject o{nullptr};
        std::wstring text = ReadTextFile(state_->cachePath_);
        if (!text.empty() && json::JsonObject::TryParse(text, o))
            FromJson(o, state_->latest_);
    }
    thread_ = std::thread([state = state_] { state->Run(); });
}

WeatherSource::~WeatherSource()
{
    {
        std::lock_guard g(state_->lock_);
        state_->quit_.store(true);
        state_->notify_ = nullptr;
    }
    if (state_->quitEvent_)
        SetEvent(state_->quitEvent_);
    state_->wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void WeatherSource::SetActive(bool on)
{
    {
        std::lock_guard g(state_->lock_);
        if (on == state_->active_)
            return;
        state_->active_ = on;
    }
    state_->wake_.notify_all();
}

void WeatherSource::Refresh()
{
    {
        std::lock_guard g(state_->lock_);
        state_->dirty_ = true;
    }
    state_->wake_.notify_all();
}

Weather WeatherSource::Latest()
{
    std::lock_guard g(state_->lock_);
    return state_->latest_;
}

void WeatherSource::State::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        bool english = false;
        {
            std::unique_lock g(lock_);
            wake_.wait(g, [&] { return quit_ || active_; }); // nothing to do while no weather widget shows
            if (quit_)
                break;
            wake_.wait_until(g, next, [&] { return quit_ || !active_ || dirty_; });
            if (quit_)
                break;
            if (!active_ || (!dirty_ && std::chrono::steady_clock::now() < next))
                continue;
            dirty_ = false;
            english = LanguageIsEnglish();
        }
        bool ok = false;
        if (Weather w; fetcher_) {
            if (ok = fetcher_(quit_, w); ok && !quit_) {
                std::lock_guard g(lock_);
                latest_ = std::move(w);
            }
        } else {
            ok = Fetch(english);
        }
        if (quit_)
            break;
        next = std::chrono::steady_clock::now() + (ok ? kEvery : kRetry);
        HWND notify;
        {
            std::lock_guard g(lock_);
            notify = notify_;
        }
        if (notify)
            PostMessageW(notify, message_, 0, 0);
    }
    winrt::uninit_apartment();
}

namespace {
struct HourItem {
    int64_t at;
    WeatherHour hour;
};
void AddSunEvents(const json::JsonObject& daily, int64_t now, std::vector<HourItem>& items)
{
    if (items.empty())
        return;
    const int64_t last = items.back().at;
    for (const wchar_t* key : {L"sunrise", L"sunset"}) {
        json::JsonArray list{nullptr};
        if (!NamedArr(daily, key, list))
            continue;
        for (uint32_t i = 0; i < list.Size(); ++i) {
            if (list.GetAt(i).ValueType() != json::JsonValueType::String)
                continue;
            int h = 0, m = 0;
            int64_t at = Minutes(std::wstring(list.GetStringAt(i)), &h, &m);
            if (at <= now || at >= last)
                continue;
            WeatherHour x;
            wchar_t label[16];
            swprintf_s(label, L"%d:%02d", h, m);
            x.label = label;
            x.event = key[4] == L'r' ? 1 : 2;
            items.push_back({at, x});
        }
    }
    std::ranges::stable_sort(items, [](const HourItem& a, const HourItem& b) { return a.at < b.at; });
    if (items.size() > 6)
        items.resize(6);
}
} // namespace

bool ParseWeatherForecastJson(const std::string& body, double lat, double lon, const std::wstring& city, bool english, Weather& out)
{
    out = Weather{};
    json::JsonObject o{nullptr}, cur{nullptr}, hr{nullptr}, dy{nullptr};
    json::JsonArray tm{nullptr}, tp{nullptr}, cd{nullptr}, ds{nullptr}, dt{nullptr}, dc{nullptr}, hi{nullptr}, lo{nullptr};
    auto obj = [&](const wchar_t* k, json::JsonObject& x) {
        return o.HasKey(k) && o.GetNamedValue(k).ValueType() == json::JsonValueType::Object ? (x = o.GetNamedObject(k), true) : false;
    };
    if (!TryParseJson(body, o) || !obj(L"current", cur) || !obj(L"hourly", hr) || !obj(L"daily", dy) || !NamedArr(hr, L"time", tm) ||
        !NamedArr(hr, L"temperature_2m", tp) || !NamedArr(hr, L"weather_code", cd) || !NamedArr(hr, L"is_day", ds) || !NamedArr(dy, L"time", dt) ||
        !NamedArr(dy, L"weather_code", dc) || !NamedArr(dy, L"temperature_2m_max", hi) || !NamedArr(dy, L"temperature_2m_min", lo))
        return false;
    Weather w;
    w.city = city, w.lat = lat, w.lon = lon, w.placed = IsValidWeatherCoords(lat, lon), w.temp = float(JsonNumber(cur, L"temperature_2m")),
    w.code = int(JsonNumber(cur, L"weather_code")), w.day = JsonNumber(cur, L"is_day", 1) != 0;
    const int64_t now = Minutes(JsonString(cur, L"time"));
    std::vector<HourItem> items;
    auto isNum = [](const json::JsonArray& a, uint32_t i) { return a.GetAt(i).ValueType() == json::JsonValueType::Number; };
    for (uint32_t i = 0, nh = std::min({tm.Size(), tp.Size(), cd.Size(), ds.Size()}); i < nh && items.size() < 6; ++i) {
        if (tm.GetAt(i).ValueType() != json::JsonValueType::String || !isNum(tp, i) || !isNum(cd, i) || !isNum(ds, i))
            continue;
        int h = 0;
        int64_t at = Minutes(std::wstring(tm.GetStringAt(i)), &h);
        if (at + 60 > now)
            items.push_back({at,
                             {items.empty() ? std::wstring(Tr(L"Now")) : std::to_wstring(h) + (english ? L":00" : L"时"), int(cd.GetNumberAt(i)),
                              ds.GetNumberAt(i) != 0, float(tp.GetNumberAt(i)), 0}});
    }
    AddSunEvents(dy, now, items);
    for (const HourItem& i : items)
        w.hours.push_back(i.hour);
    for (uint32_t i = 0, nd = std::min({dt.Size(), dc.Size(), hi.Size(), lo.Size()}); i < nd; ++i) {
        if (dt.GetAt(i).ValueType() != json::JsonValueType::String || !isNum(dc, i) || !isNum(hi, i) || !isNum(lo, i))
            continue;
        int wd = 0;
        Minutes(std::wstring(dt.GetStringAt(i)) + L"T00:00", nullptr, nullptr, &wd);
        w.days.push_back({i == 0 ? Tr(L"Today") : WeekName(wd, english), int(dc.GetNumberAt(i)), float(lo.GetNumberAt(i)), float(hi.GetNumberAt(i))});
    }
    return (!w.hours.empty() && !w.days.empty()) ? (w.hi = w.days[0].hi, w.lo = w.days[0].lo, w.ok = true, out = std::move(w), true) : false;
}

bool WeatherSource::State::Fetch(bool english)
{
    double lat = 0, lon = 0;
    std::wstring city;
    if (!Locate(lat, lon, city, english)) {
        if (quit_.load())
            return false;
        std::lock_guard g(lock_);
        if (!latest_.placed) {
            latest_.ok = latest_.ok && !latest_.hours.empty();
            latest_.error = Tr(L"Location unavailable");
            Log(L"weather: no location");
            return false;
        }
        // No service could say where: the last place (a computer seldom moves far).
        lat = latest_.lat;
        lon = latest_.lon;
        city = latest_.city;
        Log(L"weather: no location now; the last place (%s)", city.c_str());
    }
    wchar_t url[512];
    swprintf_s(url,
               L"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&timezone=auto&forecast_days=7"
               L"&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
               L"&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset",
               lat, lon);
    std::string forecast = GetAny(url);
    if (quit_.load())
        return false;
    Weather w;
    if (!ParseWeatherForecastJson(forecast, lat, lon, city, english, w)) {
        std::lock_guard g(lock_);
        latest_.error = Tr(L"Weather unavailable");
        Log(L"weather: no forecast for %s", city.c_str());
        return false;
    }
    if (quit_.load())
        return false;
    if (!WriteTextFile(cachePath_, std::wstring(ToJson(w).Stringify())))
        Log(L"weather: could not write cache %s (%lu)", cachePath_.c_str(), GetLastError());
    if (quit_.load())
        return false;
    Log(L"weather: %s %.0f° %s", w.city.c_str(), w.temp, WeatherCondition(w.code).c_str());
    std::lock_guard g(lock_);
    latest_ = std::move(w);
    return true;
}

} // namespace atrium
