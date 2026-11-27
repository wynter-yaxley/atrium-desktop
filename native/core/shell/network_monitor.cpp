// winsock2.h before windows.h: iphlpapi.h is written against it. ws2ipdef.h is what makes
// iphlpapi.h pull in netioapi.h, which is where GetIfTable2 and MIB_IF_ROW2 actually live: without
// it the include is silently a no-op and every type below is unknown.
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include "shell/network_monitor.h"

#include <utility>

namespace atrium {

namespace {

constexpr ULONGLONG kGapMs = 3000;  // a longer silence than this: the counters start again

// The link itself, not something stacked on it. Every filter driver on an adapter (the WFP layers,
// the QoS scheduler, Wi-Fi Direct) is reported as its own interface carrying the same counters, and
// so is a Hyper-V switch: counting them too would multiply the widget's rates. These two bits say
// exactly that, and unlike the names they read the same in every language — an alias such as
// "以太网-WFP Native MAC Layer LightWeight Filter-0000" is translated, "Realtek Gaming 2.5GbE" is not.
bool Usable(const MIB_IF_ROW2& row)
{
    if (row.MediaConnectState != MediaConnectStateConnected)
        return false;
    return row.InterfaceAndOperStatusFlags.HardwareInterface && !row.InterfaceAndOperStatusFlags.FilterInterface;
}

template<typename TrafficMap> const MIB_IF_ROW2* PickInterface(const MIB_IF_TABLE2& table, uint64_t currentLuid, const TrafficMap& before, TrafficMap& next)
{
    const MIB_IF_ROW2* busiest = nullptr; // carried the most bytes since the last reading
    const MIB_IF_ROW2* biggest = nullptr; // carried the most since the machine started
    const MIB_IF_ROW2* shown = nullptr;   // the one the widget shows now
    uint64_t busiestBytes = 0, biggestBytes = 0;
    for (ULONG i = 0; i < table.NumEntries; ++i) {
        const MIB_IF_ROW2& row = table.Table[i];
        if (!Usable(row))
            continue;
        next[row.InterfaceLuid.Value] = {row.InOctets, row.OutOctets};
        uint64_t moved = 0;
        if (const auto it = before.find(row.InterfaceLuid.Value); it != before.end() && row.InOctets >= it->second.in && row.OutOctets >= it->second.out) {
            moved = (row.InOctets - it->second.in) + (row.OutOctets - it->second.out);
        }
        if (moved > busiestBytes) {
            busiestBytes = moved;
            busiest = &row;
        }
        if (row.InOctets + row.OutOctets > biggestBytes) {
            biggestBytes = row.InOctets + row.OutOctets;
            biggest = &row;
        }
        if (row.InterfaceLuid.Value == currentLuid)
            shown = &row;
    }
    // The link carrying the traffic now; the one already shown while nothing moved (a widget that
    // jumped from WLAN to a sleeping Ethernet port while idle would be noise); else the busiest ever.
    return busiest ? busiest : shown ? shown : biggest;
}

template<typename TrafficMap> void ComputeRates(const MIB_IF_ROW2& pick, const TrafficMap& before, double dt, double& down, double& up, double& downBytes,
                                                double& upBytes, std::vector<NetworkRate>& history)
{
    down = up = 0;
    if (dt <= 0)
        return;
    if (const auto it = before.find(pick.InterfaceLuid.Value); it != before.end()) {
        // A counter that went backwards is an adapter that came back, not a huge burst.
        if (pick.InOctets >= it->second.in)
            down = double(pick.InOctets - it->second.in) / dt;
        if (pick.OutOctets >= it->second.out)
            up = double(pick.OutOctets - it->second.out) / dt;
    }
    downBytes += down * dt;
    upBytes += up * dt;
    history.push_back(NetworkRate{float(down), float(up)});
    if (history.size() > kNetworkPoints)
        history.erase(history.begin());
}

} // namespace

void NetworkMonitor::Sample()
{
    if (!active_)
        return;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || !table)
        return;
    const ULONGLONG now = GetTickCount64();
    const bool fresh = lastMs_ == 0 || now <= lastMs_ || now - lastMs_ > kGapMs;
    const double dt = fresh ? 0.0 : double(now - lastMs_) / 1000.0;
    // The counters of the reading before this one: the deltas are taken from them, and the ones of
    // an interface that has just appeared are unknown, so its first second reads as nothing.
    std::unordered_map<uint64_t, Traffic> before = std::move(last_);
    last_.clear();
    if (!fresh)
        last_.reserve(table->NumEntries);

    if (const MIB_IF_ROW2* pick = PickInterface(*table, luid_, before, last_)) {
        linked_ = true;
        luid_ = pick->InterfaceLuid.Value;
        name_ = pick->Alias[0] ? pick->Alias : pick->Description;
        linkMbps_ = LinkSpeedMbps(pick->ReceiveLinkSpeed);
        ComputeRates(*pick, before, dt, down_, up_, downBytes_, upBytes_, history_);
    } else {
        linked_ = false;
        luid_ = 0;
        name_.clear();
        linkMbps_ = 0;
        down_ = up_ = 0;
    }
    if (fresh) // a link that is new to the widget, or a silence: the graph starts over
        history_.clear();
    lastMs_ = now;
    FreeMibTable(table);
}

} // namespace atrium
