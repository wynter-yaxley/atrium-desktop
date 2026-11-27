#include "shell/radios.h"

#include "log.h"

#include <wbemidl.h>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>

namespace atrium {

using namespace winrt;
using namespace winrt::Windows::Devices;

namespace {

template<typename DeviceT> void CollectBluetoothDevices(bool lowEnergy, std::vector<BluetoothDevice>& raw)
{
    for (const auto& d : Enumeration::DeviceInformation::FindAllAsync(DeviceT::GetDeviceSelectorFromPairingState(true)).get()) {
        BluetoothDevice b;
        b.name = std::wstring(d.Name());
        b.lowEnergy = lowEnergy;
        try {
            if (auto dev = DeviceT::FromIdAsync(d.Id()).get())
                b.connected = dev.ConnectionStatus() == Bluetooth::BluetoothConnectionStatus::Connected;
        } catch (const winrt::hresult_error& error) {
            Log(L"radios: reading %s Bluetooth device state failed (0x%08X)", lowEnergy ? L"LE" : L"classic", unsigned(error.code()));
        }
        raw.push_back(std::move(b));
    }
}

} // namespace

RadioState QueryRadios()
{
    RadioState s;
    try {
        for (const Radios::Radio& r : Radios::Radio::GetRadiosAsync().get()) {
            const bool on = r.State() == Radios::RadioState::On;
            if (r.Kind() == Radios::RadioKind::WiFi) {
                s.wifiPresent = true;
                s.wifiOn = s.wifiOn || on;
            } else if (r.Kind() == Radios::RadioKind::Bluetooth) {
                s.btPresent = true;
                s.btOn = s.btOn || on;
            }
        }
    } catch (const winrt::hresult_error& error) {
        Log(L"radios: reading the radios failed (0x%08X)", unsigned(error.code()));
    }
    return s;
}

bool SetRadio(bool bluetooth, bool on)
{
    bool allowed = false;
    bool ok = false;
    try {
        allowed = Radios::Radio::RequestAccessAsync().get() == Radios::RadioAccessStatus::Allowed;
        if (allowed) {
            for (const Radios::Radio& r : Radios::Radio::GetRadiosAsync().get())
                if (r.Kind() == (bluetooth ? Radios::RadioKind::Bluetooth : Radios::RadioKind::WiFi))
                    ok = r.SetStateAsync(on ? Radios::RadioState::On : Radios::RadioState::Off).get() == Radios::RadioAccessStatus::Allowed || ok;
        }
    } catch (const winrt::hresult_error& error) {
        Log(L"radios: switching the radio failed (0x%08X)", unsigned(error.code()));
        return false;
    }
    if (!allowed) {
        Log(L"radios: access to the radios was refused");
        return false;
    }
    Log(L"radios: %s %s %s", bluetooth ? L"Bluetooth" : L"Wi-Fi", on ? L"on" : L"off", ok ? L"done" : L"refused");
    return ok;
}

std::vector<BluetoothDevice> ListBluetooth()
{
    std::vector<BluetoothDevice> raw;
    try {
        // Classic devices (headsets, keyboards, phones), then Bluetooth LE.
        CollectBluetoothDevices<Bluetooth::BluetoothDevice>(false, raw);
        CollectBluetoothDevices<Bluetooth::BluetoothLEDevice>(true, raw);
    } catch (const winrt::hresult_error& error) {
        Log(L"radios: listing Bluetooth devices failed (0x%08X)", unsigned(error.code()));
    }
    std::vector<BluetoothDevice> out;
    for (const BluetoothDevice& b : raw) {
        if (b.name.empty())
            continue;
        auto it = std::ranges::find_if(out, [&](const BluetoothDevice& existing) { return existing.name == b.name; });
        if (it != out.end())
            it->connected |= b.connected;
        else
            out.push_back(b);
    }
    std::ranges::stable_sort(out, [](const BluetoothDevice& a, const BluetoothDevice& b) { return a.connected > b.connected; });
    return out;
}

namespace {

// ROOT\WMI, where laptops publish the built-in screen's brightness.
com_ptr<IWbemServices> Wmi()
{
    com_ptr<IWbemLocator> locator;
    com_ptr<IWbemServices> services;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(locator.put()))))
        return nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put());
    SysFreeString(ns);
    if (FAILED(hr))
        return nullptr;
    CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return services;
}

// The first instance of a WMI class.
com_ptr<IWbemClassObject> First(IWbemServices* wmi, const wchar_t* query)
{
    com_ptr<IEnumWbemClassObject> e;
    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
    HRESULT hr = wmi->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, e.put());
    SysFreeString(lang);
    SysFreeString(q);
    com_ptr<IWbemClassObject> o;
    ULONG n = 0;
    if (FAILED(hr) || !e || FAILED(e->Next(2000, 1, o.put(), &n)) || n == 0)
        return nullptr;
    return o;
}

} // namespace

int GetBrightness()
{
    com_ptr<IWbemServices> wmi = Wmi();
    com_ptr<IWbemClassObject> o = wmi ? First(wmi.get(), L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE") : nullptr;
    if (!o)
        return -1;
    VARIANT v;
    VariantInit(&v);
    int out = -1;
    if (SUCCEEDED(o->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && (v.vt == VT_UI1 || v.vt == VT_I4))
        out = v.vt == VT_UI1 ? int(v.bVal) : int(v.lVal);
    VariantClear(&v);
    return out;
}

bool SetBrightness(int percent)
{
    com_ptr<IWbemServices> wmi = Wmi();
    com_ptr<IWbemClassObject> inst = wmi ? First(wmi.get(), L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE") : nullptr;
    if (!inst)
        return false;
    VARIANT path;
    VariantInit(&path);
    com_ptr<IWbemClassObject> cls, inDef, in;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods"), method = SysAllocString(L"WmiSetBrightness");
    bool ok = false;
    if (SUCCEEDED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR &&
        SUCCEEDED(wmi->GetObject(clsName, 0, nullptr, cls.put(), nullptr)) && SUCCEEDED(cls->GetMethod(method, 0, inDef.put(), nullptr)) && inDef &&
        SUCCEEDED(inDef->SpawnInstance(0, in.put()))) {
        VARIANT t, b;
        t.vt = VT_I4;
        t.lVal = 0;
        b.vt = VT_UI1;
        b.bVal = BYTE(std::clamp(percent, 0, 100));
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        ok = SUCCEEDED(wmi->ExecMethod(path.bstrVal, method, 0, nullptr, in.get(), nullptr, nullptr));
    }
    VariantClear(&path);
    SysFreeString(clsName);
    SysFreeString(method);
    return ok;
}

} // namespace atrium
