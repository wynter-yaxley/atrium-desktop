// Pure parser and calendar unit tests (no network, disk profile, or UI).
#include "shell/jump_list.h"
#include "shell/lunar.h"
#include "shell/start_layout.h"
#include "shell/task_rules.h"
#include "shell/weather.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
namespace {
void Check(int& f, bool ok, const char* what)
{
    if (!ok && ++f)
        std::fprintf(stderr, "FAIL: %s\n", what);
}
void TestWeather(int& f)
{
    using namespace atrium;
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    auto& v = IsValidWeatherCoords;
    Check(f, v(32, 118) && !v(nan, 0) && !v(0, inf) && !v(90.1, 0) && !v(0, -180.1), "coords");
    Weather src{.ok = true, .city = L"NJ", .temp = 21.5f, .hi = 25, .lo = 15, .code = 2, .day = true, .lat = 32, .lon = 118, .placed = true}, dst, bad;
    src.hours.push_back({L"Now", 2, true, 21.5f, 0}), src.days.push_back({L"Today", 2, 15, 25});
    Check(f, WeatherFromJsonString(WeatherToJsonString(src), dst) && dst.ok && dst.placed && dst.city == L"NJ", "round-trip");
    Check(f, !WeatherFromJsonString(L"{", bad) && WeatherFromJsonString(L"{\"lat\":91,\"lon\":0,\"hours\":[{}],\"days\":[{}]}", bad), "bad json");
    double lat = 0, lon = 0;
    std::wstring c;
    auto& pl = ParseWeatherLocationJson;
    Check(f, pl(R"({"status":"success","city":"NJ","lat":32,"lon":118})", 0, lat, lon, c) && c == L"NJ" && !bad.placed, "ip-api");
    Check(f, pl(R"({"success":true,"city":"TY","latitude":35,"longitude":139})", 1, lat, lon, c) && c == L"TY", "ipwho");
    Check(f, !pl(R"({"city":"X","latitude":95,"longitude":0})", 2, lat, lon, c) && !pl("{", 0, lat, lon, c), "bad loc");
    const std::string ok =
        R"({"current":{"time":"2026-11-20T10:00"},"hourly":{"time":["2026-11-20T10:00"],"temperature_2m":[18],"weather_code":[1],"is_day":[1]},)"
        R"("daily":{"time":["2026-11-20"],"weather_code":[1],"temperature_2m_max":[22],"temperature_2m_min":[10]}})";
    const std::string badArr = R"({"current":{"time":"2026-11-20T10:00"},"hourly":{"time":["2026-11-20T10:00"],"temperature_2m":null,"weather_code":[1],)"
                               R"("is_day":[1]},"daily":{"time":["2026-11-20"],"weather_code":[1],"temperature_2m_max":[20],"temperature_2m_min":[10]}})";
    Check(f, ParseWeatherForecastJson(ok, 32, 118, L"NJ", true, dst) && dst.ok && dst.hours.size() == 1, "valid forecast");
    const std::string sunEvents =
        R"({"current":{"time":"2026-11-20T06:00"},"hourly":{"time":["2026-11-20T06:00","2026-11-20T07:00","2026-11-20T08:00"],)"
        R"("temperature_2m":[10,11,12],"weather_code":[1,1,1],"is_day":[0,1,0]},)"
        R"("daily":{"time":["2026-11-20"],"weather_code":[1],"temperature_2m_max":[22],"temperature_2m_min":[10],)"
        R"("sunrise":["2026-11-20T06:30"],"sunset":["2026-11-20T07:30"]}})";
    Check(f,
          ParseWeatherForecastJson(sunEvents, 32, 118, L"NJ", true, dst) && dst.hours.size() == 5 && dst.hours[1].event == 1 &&
              dst.hours[1].label == L"6:30" && dst.hours[3].event == 2 && dst.hours[3].label == L"7:30",
          "sunrise and sunset keep distinct event types in chronological order");
    Check(f, !ParseWeatherForecastJson(badArr, 32, 118, L"NJ", true, bad) && !ParseWeatherForecastJson("{", 32, 118, L"NJ", true, bad), "bad forecast");
}
void TestLunar(int& f)
{
    Check(f, atrium::DaysIn(2000, 2) == 29 && atrium::DaysIn(2024, 2) == 29 && atrium::DaysIn(1900, 2) == 28 && atrium::DaysIn(2100, 2) == 28, "DaysIn");
    Check(f, atrium::Weekday(2000, 1, 1) == 5 && atrium::Weekday(2024, 2, 29) == 3, "Weekday");
    atrium::ChineseCalendar cal;
    if (cal.Available()) {
        int lm = 0, ld = 0, cy = 0;
        bool leap = false, fest = false, nonFest = false;
        Check(f, cal.Lunar(2024, 2, 10, lm, ld, leap, cy) && lm == 1 && ld == 1 && !leap, "Lunar CNY");
        Check(f, !cal.Line(2024, 2, 10).empty() && !cal.MonthDay(2024, 2, 10).empty(), "Line/MonthDay");
        cal.DayLabel(2024, 2, 11, &nonFest), Check(f, !nonFest && !cal.DayLabel(2024, 2, 10, &fest).empty() && fest, "DayLabel");
    }
}
void TestJumpListAndStart(int& f)
{
    std::vector<BYTE> dest(174, 0), lnk(86, 0);
    const short chars = 4, huge = 5000;
    std::memcpy(dest.data() + 160, &chars, 2), std::memcpy(dest.data() + 162, L"abcd", 8);
    Check(f, atrium::ParseDestList(dest).size() == 1 && atrium::ParseDestList(dest)[0].path == L"abcd", "valid DestList");
    std::memcpy(dest.data() + 160, &huge, 2), Check(f, atrium::ParseDestList({0, 1}).empty() && atrium::ParseDestList(dest).empty(), "bad DestList");
    const uint32_t hdr = 0x4C, flags = 0x84;
    const uint16_t cnt = 4, badCnt = 999;
    std::memcpy(lnk.data(), &hdr, 4), std::memcpy(lnk.data() + 20, &flags, 4);
    std::memcpy(lnk.data() + 0x4C, &cnt, 2), std::memcpy(lnk.data() + 0x4E, L"Test", 8);
    atrium::LinkStrings ls;
    auto& p = atrium::ParseLinkStrings;
    Check(f, p(lnk.data(), lnk.size(), ls) && ls.name == L"Test", "valid LinkStrings");
    std::memcpy(lnk.data() + 0x4C, &badCnt, 2), Check(f, !p(lnk.data(), 10, ls) && !p(lnk.data(), lnk.size(), ls), "bad LinkStrings");
    auto& r = atrium::IsRemotePath;
    Check(f, r(L"\\\\?\\UNC\\s\\a\\f") && r(L"\\\\s\\a") && !r(L"C:\\W") && !r(L"\\\\?\\C:\\t"), "IsRemotePath");
    atrium::StartLayout l{.size = 99, .icon = -2};
    l.tiles = {{L"", L"e", {}}, {L"a", L"A", {{L"s", L"S", {}}}}, {L"a", L"D", {}}, {L"folder:1", L"E", {}}};
    l.tiles.push_back({L"folder:2", L"W", {{L"folder:3", L"N", {}}, {L"a", L"D", {}}, {L"b", L"B", {}}}});
    l.hidden = {L"", L"c", L"c"}, l.Clamp();
    auto& t = l.tiles;
    Check(f, l.size == 3 && l.icon == 0 && t.size() == 2 && t[0].apps.empty() && t[1].apps.size() == 1 && l.hidden.size() == 1, "Clamp");
    Check(f, l.NewFolderId() == L"folder:3" && atrium::NameInitials(L"OneDrive") == L"od" && atrium::IndexLetter(L" 9") == L'#', "StartLayout");
    const auto g = atrium::ParseStartLayoutXml(
        L"\xFEFF<?xml version=\"1.0\"?><LayoutModificationTemplate xmlns:start=\"http://schemas.microsoft.com/Start/2014/StartLayout\">"
        L"<StartLayout><start:Group Name=\"\"><start:DesktopApplicationTile Row=\"2\" Column=\"0\" DesktopApplicationLinkPath=\"%APPDATA%\\A &amp; B.lnk\"/>"
        L"<start:Folder Name=\"Tools &quot;X&quot;\" Row=\"0\" Column=\"2\"><start:Tile Row=\"0\" Column=\"1\" AppUserModelID=\"Pkg2!App\"/>"
        L"<start:DesktopApplicationTile Row=\"0\" Column=\"0\" DesktopApplicationID=\"::{645FF040-5081-101B-9F08-00AA002F954E}\"/></start:Folder>"
        L"<start:Tile Row=\"0\" Column=\"0\" AppUserModelID=\"Pkg1!App\"/></start:Group><start:Group Name=\"生产力&#x20;&amp; 办公\">"
        L"<start:SecondaryTile AppUserModelID=\"Sec!Tile\"/><start:DesktopApplicationTile Row=\"0\" Column=\"0\" "
        L"DesktopApplicationID=\"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\notepad.exe\"/><start:Tile Row=\"0\" Column=\"2\" AppUserModelID=\"Pkg3!App\"/>"
        L"</start:Group></StartLayout></LayoutModificationTemplate>");
    Check(f,
          g.size() == 4 && !g[0].folder && g[0].items[0].value == L"Pkg1!App" && !g[2].folder && g[2].items[0].value == L"%APPDATA%\\A & B.lnk" &&
              g[1].folder && g[1].name == L"Tools \"X\"" && g[1].items[0].kind == atrium::StartPinItem::Kind::DesktopAppId &&
              g[1].items[1].value == L"Pkg2!App" && !g[3].folder && g[3].name == L"生产力 & 办公" && g[3].items.size() == 2 &&
              g[3].items[1].value == L"Pkg3!App" && atrium::ParseStartLayoutXml(L"<start:Group Name=\"Bad\"><start:Tile AppUserModelID=\"x").empty(),
          "xml");
    std::vector<atrium::StartTile> ft = {
        {L"a", L"A", {}}, {L"folder:1", L"F1", {{L"b", L"B", {}}, {L"c", L"C", {}}}}, {L"folder:2", L"F2", {{L"d", L"D", {}}}}};
    Check(f,
          !atrium::RemoveStartFolder(ft, L"folder:9") && atrium::RemoveStartFolder(ft, L"folder:1") && ft.size() == 2 &&
              !atrium::UngroupStartFolder(ft, L"folder:9") && atrium::UngroupStartFolder(ft, L"folder:2") && ft.size() == 2 && ft[1].id == L"d",
          "folder ops");
    std::vector<atrium::StartTile> at = {{L"a", L"A", {}}, {L"folder:1", L"F1", {{L"b", L"B", {}}}}};
    Check(f, !atrium::ApplyTileAlias(at, L"missing", L"X") && atrium::ApplyTileAlias(at, L"a", L"A2") && at[0].alias == L"A2", "ApplyTileAlias top");
    Check(f, atrium::ApplyTileAlias(at, L"b", L"B2") && at[1].apps[0].alias == L"B2" && atrium::ApplyTileAlias(at, L"a", L"") && at[0].alias.empty(),
          "ApplyTileAlias folder/clear");
    atrium::StartLayout src{.size = 1, .icon = 2, .custom = true, .tiles = at}, dst, legacy;
    const std::wstring j = atrium::SerializeStartLayoutJson(src);
    Check(f,
          j.find(L"\"alias\":\"B2\"") != std::wstring::npos && atrium::ParseStartLayoutJson(j, dst) && dst.tiles.size() == 2 && dst.tiles[0].alias.empty() &&
              dst.tiles[1].apps[0].alias == L"B2",
          "StartLayout alias round-trip");
    Check(f,
          atrium::ParseStartLayoutJson(L"{\"version\":2,\"size\":\"medium\",\"tiles\":[{\"id\":\"a\",\"name\":\"A\"}]}", legacy) && legacy.tiles.size() == 1 &&
              legacy.tiles[0].alias.empty(),
          "StartLayout legacy");
}
void TestTitleBadges(int& f)
{
    using atrium::ParseTitleBadge;
    Check(f, ParseTitleBadge(L"(3) Inbox", true, L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"") == 3, "web app title badge");
    Check(f, ParseTitleBadge(L"(99+) Discord", false, L"C:\\Users\\x\\AppData\\Local\\Discord\\Discord.exe", L"") == 100, "99+ discord");
    Check(f, ParseTitleBadge(L"(12) Slack", false, L"slack.exe", L"") == 12, "slack badge");
    Check(f, ParseTitleBadge(L"(5) Mail", false, L"olk.exe", L"") == 5, "new outlook badge");
    Check(f, ParseTitleBadge(L"(2) Chat", false, L"", L"MSTeams_8wekyb3d8bbwe!MSTeams") == 2, "ms teams aumid badge");
    Check(f, ParseTitleBadge(L"(2026) Report", true, L"chrome.exe", L"") == 0, "reject 4-digit year");
    Check(f, ParseTitleBadge(L"file (2).pdf", true, L"chrome.exe", L"") == 0, "reject non-leading count");
    Check(f, ParseTitleBadge(L"(0) x", true, L"chrome.exe", L"") == 0, "reject zero count");
    Check(f, ParseTitleBadge(L"(3) Inbox", false, L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"") == 0, "reject plain browser window");
}
} // namespace
int main()
{
    int f = 0;
    TestWeather(f), TestLunar(f), TestJumpListAndStart(f), TestTitleBadges(f);
    return f ? 1 : 0;
}
