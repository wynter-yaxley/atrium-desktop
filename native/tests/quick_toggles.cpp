#include "shell/quick_toggles.h"

#include <cstdint>
#include <iostream>
#include <optional>
#include <vector>

namespace atrium {

struct EnergySaverInput {
    bool on = false;
    bool onAc = false;
    uint32_t curAc = 0;
    uint32_t curDc = 0;
    uint32_t defaultAc = 0;
    uint32_t defaultDc = 20;
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

EnergySaverPlan PlanEnergySaver(const EnergySaverInput& in);

} // namespace atrium

using namespace atrium;

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool CheckNightLightEncoding()
{
    // Captured from this PC (Windows 11 build 26300) in OFF and ON states.
    const std::vector<uint8_t> capturedOff = {
        0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0x9E, 0xD3, 0xE3, 0xD4,
        0x06, 0x2A, 0x2B, 0x0E, 0x05, 0x43, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::vector<uint8_t> capturedOn = {
        0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0xC3, 0xFE, 0x8E, 0xD8, 0x06,
        0x2A, 0x2B, 0x0E, 0x07, 0x43, 0x42, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    bool ok = true;
    ok &= Check(IsNightLightOn(capturedOff) == false, "captured OFF record parses as off");
    ok &= Check(IsNightLightOn(capturedOn) == true, "captured ON record parses as on");
    ok &= Check(!IsNightLightOn({}).has_value(), "empty record returns nullopt");

    const auto toOn = ToggleNightLightData(capturedOff, true, 1795407683ULL);
    ok &= Check(toOn == capturedOn, "OFF -> ON produces exact captured ON record");

    const auto toOff = ToggleNightLightData(capturedOn, false, 1795407685ULL);
    ok &= Check(IsNightLightOn(toOff) == false, "ON -> OFF parses as off");
    ok &= Check(toOff.size() == capturedOff.size(), "ON -> OFF restores 27-byte length");
    ok &= Check(toOff[18] == 0x05, "ON -> OFF decrements length byte back to 0x05");

    const auto bumped = ToggleNightLightData(capturedOff, true, 1);
    ok &= Check(IsNightLightOn(bumped) == true, "stale timestamp toggle still turns on");
    ok &= Check(bumped[10] != capturedOff[10], "stale timestamp is bumped");
    return ok;
}

bool CheckEnergySaverPlanning()
{
    bool ok = true;

    // Plugged in (AC): toggles AC threshold only, leaving custom DC threshold untouched.
    const auto acOn = PlanEnergySaver({.on = true, .onAc = true, .curAc = 0, .curDc = 30});
    ok &= Check(acOn.writeAc == 100u && !acOn.writeDc.has_value(), "AC enable writes AC=100 without touching DC");
    ok &= Check(acOn.saveAc == 0u && !acOn.saveDc.has_value() && acOn.NeedsApply(), "AC enable records prior AC=0");

    // Restoring after restart when saved AC threshold is reloaded.
    const auto acOff = PlanEnergySaver({.on = false, .onAc = true, .curAc = 100, .curDc = 30, .savedAc = 0u});
    ok &= Check(acOff.writeAc == 0u && !acOff.writeDc.has_value(), "AC disable restores saved AC=0 and leaves DC=30");
    ok &= Check(acOff.clearAc && !acOff.clearDc && acOff.NeedsApply(), "AC disable clears saved AC entry");

    // On battery (DC): preserves custom threshold 30 across toggle/restart.
    const auto dcOn = PlanEnergySaver({.on = true, .onAc = false, .curAc = 0, .curDc = 30});
    ok &= Check(dcOn.writeDc == 100u && !dcOn.writeAc.has_value(), "DC enable writes DC=100 without touching AC");
    ok &= Check(dcOn.saveDc == 30u && !dcOn.saveAc.has_value(), "DC enable saves custom DC=30");

    // Turned on while on DC, then plugged into AC before turning off: restores DC=30 cleanly.
    const auto dcToAcOff = PlanEnergySaver({.on = false, .onAc = true, .curAc = 0, .curDc = 100, .savedDc = 30u});
    ok &= Check(dcToAcOff.writeDc == 30u && !dcToAcOff.writeAc.has_value(), "disable after DC->AC switch restores DC=30");
    ok &= Check(dcToAcOff.clearDc && !dcToAcOff.clearAc, "disable after DC->AC switch clears saved DC entry");

    // Already off with custom DC threshold and no saved state: no-op (does not clobber 30 with 20).
    const auto noopOff = PlanEnergySaver({.on = false, .onAc = false, .curAc = 0, .curDc = 30});
    ok &= Check(!noopOff.NeedsApply() && !noopOff.writeDc.has_value() && !noopOff.writeAc.has_value(),
                "disable when already off does not mutate custom threshold");

    // Externally enabled on AC without saved entry: falls back to scheme AC default (0), not DC default (20).
    const auto fallbackAcOff = PlanEnergySaver({.on = false, .onAc = true, .curAc = 100, .curDc = 30, .defaultAc = 0, .defaultDc = 30});
    ok &= Check(fallbackAcOff.writeAc == 0u && !fallbackAcOff.writeDc.has_value(), "AC disable without saved entry restores AC default 0");

    // Repeated enable while already at 100 with saved state: preserves original saved threshold and avoids redundant apply.
    const auto repeatOn = PlanEnergySaver({.on = true, .onAc = false, .curAc = 0, .curDc = 100, .savedDc = 30u});
    ok &= Check(!repeatOn.saveDc.has_value() && !repeatOn.NeedsApply(), "repeated DC enable does not overwrite saved=30");
    return ok;
}

bool CheckStatusToggles()
{
    bool ok = true;
    ok &= Check(ParseAirplaneRadioState(S_OK, 1, S_OK, 0) == true, "systemRadiosEnabled=0 parses as airplane mode ON");
    ok &= Check(ParseAirplaneRadioState(S_OK, 1, S_OK, 1) == false, "systemRadiosEnabled=1 parses as airplane mode OFF");
    ok &= Check(!ParseAirplaneRadioState(S_OK, 0, S_OK, 0).has_value(), "unsupported radio manager returns nullopt");
    ok &= Check(!ParseAirplaneRadioState(E_FAIL, 1, S_OK, 0).has_value(), "failed IsRMSupported returns nullopt");
    ok &= Check(!ParseAirplaneRadioState(S_OK, 1, E_FAIL, 0).has_value(), "failed GetSystemRadioState returns nullopt");

    ok &= Check(!IsQuietProfileActive(L""), "empty quiet profile is not active");
    ok &= Check(!IsQuietProfileActive(L"Microsoft.QuietHoursProfile.Unrestricted"), "Unrestricted profile is not active");
    ok &= Check(IsQuietProfileActive(L"Microsoft.QuietHoursProfile.AlarmsOnly"), "AlarmsOnly profile is active");
    ok &= Check(IsQuietProfileActive(L"Microsoft.QuietHoursProfile.PriorityOnly"), "PriorityOnly profile is active");
    return ok;
}

} // namespace

// Only the record's encoding and energy-saver planning: what this PC's switches are set to is not the tests' business.
int main()
{
    const bool ok = CheckNightLightEncoding() && CheckEnergySaverPlanning() && CheckStatusToggles();
    if (!ok)
        return 1;
    std::cout << "quick_toggles tests passed\n";
    return 0;
}
