// Bar layout: monitor/DPI/edge resolution, geometry and overflow fitting,
// scene/hit-target building with icon caching, and window/AppBar placement.
#include "bar.h"
#include "bar_entries.h"

#include "clock.h"
#include "render/ui_font.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace atrium {

namespace {

std::vector<int> BuildDragOrder(int n, bool& dragging, std::wstring& dragKey, int& dragInsert, const std::vector<TaskItem>& items, int& dragIndex)
{
    std::vector<int> order(size_t(std::max(n, 0)));
    for (int i = 0; i < n; ++i)
        order[size_t(i)] = i;
    dragIndex = -1;
    if (!dragging)
        return order;
    for (int i = 0; i < n; ++i)
        if (items[size_t(i)].key == dragKey)
            dragIndex = i;
    if (dragIndex < 0) {
        dragging = false; // the app went away mid-drag
        dragKey.clear();
        return order;
    }
    dragInsert = std::clamp(dragInsert, 0, n - 1);
    order.erase(order.begin() + dragIndex);
    order.insert(order.begin() + dragInsert, dragIndex);
    return order;
}

// Widest rendering of a clock string: every digit replaced by the widest digit.
std::wstring WidestDigits(std::wstring s)
{
    for (auto& c : s)
        if (iswdigit(c))
            c = L'8';
    return s;
}

int StretchForMagnify(float mag, int plate, int gap) { return mag > 1.001f ? int(std::ceil((mag - 1) * float(plate + gap) * 2.3f)) : 0; }

int MeasureClockBlockWidth(IDWriteFactory3* dwrite, BarEdge edge, float scale, float timePx, float datePx, bool showSeconds,
                           void (*clockStrings)(std::wstring&, std::wstring&, bool))
{
    float span = timePx * 1.22f + datePx * 1.25f;
    if (!Vertical(edge)) {
        std::wstring time, date;
        clockStrings(time, date, showSeconds);
        span = std::max(MeasureUiText(dwrite, WidestDigits(time), timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                        MeasureUiText(dwrite, WidestDigits(date), datePx, DWRITE_FONT_WEIGHT_NORMAL));
    }
    return int(std::ceil(span)) + int(std::lround(22.0 * scale));
}

} // namespace

struct Bar::RelayoutTarget {
    HMONITOR mon = nullptr;
    MONITORINFO mi{sizeof(mi)};
    float scale = 1.f;
    BarEdge edge = BarEdge::Bottom;
    bool primary = false;
    bool edgeChanged = false;
    bool monitorChanged = false;
};

struct Bar::LayoutMetrics {
    bool showClock = false;
    bool showTray = false;
    bool showIme = false;
    bool showStatus = false;
    bool showNotifications = false;
    bool showDesktop = false;
    bool showMusic = false;
    bool showStacks = false;
    int rightCount = 0;
    int groupGap = 0;
    int sepGap = 0;

    int inset = 0;
    int barH = 0;
    int icon = 0;
    int plate = 0;
    int gap = 0;
    int pad = 0;
    int v = 0;
    int pitch = 0;
    int radius = 0;
    int statusW = 0;
    int deskW = 0;
    int clockW = 0;
    float timePx = 0;
    float datePx = 0;
    float mag = 1.f;
    float range = 0;

    int shownApps = 0;
    bool overflow = false;

    int barW = 0;
    int insetX = 0;
    int insetTop = 0;
    int rise = 0;
    int winW = 0;
    int winH = 0;
    int reserve = 0;
    RECT win{};
};

HMONITOR Bar::ResolveRelayoutMonitor(MONITORINFO& mi)
{
    const std::wstring& id = !displayOverride_.empty()              ? displayOverride_ :
                             settings_.dockDisplayMode == L"fixed"  ? settings_.dockDisplay :
                             settings_.dockDisplayMode == L"follow" ? activeDisplay_ :
                                                                      std::wstring();
    auto valid = [&](HMONITOR m) {
        mi = {sizeof(mi)};
        return m && GetMonitorInfoW(m, &mi) && !IsRectEmpty(&mi.rcMonitor);
    };
    if (HMONITOR mon = ResolveDisplay(displays_, id); valid(mon))
        return mon;
    ReadDisplays();
    if (HMONITOR mon = ResolveDisplay(displays_, id); valid(mon))
        return mon;
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return valid(mon) ? mon : nullptr;
}

bool Bar::ResolveLayoutTarget(RelayoutTarget& out)
{
    out.mi = {sizeof(out.mi)};
    out.mon = ResolveRelayoutMonitor(out.mi);
    if (!out.mon)
        return false;
    out.primary = (out.mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    filterTasks_ = FilterTasksOnDisplay(settings_.taskbarApps, out.primary);
    items_ = TaskItemsForDisplay(allItems_, settings_.taskbarApps, out.primary, out.mon, GetAncestor(GetForegroundWindow(), GA_ROOTOWNER),
                                 [](HWND window) { return MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST); });
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(out.mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    out.scale = float(dpiX) / 96.f;
    out.edge = ParseBarEdge(settings_.barEdge);
    out.edgeChanged = hmonitor_ && out.edge != edge_;
    out.monitorChanged = hmonitor_ && (hmonitor_ != out.mon || !EqualRect(&monitor_, &out.mi.rcMonitor) || scale_ != out.scale || out.edgeChanged);
    if (out.edgeChanged) {
        if (dragging_) {
            dragging_ = false;
            dragKey_.clear();
        }
        HideTooltip();
        near_.Reset();
        nearX_ = -1;
        nearRise_ = nearMagnify_ = false;
        scene_.hoverKey.clear();
        scene_.pressedKey.clear();
        scene_.pointerX = scene_.pointerY = -1;
        scene_.rippleMs = 0;
        pressed_.clear();
        pressedLeft_ = tracking_ = regionExtended_ = false;
    }
    edge_ = out.edge;
    return true;
}

void Bar::FitAppStrip(LayoutMetrics& m, float scale, int screenW, int maxW, int rightPlates, int statusExtra)
{
    const Settings& st = settings_;
    const int total = int(items_.size());
    int n = total;
    bool more = false;
    auto padFor = [&](int p) { return (m.barH - p) / 2 + Px(4, scale); }; // ends: plate gap plus a little air
    auto contentWidth = [&](int count, int p, int g, bool overflow) {
        int slots = count + (overflow ? 1 : 0);
        int w = 2 * padFor(p) + p + 2 * g + (st.showLaunchpad ? p + g : 0); // logo (and Launchpad) and the air after it
        w += slots ? slots * p + (slots - 1) * g : 0;                      // apps
        if (m.rightCount)
            w += 2 * m.sepGap + p * rightPlates + (m.showStatus ? statusExtra : 0) + m.clockW + m.deskW + (m.rightCount - 1) * g;
        return std::max(w, m.barH * 2);
    };
    // Too many apps: tighten the gaps, then the plates; what still does not fit
    // goes behind the overflow entry (never silently dropped).
    while (n > 0 && contentWidth(n, m.plate, m.gap, false) > maxW && m.gap > Px(2, scale))
        --m.gap;
    while (n > 0 && contentWidth(n, m.plate, m.gap, false) > maxW && m.plate > m.icon + Px(4, scale))
        --m.plate;
    if (contentWidth(n, m.plate, m.gap, false) > maxW) {
        more = true;
        while (n > 0 && contentWidth(n, m.plate, m.gap, true) > maxW)
            --n;
    }
    hidden_.clear();
    for (int i = n; i < total; ++i)
        hidden_.push_back(items_[size_t(i)].key);

    m.shownApps = n;
    m.overflow = more;
    // Never narrower than what is always there (logo, overflow, tray, status, clock, bell).
    m.barW = std::max(st.autoWidth ? std::min(contentWidth(n, m.plate, m.gap, more), maxW) : maxW, std::min(contentWidth(0, m.plate, m.gap, more), screenW));
    m.pad = padFor(m.plate);
}

Bar::LayoutMetrics Bar::ComputeLayoutMetrics(const RelayoutTarget& target)
{
    const Settings& st = settings_;
    const float s = target.scale;
    LayoutMetrics m;
    // Resolve per-display visibility without changing the user's individual switches.
    const bool showRight = !st.taskbarRightPrimaryOnly || target.primary;
    m.showClock = showRight && st.showClock;
    m.showTray = showRight && st.showTray;
    m.showIme = showRight && st.imeStatus;
    m.showStatus = showRight && st.showStatus;
    m.showNotifications = showRight && st.showNotifications;
    m.showDesktop = showRight && st.showDesktop;
    m.showMusic = showRight && st.showNowPlaying && musicShown_;
    m.showStacks = showRight && st.showFolderStacks;

    // Metrics (px). Every icon sits centred on a square plate (press and drag
    // feedback); the plate has the same gap `v` to the top and bottom edge,
    // and its corner radius is the bar's minus that gap, so plate and bar
    // corners are concentric.
    m.inset = Px(kInsetDip, s);
    m.barH = Px(st.height, s);
    m.icon = std::clamp(Px(st.iconSize, s), Px(12, s), m.barH - Px(16, s));
    m.plate = std::max(m.icon + Px(4, s), std::min(m.barH - Px(12, s), int(std::lround(m.icon * 1.55))));
    m.gap = std::max(Px(4, s), int(std::lround(m.icon * 0.18)));
    m.sepGap = Px(8, s);
    m.mag = float(std::clamp(st.magnify, 1.0, 2.0));

    // Clock block sized for its widest possible text, or its two stacked lines on a side edge.
    m.timePx = float(m.barH) * 0.25f;
    m.datePx = float(m.barH) * 0.19f;
    if (m.showClock)
        m.clockW = MeasureClockBlockWidth(dwrite_.get(), target.edge, s, m.timePx, m.datePx, st.showClockSeconds, ClockStrings);

    // Left to right: [logo] [apps ... (overflow)] | [tray] [input method] [status] [clock] [bell].
    // The status entry as three icons (statusStyle) is wider than a plate by about an icon.
    const int statusExtra = st.statusStyle == L"icons" ? int(std::lround(m.icon * 1.3f)) : 0;
    // Show Desktop: a narrow slot at the bar's right end, a thin line in it, as the system taskbar's.
    m.deskW = m.showDesktop ? std::max(Px(10, s), int(std::lround(m.icon * 0.45))) : 0;
    const int rightPlates = int(m.showStacks) + int(m.showTray) + int(m.showIme) + int(m.showStatus) + int(m.showMusic) + int(m.showNotifications);
    m.rightCount = rightPlates + int(m.clockW != 0) + int(m.showDesktop);
    m.groupGap = m.rightCount ? m.sepGap : 0;

    const EdgeFrame wa{target.edge, target.mi.rcWork};
    const int screenW = int(wa.Along()) - 2 * m.inset;
    const int maxW = std::max(m.barH * 2, std::min(Px(st.width, s), screenW - StretchForMagnify(m.mag, m.plate, m.gap)));
    FitAppStrip(m, s, screenW, maxW, rightPlates, statusExtra);
    m.statusW = m.plate + statusExtra;
    m.v = (m.barH - m.plate) / 2;
    m.pitch = m.plate + m.gap;
    m.radius = std::min(Px(st.cornerRadius, s), std::min(m.barW, m.barH) / 2);

    // Room in the windows around the shape: the shadow everywhere; at the
    // sides half of the magnified stretch; above, the raised glass and magnified or hopping icons.
    m.range = float(m.pitch) * 2.3f;
    const int stretch = StretchForMagnify(m.mag, m.plate, m.gap);
    // The window keeps the width of the widest bar the settings allow, and the bar sits centred in it.
    const int roomW = std::max(m.barW, maxW);
    const int side = m.inset + (stretch + 1) / 2 + (stretch ? Px(2, s) : 0);
    m.insetX = side + (roomW - m.barW) / 2;
    const int lift = std::max(int(std::ceil(float(m.icon) * (m.mag - 1))), int(std::ceil(float(m.icon) * 0.55f)));
    const int glassRise = st.hoverRise ? int(std::ceil(float(m.icon) * (m.mag - 1))) : 0;
    const float badgeH = std::max(13.f * s, float(m.icon) * 0.44f);
    const int badgeHeadroom = int(std::ceil((badgeH * 0.30f + 1.5f * s) * m.mag));
    m.rise = std::max(glassRise, lift - (m.barH - m.icon) / 2 + std::max(Px(6, s), badgeHeadroom));
    // Keep shadow room above the raised glass, including at maximum magnification.
    m.insetTop = std::max(m.inset + glassRise, m.rise);

    m.winW = roomW + 2 * side;
    m.winH = m.barH + m.insetTop + m.inset;
    const int margin = Px(st.margin, s);
    // "Always shown": the bar keeps its strip of the screen as an app bar.
    m.reserve = visible_ && st.bar && st.autoHide == L"never" && st.barReserve ? margin + m.barH : 0;
    const int ownReserve = appBar_ && appBarMonitor_ == target.mon && appBarEdge_ == target.edge ? appBarReserved_ : 0;
    m.win = PlaceBarWindow({target.edge, target.mi.rcMonitor, target.mi.rcWork, m.winW, m.winH, m.barH, margin, m.insetTop, st.hideTaskbar, ownReserve});
    return m;
}

BarScene Bar::InitLayoutScene(const RelayoutTarget& target, const LayoutMetrics& m) const
{
    const D2D1_POINT_2F shift = hmonitor_ && !target.edgeChanged ?
                                    EdgeFrame{target.edge, m.win}.FromArea(EdgeFrame{target.edge, window_}.ToArea(D2D1_POINT_2F{0, 0})) :
                                    D2D1_POINT_2F{};
    BarScene scene;
    scene.edge = target.edge;
    scene.scale = target.scale;
    scene.bar = D2D1::RectF(float(m.insetX), float(m.insetTop), float(m.insetX + m.barW), float(m.insetTop + m.barH));
    scene.windowW = float(m.winW);
    scene.windowH = float(m.winH);
    scene.tucked = tucked_;
    scene.plateRadius = float(std::clamp(m.radius - m.v, Px(3, target.scale), m.plate / 2));
    scene.hoverKey = dragging_ ? std::wstring() : scene_.hoverKey;
    scene.pressedKey = scene_.pressedKey;
    scene.dragKey = dragging_ ? dragKey_ : std::wstring();
    scene.magnify = m.mag;
    scene.magnifyRange = m.range;
    scene.magnifyLift = settings_.hoverRise ? float(m.icon) * (m.mag - 1) : 0;
    scene.earlyRise = settings_.hoverRise && settings_.riseNear;
    scene.earlyMagnify = settings_.magnifyNear;
    // Keep the pointer and the ripple; only the geometry changes.
    scene.pointerX = scene_.pointerX < 0 ? -1 : scene_.pointerX + shift.x;
    scene.pointerY = scene_.pointerX < 0 ? -1 : scene_.pointerY + shift.y;
    scene.nearX = nearX_ < 0 ? -1 : nearX_ + shift.x;
    scene.nearRise = nearRise_;
    scene.nearMagnify = nearMagnify_;
    scene.alertLight = settings_.attentionLight;
    scene.alertBounce = settings_.attentionBounce;
    scene.rippleX = scene_.rippleX + shift.x;
    scene.rippleY = scene_.rippleY + shift.y;
    scene.rippleMs = scene_.rippleMs;
    return scene;
}

void Bar::ResolveTaskIcon(const TaskItem& t, int iconPx, std::map<std::wstring, CachedIcon>& keep, SceneItem& it)
{
    // Icons are cached per key until the model reports a new version.
    CachedIcon c;
    if (auto f = icons_.find(t.key); f != icons_.end() && f->second.version == t.iconVersion && f->second.px == iconPx && f->second.image)
        c = f->second;
    else if (iconProvider) {
        IconImage img = iconProvider(t.key, iconPx);
        if (!img.Empty())
            c = {t.iconVersion, iconPx, nextIconId_++, std::make_shared<const IconImage>(std::move(img))};
    }
    if (c.image) {
        it.image = c.image;
        it.imageId = c.id;
        keep[t.key] = c;
    }
}

void Bar::BuildLayoutScene(const RelayoutTarget& target, const LayoutMetrics& m)
{
    int dragIndex = -1;
    const std::vector<int> order = BuildDragOrder(m.shownApps, dragging_, dragKey_, dragInsert_, items_, dragIndex);
    // Launch feedback is one 480 ms hop.
    const double now = ClockMs();
    for (auto it = bounces_.begin(); it != bounces_.end();)
        it = now - it->second > 480 ? bounces_.erase(it) : std::next(it);

    BarScene scene = InitLayoutScene(target, m);
    hits_.clear();
    // Hit cells reach up to the window's top: magnified icons rise above the
    // shape, and the input region then covers that room too.
    const int hitTop = 0, hitBottom = m.insetTop + m.barH;
    const float plateTop = float(m.insetTop + m.v);
    const float iconTop = float(m.insetTop) + float(m.barH - m.icon) / 2;
    auto entry = [&](const wchar_t* key, int left, const wchar_t* glyph) {
        SceneItem it;
        it.key = key;
        it.plate = D2D1::RectF(float(left), plateTop, float(left + m.plate), plateTop + float(m.plate));
        float il = float(left) + float(m.plate - m.icon) / 2;
        it.icon = D2D1::RectF(il, iconTop, il + float(m.icon), iconTop + float(m.icon));
        it.glyph = glyph;
        return it;
    };
    const int startLeft = m.insetX + m.pad;
    int cursor = m.insetX + m.barW - m.pad;
    auto rightSlot = [&](int width, bool enabled) {
        if (!enabled)
            return cursor;
        cursor -= width;
        const int left = cursor;
        cursor -= m.gap;
        return left;
    };
    std::array<RightEntry, 8> right{{{kDesktopKey, m.deskW, L"", m.showDesktop},
                                     {kNotifyKey, m.plate, L"\uEA8F", m.showNotifications},
                                     {kClockKey, m.clockW, L"", m.clockW != 0},
                                     {kMusicKey, m.plate, L"", m.showMusic},
                                     {kStatusKey, m.statusW, L"", m.showStatus},
                                     {kImeKey, m.plate, L"", m.showIme},
                                     {kTrayKey, m.plate, L"\uE70E", m.showTray},
                                     {kStacksKey, m.plate, L"\uE8B7", m.showStacks}}}; // the settings' own folder symbol
    for (auto& e : right)
        e.left = rightSlot(e.width, e.shown);
    const int rightLeft = m.rightCount ? cursor + m.gap : m.insetX + m.barW - m.pad;
    const int launchLeft = startLeft + m.plate + m.gap; // Launchpad, one slot right of the logo
    const int appsMin = (settings_.showLaunchpad ? launchLeft : startLeft) + m.plate + 2 * m.gap;
    const int slots = m.shownApps + (m.overflow ? 1 : 0);
    const int appsW = slots ? slots * m.plate + (slots - 1) * m.gap : 0;
    // Apps are centred in the space between the logo and the separator (matters when autoWidth is off).
    const int appsLeft = appsMin + std::max(0, (rightLeft - 2 * m.groupGap - appsMin - appsW) / 2);
    scene.extras.push_back(entry(kStartKey, startLeft, L""));
    hits_.push_back({kStartKey, RECT{m.insetX, hitTop, (settings_.showLaunchpad ? launchLeft : appsMin) - m.gap / 2, hitBottom}});
    if (settings_.showLaunchpad) {
        scene.extras.push_back(entry(kLaunchpadKey, launchLeft, L""));
        hits_.push_back({kLaunchpadKey, RECT{launchLeft - m.gap / 2, hitTop, appsMin - m.gap / 2, hitBottom}});
    }
    // Icons are rendered for their magnified size, so they stay sharp when they grow.
    const int iconPx = int(std::lround(float(m.icon) * m.mag));
    std::map<std::wstring, CachedIcon> keep;
    for (int k = 0; k < m.shownApps; ++k) {
        const int i = order[size_t(k)];
        const TaskItem& t = items_[size_t(i)];
        SceneItem it;
        it.key = t.key;
        float left = float(appsLeft + k * m.pitch);
        if (i == dragIndex) // follows the pointer, kept within the app strip
            left = std::clamp(float(dragX_ - grabDx_) - float(m.plate) / 2, float(appsLeft - m.gap), float(appsLeft + m.shownApps * m.pitch - m.plate));
        it.plate = D2D1::RectF(left, plateTop, left + float(m.plate), plateTop + float(m.plate));
        float iconLeft = left + float(m.plate - m.icon) / 2;
        it.icon = D2D1::RectF(iconLeft, iconTop, iconLeft + float(m.icon), iconTop + float(m.icon));
        it.running = !t.windows.empty();
        it.active = t.active;
        it.attention = t.attention;
        it.badge = settings_.barBadges ? t.badge : 0;
        if (auto b = bounces_.find(t.key); b != bounces_.end())
            it.bounceMs = b->second;
        ResolveTaskIcon(t, iconPx, keep, it);
        scene.items.push_back(std::move(it));
        int hl = appsLeft + k * m.pitch - m.gap / 2;
        int hr = appsLeft + k * m.pitch + m.plate + (m.gap - m.gap / 2);
        hits_.push_back({t.key, RECT{hl, hitTop, hr, hitBottom}});
    }
    if (m.overflow) {
        int left = appsLeft + m.shownApps * m.pitch;
        scene.extras.push_back(entry(kMoreKey, left, L"\uE712"));
        hits_.push_back({kMoreKey, RECT{left - m.gap / 2, hitTop, left + m.plate + m.gap / 2, hitBottom}});
    }
    icons_ = std::move(keep);
    appsLeft_ = appsLeft;
    pitch_ = m.pitch;
    plate_ = m.plate;
    shown_ = m.shownApps;
    scene.separatorX = m.rightCount ? float(rightLeft - m.sepGap) : -1;
    scene.status = status_;
    scene.statusIcons = settings_.statusStyle == L"icons";
    scene.folderStackColor = settings_.folderStackIcon != L"mono";
    PlaceRight(scene, right, EntryMetrics{m.plate, m.gap, m.icon, hitTop, hitBottom, m.insetX + m.barW, plateTop, iconTop}, m.timePx, m.datePx);
    if (!scene.hoverKey.empty() && std::ranges::none_of(hits_, [&](const Hit& h) { return h.key == scene.hoverKey; }))
        scene.hoverKey.clear(); // the hovered app went away
    // The input method glyph is the foreground window's, not layout: it rides through a rebuild.
    scene.imeGlyph = scene_.imeGlyph;
    scene.imePrevGlyph = scene_.imePrevGlyph;
    scene.imeMs = scene_.imeMs;
    scene_ = std::move(scene);
}

void Bar::ApplyLayout(const RelayoutTarget& target, const LayoutMetrics& m)
{
    // The window only changes when the bar's size does (not on every drag move).
    const bool shown = visible_ && !slidOut_;
    if (!dragging_ || !EqualRect(&m.win, &applied_) || shown != appliedVisible_)
        PlaceWindows(m.win, shown);
    window_ = m.win;
    monitor_ = target.mi.rcMonitor;
    hmonitor_ = target.mon;
    UpdateAppBar(target.mon, target.mi.rcMonitor, m.reserve);
    scale_ = target.scale;
    insetX_ = m.insetX;
    insetTop_ = m.insetTop;
    rise_ = m.rise;
    radius_ = m.radius;
    SetRegion(regionExtended_);
    UpdateTooltips();
    Publish();
    if (target.monitorChanged)
        onMonitorChanged();
    onItemsChanged();
}

void Bar::Relayout()
{
    RelayoutTarget target;
    if (!ResolveLayoutTarget(target))
        return;
    const LayoutMetrics metrics = ComputeLayoutMetrics(target);
    BuildLayoutScene(target, metrics);
    ApplyLayout(target, metrics);
}

} // namespace atrium
