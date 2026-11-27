// What the bar's status icon shows: battery, volume and network.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

struct NetworkLink {
    enum class Kind { Ethernet, Wifi, Other };
    Kind kind = Kind::Ethernet;
    uint64_t luid = 0;
    std::wstring alias, network;
    bool internet = false, primary = false;
    int signal = -1; // -1: unknown
    bool operator==(const NetworkLink&) const = default;
};

struct SystemStatus {
    bool battery = false;   // a system battery is present
    int batteryPercent = -1;
    bool charging = false;  // on mains power
    int batteryMinutes = -1; // estimated time left on battery; -1 unknown
    bool audio = false;     // a default playback device exists
    int volume = 0;         // 0..100
    bool muted = false;
    std::wstring audioDevice; // its name
    bool online = false;    // internet connectivity
    bool connected = false; // any network
    bool airplane = false;  // system radio switch is in airplane mode
    std::wstring network;   // name of the connected network
    bool wifi = false;      // the connection is Wi-Fi (else wired or other)
    int signal = 0;         // Wi-Fi signal 0..100, or -1 unknown
    std::vector<NetworkLink> links;
    int brightness = -1;    // the built-in screen 0..100 (the power notifications); -1 unknown
    bool operator==(const SystemStatus&) const = default;
};

std::wstring FormatNetworkTooltip(const SystemStatus& status);

} // namespace atrium
