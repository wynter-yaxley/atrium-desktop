// The bar's own entries: where the right-hand group goes, what a click on one opens, their tooltips.
#include "bar.h"
#include "bar_entries.h"

#include "localization.h"

#include <algorithm>
#include <vector>

namespace atrium {

std::wstring FormatNetworkTooltip(const SystemStatus& st)
{
    auto wifiLine = [](const std::wstring& n, int s) { return s >= 0 ? TrFormat(L"Wi-Fi %s · signal %d%%", n.c_str(), s) : TrFormat(L"Wi-Fi %s", n.c_str()); };
    if (st.links.size() < 2)
        return !st.connected ? (st.airplane ? Tr(L"Airplane mode") : Tr(L"No network")) : st.wifi ? wifiLine(st.network, st.signal) : st.network;
    std::wstring out;
    size_t n = 0;
    auto add = [&](bool primary) {
        for (const auto& l : st.links)
            if (l.primary == primary && n < 3) {
                const std::wstring& name = l.network.empty() ? l.alias : l.network;
                out += (n++ ? L"\n" : L"") + (l.kind == NetworkLink::Kind::Wifi ? wifiLine(name, l.signal) : name);
            }
    };
    add(true);
    add(false);
    return st.links.size() > 3 ? out + L'\n' + TrFormat(L"+%d more", int(st.links.size() - 3)) : out;
}

RECT Bar::EntryRect(const std::wstring& key) const
{
    const EdgeFrame frame{edge_, window_};
    auto rect = [&](const D2D1_RECT_F& r) { return frame.ToArea(RECT{LONG(r.left), LONG(r.top), LONG(r.right), LONG(r.bottom)}); };
    for (const auto& it : scene_.extras)
        if (it.key == key)
            return rect(it.plate);
    for (const auto& it : scene_.items)
        if (it.key == key)
            return rect(it.plate);
    if (key == kClockKey)
        return rect(scene_.clock);
    // Toasts still have an anchor when their taskbar entry is hidden.
    const LONG h = LONG(scene_.bar.bottom - scene_.bar.top);
    return frame.ToArea(RECT{LONG(scene_.bar.right) - h, LONG(scene_.bar.top), LONG(scene_.bar.right), LONG(scene_.bar.bottom)});
}

RECT Bar::BarRect() const
{
    return EdgeFrame{edge_, window_}.ToArea(RECT{LONG(scene_.bar.left), LONG(scene_.bar.top), LONG(scene_.bar.right), LONG(scene_.bar.bottom)});
}

// A click on one of the bar's own entries: what it opens, above its rectangle.
void Bar::ClickEntry(const std::wstring& key)
{
    const RECT anchor = EntryRect(key);
    if (key == kMoreKey && onOverflow)
        onOverflow(hidden_, anchor);
    for (auto [k, f] : {std::pair{kStartKey, &onStartClick}, {kNotifyKey, &onNotifyClick}, {kTrayKey, &onTrayClick}, {kStatusKey, &onStatusClick}})
        if (key == k && *f)
            (*f)(anchor);
    for (auto [k, f] : {std::pair{kClockKey, &onClockClick}, {kDesktopKey, &onDesktopClick}, {kImeKey, &onImeClick}})
        if (key == k && *f)
            (*f)();
    if ((key == kLaunchpadKey || key == kMusicKey || key == kStacksKey) && onExtraClick)
        onExtraClick(key, anchor);
}

void Bar::SetMusic(std::shared_ptr<const MusicFeed> feed, bool shown, std::wstring tip)
{
    musicTip_ = std::move(tip);
    const bool relayout = shown != musicShown_;
    musicShown_ = shown;
    {
        std::lock_guard g(lock_);
        music_ = std::move(feed);
    }
    if (relayout) {
        Relayout(); // the entry comes or goes
    } else {
        Publish(false); // the render thread takes up the bars (playing) or lets them settle
        UpdateTooltips();
    }
}

void Bar::WakeRender()
{
    if (wake_)
        SetEvent(wake_);
}

std::wstring Bar::TooltipText(const std::wstring& key) const
{
    if (key == kStartKey)
        return Tr(L"Start");
    if (key == kLaunchpadKey)
        return Tr(L"Launchpad");
    if (key == kMusicKey)
        return musicTip_.empty() ? std::wstring(Tr(L"Now Playing")) : musicTip_;
    if (key == kStacksKey)
        return Tr(L"File Stacks");
    if (key == kNotifyKey) {
        if (dnd_)
            return unread_ ? TrFormat(L"%d unread notifications · Do Not Disturb", int(unread_)) : Tr(L"Notifications · Do Not Disturb");
        return unread_ ? TrFormat(L"%d unread notifications", int(unread_)) : Tr(L"Notifications");
    }
    if (key == kTrayKey)
        return Tr(L"Tray");
    if (key == kImeKey)
        return Tr(L"Input method");
    if (key == kDesktopKey)
        return Tr(L"Show Desktop");
    if (key == kStatusKey) {
        const SystemStatus& st = status_;
        std::wstring net = FormatNetworkTooltip(st);
        std::wstring more;
        if (st.battery)
            more = st.charging ? TrFormat(L"\nBattery %d%% · plugged in", st.batteryPercent) : TrFormat(L"\nBattery %d%%", st.batteryPercent);
        if (st.audio)
            more += st.muted ? Tr(L"\nMuted") : TrFormat(L"\nVolume %d%%", st.volume);
        return net + more;
    }
    if (key == kMoreKey)
        return TrFormat(L"%d more apps", int(hidden_.size()));
    if (key == kClockKey) {
        wchar_t buf[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, buf, 128, nullptr);
        return buf;
    }
    for (const auto& t : items_)
        if (t.key == key)
            return t.windows.size() > 1 ? TrFormat(L"%s · %d windows", t.name.c_str(), int(t.windows.size())) : t.name;
    return {};
}
// The right-hand group on the scene: each entry a plate (the status entry and Show Desktop as wide as
// they are), its hit cell splitting the gaps; the last one reaches to the bar's very end.
void Bar::PlaceRight(BarScene& scene, const std::array<RightEntry, 8>& right, const EntryMetrics& m, float timePx, float datePx)
{
    const RightEntry* last = nullptr;
    for (const auto& e : right)
        last = e.shown && !last ? &e : last;
    for (const auto& e : right) {
        if (!e.shown)
            continue;
        hits_.push_back({e.key, RECT{e.left - m.gap / 2, m.hitTop, &e == last ? m.barEnd : e.left + e.width + m.gap / 2, m.hitBottom}});
        const D2D1_RECT_F plate = D2D1::RectF(float(e.left), m.plateTop, float(e.left + e.width), m.plateTop + float(m.plate));
        if (e.key == kClockKey) {
            scene.clock = plate;
            scene.timePx = timePx;
            scene.datePx = datePx;
            continue;
        }
        SceneItem it;
        it.key = e.key;
        it.glyph = e.glyph;
        it.plate = plate;
        // The icon's square centred on the plate; a wider entry draws across its slot.
        const bool square = e.width == m.plate;
        const float il = square ? float(e.left) + float(m.plate - m.icon) / 2 : float(e.left), iw = square ? float(m.icon) : float(e.width);
        it.icon = D2D1::RectF(il, m.iconTop, il + iw, m.iconTop + float(m.icon));
        it.badge = e.key == kNotifyKey ? unread_ : 0;
        scene.extras.push_back(std::move(it));
    }
}

} // namespace atrium
