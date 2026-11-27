// winsock2.h before windows.h: iphlpapi.h requires it and ws2ipdef.h pulls in netioapi.h.
#include <winsock2.h>
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#include "shell/system_status.h"

#include "localization.h"
#include "log.h"
#include "shell/quick_toggles.h"
#include "shell/wifi.h"
#include "../posted.h"

#include <endpointvolume.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <netlistmgr.h>
#include <powrprof.h>
#include <shellapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace atrium {

namespace {

winrt::com_ptr<IAudioEndpointVolume> DefaultVolume(std::wstring* name = nullptr, std::wstring* id = nullptr)
{
    winrt::com_ptr<IMMDeviceEnumerator> devices;
    if (const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devices.put())); FAILED(hr) || !devices) {
        Log(L"status: CoCreateInstance(MMDeviceEnumerator) failed (0x%08X)", unsigned(hr));
        return nullptr;
    }
    winrt::com_ptr<IMMDevice> device;
    if (const HRESULT hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, device.put()); FAILED(hr) || !device) {
        Log(L"status: GetDefaultAudioEndpoint failed (0x%08X)", unsigned(hr));
        return nullptr;
    }
    winrt::com_ptr<IAudioEndpointVolume> volume;
    if (const HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, volume.put_void()); FAILED(hr) || !volume) {
        Log(L"status: Activate(IAudioEndpointVolume) failed (0x%08X)", unsigned(hr));
        return nullptr;
    }
    if (id) {
        LPWSTR rawId = nullptr;
        if (const HRESULT hr = device->GetId(&rawId); SUCCEEDED(hr) && rawId) {
            *id = rawId;
            CoTaskMemFree(rawId);
        } else {
            Log(L"status: default audio GetId failed (0x%08X)", unsigned(hr));
        }
    }
    winrt::com_ptr<IPropertyStore> props;
    if (name) {
        if (const HRESULT hr = device->OpenPropertyStore(STGM_READ, props.put()); SUCCEEDED(hr) && props) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                *name = v.pwszVal;
            PropVariantClear(&v);
        } else {
            Log(L"status: audio OpenPropertyStore failed (0x%08X)", unsigned(hr));
        }
    }
    return volume;
}

// Forwards Core Audio's volume notifications (on its own thread) as a message.
class VolumeCallback : public winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
public:
    VolumeCallback(HWND hwnd, UINT msg) : hwnd_(hwnd), msg_(msg) {}
    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override
    {
        PostMessageW(hwnd_, msg_, 0, 0);
        return S_OK;
    }

private:
    HWND hwnd_;
    UINT msg_;
};

int KindRank(NetworkLink::Kind k) { return k == NetworkLink::Kind::Ethernet ? 0 : k == NetworkLink::Kind::Wifi ? 1 : 2; }

std::optional<NetworkLink::Kind> UsableKind(const MIB_IF_ROW2& r)
{
    if (r.MediaConnectState != MediaConnectStateConnected || r.OperStatus != IfOperStatusUp || !r.InterfaceAndOperStatusFlags.HardwareInterface ||
        r.InterfaceAndOperStatusFlags.FilterInterface || r.TunnelType != TUNNEL_TYPE_NONE || r.Type == IF_TYPE_SOFTWARE_LOOPBACK)
        return std::nullopt;
    return r.Type == IF_TYPE_IEEE80211                                                                                    ? NetworkLink::Kind::Wifi :
           (r.Type == IF_TYPE_WWANPP || r.Type == IF_TYPE_WWANPP2 || r.PhysicalMediumType == NdisPhysicalMediumBluetooth) ? NetworkLink::Kind::Other :
           r.Type == IF_TYPE_ETHERNET_CSMACD                                                                              ? NetworkLink::Kind::Ethernet :
                                                                                                                            NetworkLink::Kind::Other;
}

std::wstring NetName(INetwork* net)
{
    BSTR b = nullptr;
    std::wstring n = net && SUCCEEDED(net->GetName(&b)) && b ? b : L"";
    SysFreeString(b);
    return n;
}

std::unordered_map<uint64_t, uint64_t> DefaultRouteMetrics()
{
    std::unordered_map<uint64_t, uint64_t> best;
    std::vector<std::tuple<uint64_t, ADDRESS_FAMILY, ULONG>> ifm;
    PMIB_IPINTERFACE_TABLE ifTable = nullptr;
    if (const DWORD err = GetIpInterfaceTable(AF_UNSPEC, &ifTable); err == NO_ERROR && ifTable) {
        for (ULONG i = 0; i < ifTable->NumEntries; ++i)
            ifm.emplace_back(ifTable->Table[i].InterfaceLuid.Value, ifTable->Table[i].Family, ifTable->Table[i].Metric);
        FreeMibTable(ifTable);
    } else if (err != NO_ERROR) {
        Log(L"status: GetIpInterfaceTable failed (%lu)", err);
    }
    PMIB_IPFORWARD_TABLE2 fwdTable = nullptr;
    if (const DWORD err = GetIpForwardTable2(AF_UNSPEC, &fwdTable); err == NO_ERROR && fwdTable) {
        for (ULONG i = 0; i < fwdTable->NumEntries; ++i) {
            const MIB_IPFORWARD_ROW2& r = fwdTable->Table[i];
            if (r.DestinationPrefix.PrefixLength != 0)
                continue;
            const uint64_t luid = r.InterfaceLuid.Value;
            auto it =
                std::ranges::find_if(ifm, [&](const auto& m) { return std::get<0>(m) == luid && std::get<1>(m) == r.DestinationPrefix.Prefix.si_family; });
            const uint64_t eff = uint64_t(r.Metric) + (it != ifm.end() ? std::get<2>(*it) : 0);
            if (auto [bit, ins] = best.emplace(luid, eff); !ins && eff < bit->second)
                bit->second = eff;
        }
        FreeMibTable(fwdTable);
    } else if (err != NO_ERROR) {
        Log(L"status: GetIpForwardTable2 failed (%lu)", err);
    }
    return best;
}

struct NlmAdapter {
    GUID guid{};
    std::wstring name;
    bool internet = false;
};

void ReadNlm(bool needGlobalNet, bool& online, bool& connected, std::wstring& globalNet, std::vector<NlmAdapter>& byAdapter)
{
    winrt::com_ptr<INetworkListManager> nlm;
    if (const HRESULT hr = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(nlm.put())); FAILED(hr) || !nlm) {
        Log(L"status: CoCreateInstance(NetworkListManager) failed (0x%08X)", unsigned(hr));
        return;
    }
    NLM_CONNECTIVITY c{};
    if (const HRESULT hr = nlm->GetConnectivity(&c); SUCCEEDED(hr)) {
        online = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
        connected = c != NLM_CONNECTIVITY_DISCONNECTED;
    } else {
        Log(L"status: NLM GetConnectivity failed (0x%08X)", unsigned(hr));
    }
    winrt::com_ptr<IEnumNetworkConnections> conns;
    if (const HRESULT hr = nlm->GetNetworkConnections(conns.put()); SUCCEEDED(hr) && conns) {
        for (winrt::com_ptr<INetworkConnection> conn; conns->Next(1, conn.put(), nullptr) == S_OK; conn = nullptr) {
            VARIANT_BOOL ok = VARIANT_FALSE;
            GUID gid{};
            NLM_CONNECTIVITY nc{};
            if (FAILED(conn->get_IsConnected(&ok)) || !ok || FAILED(conn->GetAdapterId(&gid)) || FAILED(conn->GetConnectivity(&nc)))
                continue;
            bool inet = (nc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
            winrt::com_ptr<INetwork> net;
            std::wstring n = SUCCEEDED(conn->GetNetwork(net.put())) ? NetName(net.get()) : L"";
            if (auto it = std::ranges::find_if(byAdapter, [&](const NlmAdapter& e) { return IsEqualGUID(e.guid, gid); }); it == byAdapter.end())
                byAdapter.push_back({gid, std::move(n), inet});
            else if (inet && !it->internet) {
                it->internet = true;
                it->name = n.empty() ? it->name : std::move(n);
            } else if (it->name.empty() && !n.empty())
                it->name = std::move(n);
        }
    } else {
        Log(L"status: NLM GetNetworkConnections failed (0x%08X)", unsigned(hr));
    }
    if (needGlobalNet) {
        winrt::com_ptr<IEnumNetworks> nets;
        if (const HRESULT hr = nlm->GetNetworks(NLM_ENUM_NETWORK_CONNECTED, nets.put()); SUCCEEDED(hr) && nets) {
            for (winrt::com_ptr<INetwork> net; nets->Next(1, net.put(), nullptr) == S_OK; net = nullptr) {
                NLM_CONNECTIVITY nc{};
                net->GetConnectivity(&nc);
                std::wstring n = NetName(net.get());
                if (nc & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) {
                    globalNet = std::move(n);
                    return;
                }
                globalNet = globalNet.empty() ? std::move(n) : globalNet;
            }
        } else {
            Log(L"status: NLM GetNetworks failed (0x%08X)", unsigned(hr));
        }
    }
}

void QueryNetwork(SystemStatus& s)
{
    std::vector<GUID> guids;
    PMIB_IF_TABLE2 ifTable = nullptr;
    if (const DWORD err = GetIfTable2(&ifTable); err == NO_ERROR && ifTable) {
        for (ULONG i = 0; i < ifTable->NumEntries; ++i) {
            const MIB_IF_ROW2& row = ifTable->Table[i];
            if (auto k = UsableKind(row)) {
                s.links.push_back({*k, row.InterfaceLuid.Value, row.Alias[0] ? row.Alias : row.Description});
                guids.push_back(row.InterfaceGuid);
            }
        }
        FreeMibTable(ifTable);
    } else if (err != NO_ERROR) {
        Log(L"status: GetIfTable2 failed (%lu)", err);
    }
    bool online = false, connected = false;
    std::wstring globalNet;
    std::vector<NlmAdapter> nlm;
    ReadNlm(s.links.empty(), online, connected, globalNet, nlm);
    if (s.links.empty()) {
        DeriveNetworkStatus(s, online, connected, globalNet, QueryWifi());
        return;
    }
    const auto wifi = QueryConnectedWifi();
    const auto metrics = DefaultRouteMetrics();
    std::vector<LinkRouteInfo> routes;
    for (size_t i = 0; i < s.links.size(); ++i) {
        NetworkLink& l = s.links[i];
        std::wstring nlmName;
        if (auto it = std::ranges::find_if(nlm, [&](const NlmAdapter& e) { return IsEqualGUID(e.guid, guids[i]); }); it != nlm.end()) {
            l.internet = it->internet;
            nlmName = it->name;
        }
        if (l.kind == NetworkLink::Kind::Wifi)
            if (auto it = std::ranges::find_if(wifi, [&](const WifiConnection& w) { return IsEqualGUID(w.interfaceGuid, guids[i]); }); it != wifi.end()) {
                l.signal = it->signal;
                l.network = it->ssid;
            }
        l.network = !l.network.empty() ? l.network : !nlmName.empty() ? nlmName : l.alias;
        routes.push_back({l.luid, l.kind, l.internet, metrics.contains(l.luid) ? metrics.at(l.luid) : ~uint64_t{0}});
    }
    if (int p = PickPrimaryLink(routes); p >= 0)
        s.links[size_t(p)].primary = true;
    std::ranges::sort(s.links, [](const NetworkLink& a, const NetworkLink& b) {
        return std::tuple(!a.primary, !a.internet, a.luid) < std::tuple(!b.primary, !b.internet, b.luid);
    });
    DeriveNetworkStatus(s, online, connected, globalNet, {});
}

} // namespace

int PickPrimaryLink(const std::vector<LinkRouteInfo>& c)
{
    auto k = [](const LinkRouteInfo& x) { return std::tuple(!x.internet, x.routeMetric, KindRank(x.kind), x.luid); };
    return c.empty() ? -1 : int(std::ranges::min_element(c, [&](const auto& a, const auto& b) { return k(a) < k(b); }) - c.begin());
}

void DeriveNetworkStatus(SystemStatus& s, bool globalOnline, bool globalConnected, const std::wstring& globalNetwork, const WifiState& fallbackWifi)
{
    if (s.links.empty()) {
        s.online = globalOnline;
        s.connected = globalConnected;
        s.wifi = fallbackWifi.connected && s.connected;
        s.signal = s.wifi ? fallbackWifi.signal : 0;
        s.network = s.wifi && !fallbackWifi.ssid.empty() && (globalNetwork.empty() || !s.online) ? fallbackWifi.ssid : globalNetwork;
        return;
    }
    auto k = [](const NetworkLink& l) { return std::tuple(!l.primary, !l.internet, KindRank(l.kind), l.luid); };
    const NetworkLink& p = *std::ranges::min_element(s.links, [&](const auto& a, const auto& b) { return k(a) < k(b); });
    s.connected = true;
    s.online = globalOnline || p.internet;
    s.wifi = p.kind == NetworkLink::Kind::Wifi;
    s.signal = s.wifi ? p.signal : 0;
    s.network = !p.network.empty() ? p.network : !p.alias.empty() ? p.alias : globalNetwork;
}

void SystemStatusReader::UnwatchVolume()
{
    if (watched_ && callback_)
        if (const HRESULT hr = watched_->UnregisterControlChangeNotify(callback_.get()); FAILED(hr))
            Log(L"status: UnregisterControlChangeNotify failed (0x%08X)", unsigned(hr));
    watched_ = nullptr;
    watchedEndpointId_.clear();
}

void SystemStatusReader::WatchVolume()
{
    std::wstring endpointId;
    winrt::com_ptr<IAudioEndpointVolume> volume = DefaultVolume(nullptr, &endpointId);
    if (!volume || endpointId.empty()) {
        UnwatchVolume();
        return;
    }
    float level = 0;
    if (watched_ && endpointId == watchedEndpointId_ && SUCCEEDED(watched_->GetMasterVolumeLevelScalar(&level)))
        return;
    UnwatchVolume();
    if (!callback_)
        callback_ = winrt::make<VolumeCallback>(notify_, volumeMessage_);
    if (const HRESULT hr = volume->RegisterControlChangeNotify(callback_.get()); SUCCEEDED(hr)) {
        watched_ = std::move(volume);
        watchedEndpointId_ = std::move(endpointId);
    } else {
        Log(L"status: RegisterControlChangeNotify failed (0x%08X)", unsigned(hr));
    }
}

SystemStatusReader::SystemStatusReader(HWND notify, UINT readyMessage, UINT volumeMessage)
    : notify_(notify),
      readyMessage_(readyMessage),
      volumeMessage_(volumeMessage),
      quit_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
      wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      thread_([this] { Run(); })
{
}

SystemStatusReader::~SystemStatusReader()
{
    SetEvent(quit_);
    thread_.join();
    CloseHandle(wake_);
    CloseHandle(quit_);
}

void SystemStatusReader::Refresh() { SetEvent(wake_); }

void SystemStatusReader::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Atrium system status");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        Log(L"status: CoInitializeEx failed (0x%08X)", unsigned(com));
        return;
    }
    HANDLE events[] = {quit_, wake_};
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        auto status = std::make_unique<SystemStatus>(QuerySystemStatus());
        WatchVolume();
        if (WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0)
            break;
        PostOwned(notify_, readyMessage_, 0, std::move(status));
    }
    UnwatchVolume();
    callback_ = nullptr;
    if (SUCCEEDED(com))
        CoUninitialize();
}

SystemStatus QuerySystemStatus()
{
    SystemStatus s;
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps)) {
        // 128: no system battery; 255: unknown.
        s.battery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255 && ps.BatteryLifePercent <= 100;
        s.batteryPercent = s.battery ? ps.BatteryLifePercent : -1;
        s.charging = ps.ACLineStatus == 1;
        s.batteryMinutes = s.battery && !s.charging && ps.BatteryLifeTime != DWORD(-1) ? int(ps.BatteryLifeTime / 60) : -1;
    } else {
        Log(L"status: GetSystemPowerStatus failed (%lu)", GetLastError());
    }
    s.airplane = ReadAirplaneModeFast();
    QueryNetwork(s);
    // Read interactive volume last: a slower network query must not
    // deliver a volume value captured before the user's latest input.
    if (auto v = DefaultVolume(&s.audioDevice)) {
        float level = 0;
        BOOL mute = FALSE;
        const HRESULT levelHr = v->GetMasterVolumeLevelScalar(&level);
        const HRESULT muteHr = v->GetMute(&mute);
        if (FAILED(levelHr))
            Log(L"status: GetMasterVolumeLevelScalar failed (0x%08X)", unsigned(levelHr));
        if (FAILED(muteHr))
            Log(L"status: GetMute failed (0x%08X)", unsigned(muteHr));
        if (SUCCEEDED(levelHr)) {
            s.audio = true;
            s.volume = int(std::lround(std::clamp(level, 0.f, 1.f) * 100));
            s.muted = SUCCEEDED(muteHr) && mute != FALSE;
        }
    }
    return s;
}

int AdjustVolume(int delta)
{
    auto v = DefaultVolume();
    if (!v)
        return -1;
    float level = 0;
    if (const HRESULT hr = v->GetMasterVolumeLevelScalar(&level); FAILED(hr)) {
        Log(L"status: GetMasterVolumeLevelScalar failed (0x%08X)", unsigned(hr));
        return -1;
    }
    level = std::clamp(level + float(delta) / 100.f, 0.f, 1.f);
    if (const HRESULT hr = v->SetMasterVolumeLevelScalar(level, nullptr); FAILED(hr)) {
        Log(L"status: SetMasterVolumeLevelScalar failed (0x%08X)", unsigned(hr));
        return -1;
    }
    if (delta > 0)
        if (const HRESULT hr = v->SetMute(FALSE, nullptr); FAILED(hr))
            Log(L"status: SetMute failed (0x%08X)", unsigned(hr));
    return int(std::lround(level * 100));
}

void SetVolume(int percent)
{
    if (auto v = DefaultVolume()) {
        if (const HRESULT hr = v->SetMasterVolumeLevelScalar(float(std::clamp(percent, 0, 100)) / 100.f, nullptr); FAILED(hr))
            Log(L"status: SetMasterVolumeLevelScalar failed (0x%08X)", unsigned(hr));
        if (percent > 0)
            if (const HRESULT hr = v->SetMute(FALSE, nullptr); FAILED(hr))
                Log(L"status: SetMute failed (0x%08X)", unsigned(hr));
    }
}

bool ToggleMute()
{
    auto v = DefaultVolume();
    if (!v)
        return false;
    BOOL mute = FALSE;
    if (const HRESULT hr = v->GetMute(&mute); FAILED(hr)) {
        Log(L"status: GetMute failed (0x%08X)", unsigned(hr));
        return false;
    }
    if (const HRESULT hr = v->SetMute(!mute, nullptr); FAILED(hr)) {
        Log(L"status: SetMute failed (0x%08X)", unsigned(hr));
        return false;
    }
    return true;
}

namespace {

struct HibernateWorkerJob {
    HWND notifyHost = nullptr;
    UINT noticeMsg = 0;
};

std::atomic<bool> g_hibernateInFlight{false};

struct ClearHibernateInFlight {
    ClearHibernateInFlight() = default;
    ~ClearHibernateInFlight() { g_hibernateInFlight.store(false, std::memory_order_release); }
    ClearHibernateInFlight(const ClearHibernateInFlight&) = delete;
    ClearHibernateInFlight& operator=(const ClearHibernateInFlight&) = delete;
};

void EnableShutdownPrivilege()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(token);
}

std::wstring SystemPowerCfgPath()
{
    wchar_t sys[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return L"powercfg.exe";
    return std::wstring(sys, n) + L"\\powercfg.exe";
}

void PostPowerNotice(HWND notifyHost, UINT noticeMsg, PowerAction action, PowerActionStatus status)
{
    if (!notifyHost || !noticeMsg || !IsWindow(notifyHost))
        return;
    if (auto notice = BuildPowerActionNotice(action, status))
        PostOwned(notifyHost, noticeMsg, 0, std::make_unique<Notice>(std::move(*notice)));
}

VOID CALLBACK HibernateWorkerProc(PTP_CALLBACK_INSTANCE, PVOID context)
{
    const ClearHibernateInFlight clearOnExit;
    const std::unique_ptr<HibernateWorkerJob> job(static_cast<HibernateWorkerJob*>(context));
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    DWORD err = 0;
    const PowerActionStatus status = ExecuteHibernateAction(&err);
    if (status != PowerActionStatus::Ok)
        PostPowerNotice(job->notifyHost, job->noticeMsg, PowerAction::Hibernate, status);
    if (SUCCEEDED(com))
        CoUninitialize();
}

} // namespace

HibernateCapability ClassifyHibernateCapability(bool queryOk, const SYSTEM_POWER_CAPABILITIES& caps)
{
    if (!queryOk)
        return HibernateCapability::QueryFailed;
    if (!caps.SystemS4)
        return HibernateCapability::Unsupported;
    constexpr BYTE kHiberFileTypeFull = 2; // HIBERFILE_TYPE_FULL (0=none, 1=reduced/fast-startup only, 2=full)
    return (caps.HiberFilePresent && caps.HiberFileType == kHiberFileTypeFull) ? HibernateCapability::Direct : HibernateCapability::NeedsEnable;
}

HibernateCapability QueryHibernateCapability(DWORD* error)
{
    SYSTEM_POWER_CAPABILITIES c{};
    if (!GetPwrCapabilities(&c)) {
        const DWORD err = GetLastError();
        if (error)
            *error = err;
        Log(L"power: GetPwrCapabilities failed (%lu)", err);
        return HibernateCapability::QueryFailed;
    }
    if (error)
        *error = 0;
    return ClassifyHibernateCapability(true, c);
}

HibernateMenuOption DescribeHibernateMenu(HibernateCapability capability)
{
    switch (capability) {
    case HibernateCapability::Direct: return {true, false, Tr(L"Hibernate")};
    case HibernateCapability::NeedsEnable: return {true, true, Tr(L"Enable hibernation and hibernate")};
    case HibernateCapability::Unsupported:
    case HibernateCapability::QueryFailed: break;
    }
    return {};
}

std::optional<Notice> BuildPowerActionNotice(PowerAction action, PowerActionStatus status)
{
    if (status == PowerActionStatus::Ok || action != PowerAction::Hibernate)
        return std::nullopt;
    Notice notice;
    notice.source = L"atrium";
    notice.app = L"Atrium";
    notice.unread = false;
    switch (status) {
    case PowerActionStatus::Cancelled:
        notice.title = Tr(L"Hibernation cancelled");
        notice.body = Tr(L"Administrator permission is required to enable hibernation.");
        return notice;
    case PowerActionStatus::ElevationFailed:
        notice.title = Tr(L"Could not enable hibernation");
        notice.body = Tr(L"Administrator permission could not be requested.");
        return notice;
    case PowerActionStatus::EnableFailed:
        notice.title = Tr(L"Could not enable hibernation");
        notice.body = Tr(L"Enabling hibernation failed. Check power settings or free disk space.");
        return notice;
    case PowerActionStatus::QueryFailed:
        notice.title = Tr(L"Could not check hibernation");
        notice.body = Tr(L"Could not read the system's hibernation state.");
        return notice;
    case PowerActionStatus::Unsupported:
        notice.title = Tr(L"Hibernation is unavailable");
        notice.body = Tr(L"This PC does not support hibernation.");
        return notice;
    case PowerActionStatus::HibernateFailed:
    case PowerActionStatus::Ok:
        notice.title = Tr(L"Could not hibernate");
        notice.body = Tr(L"Windows did not enter hibernation.");
        return notice;
    }
    return notice;
}

bool CanHibernate() { return QueryHibernateCapability() == HibernateCapability::Direct; }

PowerActionStatus ExecuteHibernateAction(DWORD* error)
{
    DWORD err = 0;
    HibernateCapability cap = QueryHibernateCapability(&err);
    if (cap == HibernateCapability::QueryFailed) {
        if (error)
            *error = err;
        return PowerActionStatus::QueryFailed;
    }
    if (cap == HibernateCapability::Unsupported)
        return PowerActionStatus::Unsupported;
    if (cap == HibernateCapability::NeedsEnable) {
        Log(L"power: hibernation is disabled; requesting elevation to enable and hibernate");
        const std::wstring powercfg = SystemPowerCfgPath();
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = powercfg.c_str();
        sei.lpParameters = L"/h on";
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            err = GetLastError();
            if (error)
                *error = err;
            if (err == ERROR_CANCELLED) {
                Log(L"power: hibernate elevation cancelled by user");
                return PowerActionStatus::Cancelled;
            }
            Log(L"power: failed to elevate hibernate enablement (%lu)", err);
            return PowerActionStatus::ElevationFailed;
        }
        DWORD exitCode = 1;
        if (sei.hProcess) {
            const DWORD wait = WaitForSingleObject(sei.hProcess, 60000);
            if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(sei.hProcess, &exitCode))
                exitCode = 1;
            CloseHandle(sei.hProcess);
        }
        if (exitCode != 0) {
            if (error)
                *error = exitCode;
            Log(L"power: powercfg /h on failed (%lu)", exitCode);
            return PowerActionStatus::EnableFailed;
        }
        if (QueryHibernateCapability(&err) != HibernateCapability::Direct) {
            if (error)
                *error = err;
            Log(L"power: hibernation still unavailable after enabling (%lu)", err);
            return PowerActionStatus::EnableFailed;
        }
    }
    EnableShutdownPrivilege();
    if (!SetSuspendState(TRUE, FALSE, FALSE)) {
        err = GetLastError();
        if (error)
            *error = err;
        Log(L"power: SetSuspendState(hibernate) failed (%lu)", err);
        return PowerActionStatus::HibernateFailed;
    }
    return PowerActionStatus::Ok;
}

bool RunPowerAction(PowerAction action, HWND notifyHost, UINT noticeMsg)
{
    const wchar_t* names[] = {L"sleep", L"hibernate", L"restart", L"shutdown"};
    Log(L"power: %s", names[int(action)]);
    if (action == PowerAction::Sleep)
        return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
    if (action == PowerAction::Hibernate) {
        bool expected = false;
        if (!g_hibernateInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            Log(L"power: hibernate already in progress");
            return true;
        }
        if (notifyHost && noticeMsg) {
            auto job = std::make_unique<HibernateWorkerJob>(HibernateWorkerJob{notifyHost, noticeMsg});
            if (TrySubmitThreadpoolCallback(HibernateWorkerProc, job.get(), nullptr)) {
                [[maybe_unused]] HibernateWorkerJob* raw = job.release();
                return true;
            }
            g_hibernateInFlight.store(false, std::memory_order_release);
            Log(L"power: threadpool dispatch failed (%lu)", GetLastError());
            PostPowerNotice(notifyHost, noticeMsg, PowerAction::Hibernate, PowerActionStatus::HibernateFailed);
            return false;
        }
        const ClearHibernateInFlight clearOnExit;
        return ExecuteHibernateAction() == PowerActionStatus::Ok;
    }
    // Shutting down needs SeShutdownPrivilege enabled in the token (every
    // interactive user has it, disabled by default).
    EnableShutdownPrivilege();
    UINT flags = action == PowerAction::Restart ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF;
    bool ok = ExitWindowsEx(flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED) != FALSE;
    if (!ok)
        Log(L"power: ExitWindowsEx failed (%lu)", GetLastError());
    return ok;
}

} // namespace atrium
