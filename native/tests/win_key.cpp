#include "shell/win_key.h"

#include "shell/process_integrity.h"

#include <iostream>

using namespace atrium;

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;
    WinKeyState s;
    auto key = [&](DWORD vk, bool down, bool winDown, bool elevated = false, bool modified = false, bool injected = false, bool alone = true,
                   bool numbers = true) { return StepWinKey(s, vk, down, winDown, elevated, modified, injected, alone, numbers); };
    auto passed = [](WinKeyAction action) { return !action.swallow && !action.notify && !action.maskRelease; };

    ok &= Check(passed(key(VK_LWIN, true, false)) && s.key == VK_LWIN, "physical Win down passes");
    ok &= Check(passed(key(VK_LWIN, true, true)), "physical Win repeat passes");
    auto a = key(VK_LWIN, false, true);
    ok &= Check(a.notify && a.maskRelease && !s.key, "solo Win opens Atrium after a masked release");

    for (const DWORD vk : {DWORD('V'), DWORD('E'), DWORD('L')}) {
        s = {};
        ok &= Check(passed(key(VK_RWIN, true, false)), "right Win down passes");
        ok &= Check(passed(key(vk, true, true)) && passed(key(vk, false, true)), "physical combination reaches downstream hooks");
        ok &= Check(passed(key(VK_RWIN, false, true)), "combination release passes without opening Atrium");
    }

    s = {};
    key(VK_LWIN, true, false);
    ok &= Check(passed(key(0xE8, true, true, false, false, true)), "a clipboard hook can consume V and leave its mask key");
    ok &= Check(passed(key(VK_LWIN, false, true)), "clipboard hook mask cancels the solo Win action");

    s = {};
    key(VK_LWIN, true, false);
    a = key('1', true, true);
    ok &= Check(a.swallow && a.notify && a.notifyParam == 1, "Win+1 invokes the pinned app once");
    a = key('1', true, true);
    ok &= Check(a.swallow && !a.notify, "digit repeats stay suppressed");
    a = key(VK_LWIN, false, true);
    ok &= Check(a.maskRelease && !a.notify, "Win+digit masks the system menu without opening Atrium Start");
    ok &= Check(key('1', false, false).swallow, "digit release stays suppressed when Win is released first");

    s = {};
    key(VK_LWIN, true, false);
    ok &= Check(passed(key('0', true, true, false, true)), "modified Win+digit passes");
    ok &= Check(passed(key(VK_LWIN, false, true)), "modified chord release passes");
    s = {};
    key(VK_LWIN, true, false, true);
    ok &= Check(passed(key('1', true, true)) && passed(key(VK_LWIN, false, true)), "higher integrity window receives the native chord");
    s = {};
    key(VK_LWIN, true, false, false, false, false, false);
    ok &= Check(passed(key(VK_LWIN, false, true, false, false, false, false)), "digit-only mode forwards solo Win");
    s = {};
    key(VK_LWIN, true, false);
    key('L', true, true);
    key(VK_LWIN, true, false);
    a = key(VK_LWIN, false, true);
    ok &= Check(a.notify && a.maskRelease, "lost Win+L release does not suppress the next solo tap");
    s = {};
    key(VK_LWIN, true, false);
    key(VK_RWIN, true, true);
    ok &= Check(passed(key(VK_LWIN, false, true)) && passed(key(VK_RWIN, false, false)), "overlapping Windows keys remain native");
    ok &=
        Check(!IsHigherIntegrityRid(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_HIGH_RID), "same high integrity is not higher (UAC off / elevated Atrium)");
    ok &= Check(IsHigherIntegrityRid(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID), "high integrity is higher than medium");
    ok &= Check(IsHigherIntegrityRid(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_SYSTEM_RID), "system integrity is higher than high");
    ok &= Check(!IsHigherIntegrityRid(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID), "medium integrity is not higher than high");
    if (!ok)
        return 1;
    std::cout << "win key checks passed\n";
    return 0;
}
