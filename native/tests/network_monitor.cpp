#include "render/wifi_glyph.h"
#include "shell/network_monitor.h"
#include "shell/system_status.h"
#include "shell/wifi.h"
#include "ui/control_center.h"

#include <cstdint>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

} // namespace

int main()
{
    using namespace atrium;

    Check(LinkSpeedMbps(0) == 0.0, "zero link speed stays zero");
    Check(LinkSpeedMbps(1000000000ULL) == 1000.0, "1 Gbps reports 1000 Mbps");
    Check(LinkSpeedMbps(2500000000ULL) == 2500.0, "2.5 Gbps reports 2500 Mbps");
    Check(LinkSpeedMbps(~uint64_t{0}) == 0.0, "NDIS_LINK_SPEED_UNKNOWN reports zero instead of ~1.8e13 Mbps");

    constexpr GUID kGuid1{0x11111111, 0, 0, {1}}, kGuid2{0x22222222, 0, 0, {2}};
    auto m1 = MergeWifiNetworks({{L"HiddenNet", L"HiddenNet", 72, true, true, kGuid1}});
    Check(m1.size() == 1 && m1[0].ssid == L"HiddenNet" && m1[0].connected && IsEqualGUID(m1[0].interfaceGuid, kGuid1), "hidden SSID via profile");
    auto m2 = MergeWifiNetworks({{L"Neighbor", L"", 95, true, false, kGuid1},
                                 {L"Office", L"Office", 55, true, true, kGuid1},
                                 {L"Cafe", L"Cafe", 40, false, false, kGuid2},
                                 {L"Home5G", L"Home5G", 80, true, true, kGuid2}});
    Check(m2.size() == 4 && m2[0].ssid == L"Home5G" && m2[1].ssid == L"Office" && m2[2].ssid == L"Neighbor" && m2[3].ssid == L"Cafe" &&
              IsEqualGUID(m2[0].interfaceGuid, kGuid2) && IsEqualGUID(m2[1].interfaceGuid, kGuid1),
          "two connected interfaces and ordering");
    auto m3 = MergeWifiNetworks({{L"Corp", L"Corp-Old", 90, true, false, kGuid2}, {L"Corp", L"Corp-New", 65, true, true, kGuid1}});
    Check(m3.size() == 1 && m3[0].connected && m3[0].profile == L"Corp-New" && IsEqualGUID(m3[0].interfaceGuid, kGuid1) && m3[0].signal == 90,
          "connected profile/interface wins and keeps max signal");
    Check(ClassifyWifiListStatus(0, false, false) == WifiListStatus::NoAdapter && ClassifyWifiListStatus(2, false, true) == WifiListStatus::AccessDenied &&
              ClassifyWifiListStatus(2, true, true) == WifiListStatus::Ok && ClassifyWifiListStatus(1, false, false) == WifiListStatus::Error &&
              ClassifyWifiListStatus(0, false, false, ERROR_ACCESS_DENIED) == WifiListStatus::AccessDenied,
          "status classification");

    using K = NetworkLink::Kind;
    Check(PickPrimaryLink({}) == -1 && PickPrimaryLink({{1, K::Ethernet, false, 10}, {2, K::Wifi, true, 50}}) == 1 &&
              PickPrimaryLink({{1, K::Wifi, true, 25}, {2, K::Ethernet, true, 35}}) == 0 &&
              PickPrimaryLink({{2, K::Wifi, true, 25}, {1, K::Ethernet, true, 25}}) == 1 && WifiSteps(-1) == 3 && WifiSteps(20) == 1 && WifiSteps(40) == 2 &&
              WifiSteps(80) == 3,
          "primary link selection and WifiSteps");
    SystemStatus legacy,
        both{.connected = true, .links = {{K::Ethernet, 1, L"Ethernet", L"CorpNet", true, true, -1}, {K::Wifi, 2, L"Wi-Fi", L"Home5G", true, false, 88}}},
        noPri{.links = {{K::Wifi, 2, L"Wi-Fi", L"Home5G", false, false, -1}, {K::Ethernet, 1, L"Ethernet", L"LAN", false, false, -1}}},
        single{.connected = true, .network = L"Home", .wifi = true, .signal = -1},
        wifiOnly{.connected = true, .wifi = true, .links = {{K::Wifi, 2, L"Wi-Fi", L"Home5G", true, true, 80}}},
        wiredOnly{.connected = true, .links = {{K::Ethernet, 1, L"Ethernet", L"Office", true, true, -1}}};
    DeriveNetworkStatus(legacy, true, true, L"Dialup", {true, true, L"Home", 75});
    DeriveNetworkStatus(both, false, true, L"", {});
    DeriveNetworkStatus(noPri, true, true, L"", {});
    Check(legacy.online && legacy.connected && legacy.wifi && legacy.signal == 75 && legacy.network == L"Dialup" && both.online && both.connected &&
              !both.wifi && both.network == L"CorpNet" && both.signal == 0 && noPri.online && noPri.connected && !noPri.wifi && noPri.network == L"LAN",
          "DeriveNetworkStatus");
    Check(FormatNetworkTooltip(single) == L"Wi-Fi Home", "single wifi unknown signal omits signal");
    SystemStatus airplaneOnly{.connected = false, .airplane = true};
    Check(FormatNetworkTooltip(airplaneOnly) == L"Airplane mode", "disconnected airplane mode tooltip");
    single.links = {{K::Wifi, 2, L"Wi-Fi", L"", true, false, -1},
                    {K::Ethernet, 1, L"Ethernet", L"Office", true, true, -1},
                    {K::Other, 3, L"WWAN", L"5G", false, false, -1},
                    {K::Wifi, 4, L"Wi-Fi 2", L"Guest", false, false, 60}};
    Check(FormatNetworkTooltip(single) == L"Office\nWi-Fi Wi-Fi\n5G\n+1 more" && WifiTileSubtitle(both, true, true, true, false) == L"Ethernet · Home5G" &&
              WifiTileSubtitle(both, true, true, true, true) == L"Ethernet · Connected" &&
              WifiTileSubtitle(both, true, true, false, false) == L"Off · Ethernet" && WifiTileSubtitle(wifiOnly, true, true, true, false) == L"Home5G" &&
              WifiTileSubtitle(wiredOnly, true, true, true, false) == L"Ethernet",
          "tooltip and control centre subtitles");

    if (g_failures) {
        std::fprintf(stderr, "%d network monitor check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("network monitor checks passed");
    return 0;
}
