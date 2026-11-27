#include "shell/ime_status.h"

#include <iostream>

using namespace atrium;

namespace {
bool g_failed = false;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        g_failed = true;
    }
}

ImeStatus Status(DWORD layout, bool ime, bool open, bool native, bool upper = false)
{
    ImeStatus status;
    status.layout = layout;
    status.ime = ime;
    status.open = open;
    status.native = native;
    status.upper = upper;
    return status;
}

void CheckGlyphs()
{
    Require(ImeGlyph(Status(0x08040804, true, true, true)) == L"中", "Chinese composing shows 中");
    Require(ImeGlyph(Status(0x04110411, true, true, true)) == L"あ", "Japanese composing shows あ");
    Require(ImeGlyph(Status(0x04120412, true, true, true)) == L"가", "Korean composing shows 가");
    Require(ImeGlyph(Status(0x04090409, false, false, false)) == L"a", "latin direct input shows a");
    Require(ImeGlyph(Status(0x04090409, false, false, false, true)) == L"A", "a Shift capital shows A");
    ImeStatus caps = Status(0x08040804, true, true, true);
    caps.caps = caps.upper = true;
    Require(ImeGlyph(caps) == L"A", "CapsLock over a native method types capital Latin");
    caps.upper = false;
    Require(ImeGlyph(caps) == L"中", "Shift under CapsLock gives the script back");
}

void CheckNews()
{
    const ImeStatus zh = Status(0x08040804, true, true, true);
    const ImeStatus en = Status(0x08040804, true, true, false);
    Require(!zh.SameAs(en), "a conversion state change is news");
    ImeStatus caps = zh;
    caps.caps = caps.upper = true;
    Require(!zh.SameAs(caps), "CapsLock's latch is news");
    ImeStatus shift = en;
    shift.upper = true;
    Require(en.SameAs(shift), "a Shift held alone is not news");
    Require(!Status(0x04090409, false, false, false).SameAs(Status(0x04070407, false, false, false)), "a language switch is news");
}

void CheckWatch()
{
    ImeWatch watch;
    const HWND a = reinterpret_cast<HWND>(1), b = reinterpret_cast<HWND>(2);
    const ImeStatus zh = Status(0x08040804, true, true, true);
    const ImeStatus en = Status(0x08040804, true, true, false);
    Require(!watch.Update(a, zh), "the first read only seeds the baseline");
    Require(!watch.Update(a, zh), "the same state never announces twice");
    Require(watch.Update(a, en), "a switch announces");
    Require(!watch.State().native, "the announced state is the newest one");
    Require(watch.Update(a, zh), "switching back announces too");
    Require(!watch.Update(b, en), "another window brings its own state without news");
    Require(watch.State().layout == zh.layout && !watch.State().native, "that window's state reseeded the baseline");
    ImeStatus stalled = en;
    stalled.reliable = false;
    Require(!watch.Update(b, stalled), "a stalled read announces nothing");
}

void CheckCarryForward()
{
    ImeStatus last = Status(0x08040804, true, true, true);
    ImeStatus stalled = Status(0x08040804, true, false, false);
    stalled.reliable = false;
    ImeCarryForward(stalled, last);
    Require(stalled.open && stalled.native, "a stalled read of the same layout carries the conversion state forward");
    ImeStatus other = Status(0x04090409, false, false, false);
    other.reliable = false;
    ImeCarryForward(other, last);
    Require(!other.open && !other.native, "a different layout is not carried into");
}

void CheckWakeKeys()
{
    Require(ImeShouldWakeOnKey(VK_SHIFT, true, false, false), "Shift down wakes");
    Require(ImeShouldWakeOnKey(VK_SHIFT, false, false, false), "Shift up wakes for Chinese IME toggle");
    Require(ImeShouldWakeOnKey(VK_LSHIFT, false, false, false), "Left Shift up wakes");
    Require(ImeShouldWakeOnKey(VK_RSHIFT, false, false, false), "Right Shift up wakes");
    Require(!ImeShouldWakeOnKey(VK_CONTROL, true, false, false), "Ctrl down alone does not wake");
    Require(ImeShouldWakeOnKey(VK_CONTROL, false, false, false), "Ctrl up wakes for IMEs that toggle on release");
    Require(ImeShouldWakeOnKey(VK_CAPITAL, true, false, false), "CapsLock down wakes");
    Require(!ImeShouldWakeOnKey(VK_CAPITAL, false, false, false), "CapsLock up does not wake");
    Require(ImeShouldWakeOnKey(VK_KANA, true, false, false), "Kana/Hangul key down wakes");
    Require(ImeShouldWakeOnKey(VK_SPACE, true, true, false), "Win/Ctrl+Space down wakes");
    Require(ImeShouldWakeOnKey(VK_SPACE, true, false, true), "replayed Win+Space down wakes");
    Require(!ImeShouldWakeOnKey(VK_SPACE, true, false, false), "plain Space does not wake");
    Require(!ImeShouldWakeOnKey('A', true, false, false) && !ImeShouldWakeOnKey('A', false, false, false), "typing keys never wake");
}

} // namespace

int main()
{
    CheckGlyphs();
    CheckNews();
    CheckWatch();
    CheckCarryForward();
    CheckWakeKeys();
    if (g_failed)
        return 1;
    std::cout << "ime status checks passed\n";
    return 0;
}
