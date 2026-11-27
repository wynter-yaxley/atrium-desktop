#include "shell/quick_toggles.h"

#include <windows.h>
#include <asyncinfo.h>
#include <powrprof.h>
#include <powersetting.h>
#include <roapi.h>
#include <windows.networking.connectivity.h>
#include <windows.networking.networkoperators.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace atrium {

namespace {

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Networking::Connectivity;
using namespace ABI::Windows::Networking::NetworkOperators;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

constexpr wchar_t kNightLightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kEnergySaverKey[] = L"Software\\Atrium\\EnergySaver";
constexpr wchar_t kRadioStateKey[] = L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement\\SystemRadioState";
constexpr CLSID kRadioManagementClsid = {0x581333F6, 0x28DB, 0x41BE, {0xBC, 0x7A, 0xFF, 0x20, 0x1F, 0x12, 0xF3, 0xF6}};
constexpr IID kIRadioManagerIid = {0xDB3AFBFB, 0x08E6, 0x46C6, {0xAA, 0x70, 0xBF, 0x9A, 0x34, 0xC3, 0x0A, 0xB7}};

struct IRadioManager : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsRMSupported(DWORD* pdwState) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUIRadioInstances(IUnknown** ppCollection) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSystemRadioState(int* pbEnabled, int* param2, DWORD* pReason) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSystemRadioState(int bEnabled) = 0;
};
constexpr size_t kTimestampOffset = 10;
constexpr uint32_t kDefaultAcThreshold = 0;
constexpr uint32_t kDefaultDcThreshold = 20;

std::mutex g_energyMutex;
std::map<std::wstring, uint32_t> g_prevThresholds;

std::wstring SchemeValueName(const GUID& scheme, bool ac)
{
    wchar_t buf[64]{};
    const int n = StringFromGUID2(scheme, buf, static_cast<int>(std::size(buf)));
    return (n > 0 ? std::wstring(buf) : std::wstring(L"{default}")) + (ac ? L"_AC" : L"_DC");
}

std::optional<uint32_t> LoadSavedThreshold(const std::wstring& keyName)
{
    if (const auto it = g_prevThresholds.find(keyName); it != g_prevThresholds.end())
        return it->second;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kEnergySaverKey, keyName.c_str(), RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    g_prevThresholds[keyName] = value;
    return value;
}

void StoreSavedThreshold(const std::wstring& keyName, uint32_t value)
{
    g_prevThresholds[keyName] = value;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kEnergySaverKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD dword = value;
        RegSetValueExW(key, keyName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&dword), sizeof(dword));
        RegCloseKey(key);
    }
}

void ClearSavedThreshold(const std::wstring& keyName)
{
    g_prevThresholds.erase(keyName);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnergySaverKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, keyName.c_str());
        RegCloseKey(key);
    }
}

size_t VarintEnd(const std::vector<uint8_t>& data, size_t start)
{
    size_t i = start;
    while (i < data.size() && (data[i] & 0x80u) != 0)
        ++i;
    return i < data.size() ? i + 1 : data.size();
}

uint64_t DecodeVarint(const std::vector<uint8_t>& data, size_t start, size_t end)
{
    uint64_t value = 0;
    for (size_t i = end; i > start; --i)
        value = (value << 7) | (data[i - 1] & 0x7Fu);
    return value;
}

std::vector<uint8_t> EncodeVarint(uint64_t value)
{
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0)
            byte |= 0x80u;
        out.push_back(byte);
    } while (value != 0);
    return out;
}

std::optional<DWORD> ReadRegDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(root, subKey, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return value;
}

std::optional<std::vector<uint8_t>> ReadNightLightRaw()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data", RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return std::nullopt;
    std::vector<uint8_t> data(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kNightLightKey, L"Data", RRF_RT_REG_BINARY, nullptr, data.data(), &size) != ERROR_SUCCESS)
        return std::nullopt;
    data.resize(size);
    return data;
}

ComPtr<INetworkOperatorTetheringManager> OpenTetheringManager()
{
    ComPtr<INetworkInformationStatics> netInfo;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Networking_Connectivity_NetworkInformation).Get(), IID_PPV_ARGS(&netInfo))))
        return nullptr;
    ComPtr<IConnectionProfile> profile;
    if (FAILED(netInfo->GetInternetConnectionProfile(&profile)) || !profile)
        return nullptr;

    ComPtr<INetworkOperatorTetheringManagerStatics2> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Networking_NetworkOperators_NetworkOperatorTetheringManager).Get(),
                                      IID_PPV_ARGS(&statics))))
        return nullptr;

    TetheringCapability cap = TetheringCapability_DisabledByHardwareLimitation;
    if (FAILED(statics->GetTetheringCapabilityFromConnectionProfile(profile.Get(), &cap)) || cap != TetheringCapability_Enabled)
        return nullptr;

    ComPtr<INetworkOperatorTetheringManager> mgr;
    if (FAILED(statics->CreateFromConnectionProfile(profile.Get(), &mgr)))
        return nullptr;
    return mgr;
}

bool AwaitTetheringOp(IAsyncOperation<NetworkOperatorTetheringOperationResult*>* op)
{
    if (!op)
        return false;
    HANDLE rawDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!rawDone)
        return false;
    std::shared_ptr<void> done(rawDone, &CloseHandle);
    auto handler = Callback<IAsyncOperationCompletedHandler<NetworkOperatorTetheringOperationResult*>>(
        [done](IAsyncOperation<NetworkOperatorTetheringOperationResult*>*, AsyncStatus) -> HRESULT {
            SetEvent(done.get());
            return S_OK;
        });
    if (!handler || FAILED(op->put_Completed(handler.Get())))
        return false;
    if (WaitForSingleObject(done.get(), 15000) != WAIT_OBJECT_0) {
        ComPtr<IAsyncInfo> info;
        if (SUCCEEDED(op->QueryInterface(IID_PPV_ARGS(&info))) && info)
            info->Cancel();
        return false;
    }

    ComPtr<INetworkOperatorTetheringOperationResult> result;
    if (FAILED(op->GetResults(&result)) || !result)
        return false;
    TetheringOperationStatus status = TetheringOperationStatus_Unknown;
    return SUCCEEDED(result->get_Status(&status)) && status == TetheringOperationStatus_Success;
}

} // namespace

std::optional<bool> IsNightLightOn(const std::vector<uint8_t>& data)
{
    if (data.size() <= kTimestampOffset)
        return std::nullopt;
    const size_t p = VarintEnd(data, kTimestampOffset);
    if (p + 10 > data.size())
        return std::nullopt;
    if (data[p] != 0x2A || data[p + 1] != 0x2B || data[p + 2] != 0x0E)
        return std::nullopt;
    return data[p + 8] == 0x10 && data[p + 9] == 0x00;
}

std::vector<uint8_t> ToggleNightLightData(const std::vector<uint8_t>& data, bool on, uint64_t timestampSec)
{
    const auto wasOn = IsNightLightOn(data);
    if (!wasOn.has_value())
        return {};
    const size_t tsEnd = VarintEnd(data, kTimestampOffset);
    const uint64_t oldTs = DecodeVarint(data, kTimestampOffset, tsEnd);
    const std::vector<uint8_t> tsBytes = EncodeVarint(timestampSec > oldTs ? timestampSec : oldTs + 1);

    std::vector<uint8_t> out;
    out.reserve(data.size() + 4);
    out.insert(out.end(), data.begin(), data.begin() + kTimestampOffset);
    out.insert(out.end(), tsBytes.begin(), tsBytes.end());
    out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8));

    const size_t lenIdx = kTimestampOffset + tsBytes.size() + 3;
    if (*wasOn == on) {
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8), data.end());
    } else if (on) {
        out[lenIdx] = static_cast<uint8_t>(out[lenIdx] + 2);
        out.push_back(0x10);
        out.push_back(0x00);
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 8), data.end());
    } else {
        out[lenIdx] = static_cast<uint8_t>(out[lenIdx] - 2);
        out.insert(out.end(), data.begin() + static_cast<ptrdiff_t>(tsEnd + 10), data.end());
    }
    return out;
}

std::optional<bool> ReadNightLight()
{
    const auto data = ReadNightLightRaw();
    if (!data.has_value())
        return std::nullopt;
    return IsNightLightOn(*data);
}

bool SetNightLight(bool on)
{
    const auto data = ReadNightLightRaw();
    if (!data.has_value())
        return false;
    const auto nowSec = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const std::vector<uint8_t> next = ToggleNightLightData(*data, on, nowSec);
    if (next.empty())
        return false;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNightLightKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const LSTATUS rc = RegSetValueExW(key, L"Data", 0, REG_BINARY, next.data(), static_cast<DWORD>(next.size()));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::optional<bool> ReadEnergySaver()
{
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps))
        return std::nullopt;
    return (sps.SystemStatusFlag & 1u) != 0;
}

struct EnergySaverInput {
    bool on = false;
    bool onAc = false;
    uint32_t curAc = 0;
    uint32_t curDc = 0;
    uint32_t defaultAc = kDefaultAcThreshold;
    uint32_t defaultDc = kDefaultDcThreshold;
    std::optional<uint32_t> savedAc;
    std::optional<uint32_t> savedDc;
};

struct EnergySaverPlan {
    std::optional<uint32_t> writeAc;
    std::optional<uint32_t> writeDc;
    std::optional<uint32_t> saveAc;
    std::optional<uint32_t> saveDc;
    bool clearAc = false;
    bool clearDc = false;
    bool NeedsApply() const { return writeAc.has_value() || writeDc.has_value(); }
};

EnergySaverPlan PlanEnergySaver(const EnergySaverInput& in)
{
    EnergySaverPlan plan;
    if (in.on) {
        if (in.onAc) {
            if (!in.savedAc.has_value() && in.curAc < 100)
                plan.saveAc = in.curAc;
            if (in.curAc != 100)
                plan.writeAc = 100;
        } else {
            if (!in.savedDc.has_value() && in.curDc < 100)
                plan.saveDc = in.curDc;
            if (in.curDc != 100)
                plan.writeDc = 100;
        }
        return plan;
    }

    auto planRestore = [](uint32_t cur, std::optional<uint32_t> saved, uint32_t fallback, bool activeSource, std::optional<uint32_t>& write, bool& clear) {
        const uint32_t safeFallback = fallback < 100 ? fallback : kDefaultDcThreshold;
        if (saved.has_value()) {
            const uint32_t restore = *saved < 100 ? *saved : safeFallback;
            if (cur != restore)
                write = restore;
            clear = true;
        } else if (activeSource && cur >= 100 && cur != fallback) {
            write = fallback;
        }
    };
    const uint32_t acFallback = in.defaultAc < 100 ? in.defaultAc : kDefaultAcThreshold;
    const uint32_t dcFallback = in.defaultDc < 100 ? in.defaultDc : kDefaultDcThreshold;
    planRestore(in.curAc, in.savedAc, acFallback, in.onAc, plan.writeAc, plan.clearAc);
    planRestore(in.curDc, in.savedDc, dcFallback, !in.onAc, plan.writeDc, plan.clearDc);
    return plan;
}

bool SetEnergySaver(bool on)
{
    std::scoped_lock lock(g_energyMutex);
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active)
        return false;

    SYSTEM_POWER_STATUS sps{};
    const bool onAc = GetSystemPowerStatus(&sps) && sps.ACLineStatus == 1;
    DWORD curAc = 0, curDc = 0, defAc = kDefaultAcThreshold, defDc = kDefaultDcThreshold;
    const bool hasAc = PowerReadACValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &curAc) == ERROR_SUCCESS;
    const bool hasDc = PowerReadDCValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &curDc) == ERROR_SUCCESS;
    if (PowerReadACDefaultIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &defAc) != ERROR_SUCCESS)
        defAc = kDefaultAcThreshold;
    if (PowerReadDCDefaultIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &defDc) != ERROR_SUCCESS)
        defDc = kDefaultDcThreshold;

    const std::wstring acKey = SchemeValueName(*active, true);
    const std::wstring dcKey = SchemeValueName(*active, false);
    const EnergySaverInput input{
        .on = on,
        .onAc = onAc,
        .curAc = hasAc ? curAc : (onAc ? 0u : defAc),
        .curDc = hasDc ? curDc : kDefaultDcThreshold,
        .defaultAc = defAc,
        .defaultDc = defDc,
        .savedAc = LoadSavedThreshold(acKey),
        .savedDc = LoadSavedThreshold(dcKey),
    };
    const EnergySaverPlan plan = PlanEnergySaver(input);

    bool ok = true;
    if (plan.writeAc.has_value()) {
        ok = PowerWriteACValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, static_cast<DWORD>(*plan.writeAc)) ==
             ERROR_SUCCESS;
    }
    if (ok && plan.writeDc.has_value()) {
        ok = PowerWriteDCValueIndex(nullptr, active, &GUID_ENERGY_SAVER_SUBGROUP, &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, static_cast<DWORD>(*plan.writeDc)) ==
             ERROR_SUCCESS;
    }
    if (ok && plan.NeedsApply())
        ok = PowerSetActiveScheme(nullptr, active) == ERROR_SUCCESS;

    if (ok) {
        if (plan.saveAc.has_value())
            StoreSavedThreshold(acKey, *plan.saveAc);
        if (plan.saveDc.has_value())
            StoreSavedThreshold(dcKey, *plan.saveDc);
        if (plan.clearAc)
            ClearSavedThreshold(acKey);
        if (plan.clearDc)
            ClearSavedThreshold(dcKey);
    }
    LocalFree(active);
    return ok;
}

std::optional<bool> ReadDarkMode()
{
    const auto apps = ReadRegDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme");
    const auto system = ReadRegDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme");
    if (!apps.has_value() || !system.has_value())
        return std::nullopt;
    return *apps == 0 && *system == 0;
}

bool SetDarkMode(bool on)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const DWORD val = on ? 0 : 1;
    const LSTATUS r1 = RegSetValueExW(key, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    const LSTATUS r2 = RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(key);
    if (r1 != ERROR_SUCCESS || r2 != ERROR_SUCCESS)
        return false;

    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 5000, nullptr);
    return true;
}

std::optional<bool> ReadMobileHotspot()
{
    const auto mgr = OpenTetheringManager();
    if (!mgr)
        return std::nullopt;
    TetheringOperationalState state = TetheringOperationalState_Unknown;
    if (FAILED(mgr->get_TetheringOperationalState(&state)))
        return std::nullopt;
    return state == TetheringOperationalState_On;
}

bool SetMobileHotspot(bool on)
{
    const auto mgr = OpenTetheringManager();
    if (!mgr)
        return false;
    ComPtr<IAsyncOperation<NetworkOperatorTetheringOperationResult*>> op;
    const HRESULT hr = on ? mgr->StartTetheringAsync(&op) : mgr->StopTetheringAsync(&op);
    if (FAILED(hr))
        return false;
    return AwaitTetheringOp(op.Get());
}

std::optional<bool> ParseAirplaneRadioState(HRESULT isSupportedHr, uint32_t supported, HRESULT stateHr, int systemRadiosEnabled)
{
    if (FAILED(isSupportedHr) || supported == 0 || FAILED(stateHr))
        return std::nullopt;
    return systemRadiosEnabled == 0;
}

bool IsQuietProfileActive(const std::wstring& profile) { return !profile.empty() && profile != L"Microsoft.QuietHoursProfile.Unrestricted"; }

std::optional<bool> ReadAirplaneMode()
{
    ComPtr<IRadioManager> mgr;
    if (FAILED(CoCreateInstance(kRadioManagementClsid, nullptr, CLSCTX_LOCAL_SERVER, kIRadioManagerIid, reinterpret_cast<void**>(mgr.GetAddressOf()))) || !mgr)
        return std::nullopt;
    DWORD supported = 0;
    const HRESULT supHr = mgr->IsRMSupported(&supported);
    int enabled = 1;
    int param2 = 0;
    DWORD reason = 0;
    const HRESULT stateHr = SUCCEEDED(supHr) && supported != 0 ? mgr->GetSystemRadioState(&enabled, &param2, &reason) : E_FAIL;
    return ParseAirplaneRadioState(supHr, supported, stateHr, enabled);
}

bool SetAirplaneMode(bool on)
{
    ComPtr<IRadioManager> mgr;
    if (FAILED(CoCreateInstance(kRadioManagementClsid, nullptr, CLSCTX_LOCAL_SERVER, kIRadioManagerIid, reinterpret_cast<void**>(mgr.GetAddressOf()))) || !mgr)
        return false;
    DWORD supported = 0;
    if (FAILED(mgr->IsRMSupported(&supported)) || supported == 0)
        return false;
    return SUCCEEDED(mgr->SetSystemRadioState(on ? 0 : 1));
}

bool ReadAirplaneModeFast()
{
    const auto value = ReadRegDword(HKEY_LOCAL_MACHINE, kRadioStateKey, nullptr);
    return value.value_or(0) != 0;
}

} // namespace atrium
