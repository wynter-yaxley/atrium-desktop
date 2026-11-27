// Stage Manager window lifecycle: staging, minimizing/adopting, picture capture, cache pruning, and beside-strip placement.
#include "ui/stage_manager.h"

#include "log.h"
#include "shell/displays.h"
#include "shell/process_integrity.h"

#include <dwmapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace atrium {

namespace {

void PruneStringSet(std::unordered_set<std::wstring>& issued, const std::unordered_set<std::wstring>& live,
                    const std::function<void(const std::wstring&)>& forget)
{
    for (auto it = issued.begin(); it != issued.end();) {
        if (live.contains(*it)) {
            ++it;
        } else {
            forget(*it);
            it = issued.erase(it);
        }
    }
}

} // namespace

void StageManager::CollectEntryIconKeys(const std::vector<std::wstring>& keys, int iconPx, size_t maxIcons, std::unordered_set<std::wstring>& out)
{
    const std::wstring pxSuffix = L"|" + std::to_wstring(iconPx);
    size_t count = 0;
    std::unordered_set<std::wstring> seen;
    for (const std::wstring& k : keys) {
        const std::wstring app = AppKeyOf(k);
        if (!seen.insert(app).second)
            continue;
        out.insert(L"stage|" + app + pxSuffix);
        if (++count >= maxIcons)
            break;
    }
}

int StageManager::NearestWindowDepth(const std::unordered_map<HWND, int>& depth, const std::unordered_map<HWND, Parked>& parked, const Set& set)
{
    int best = INT_MAX;
    for (const auto& [h, p] : parked) {
        if (HasKey(set, p.key)) {
            if (auto d = depth.find(h); d != depth.end())
                best = std::min(best, d->second);
        }
    }
    return best;
}

// Not a window to stage: a layer over everything (a screenshot's selection, an overlay), topmost
// without a title bar, or one covering the whole screen. It neither takes the stage, nor is put
// away, nor is moved beside the strip; covering the screen, it sends the strip away instead.
bool StageManager::Transient(HWND h)
{
    const LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOPMOST) && (style & WS_CAPTION) != WS_CAPTION)
        return true;
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    return !IsZoomed(h) && GetWindowRect(h, &r) && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi) && r.left <= mi.rcMonitor.left &&
           r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

void StageManager::SetTransitions(HWND h, bool on)
{
    BOOL off = on ? FALSE : TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

// Each window waits on a card of its own: keyed by its app's key and itself ('|' is in neither an
// app's path nor its AUMID).
std::wstring StageManager::WindowKey(const std::wstring& app, HWND h)
{
    wchar_t hex[24];
    swprintf_s(hex, L"|%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
    return app + hex;
}

std::wstring StageManager::AppKeyOf(const std::wstring& key)
{
    const size_t bar = key.rfind(L'|');
    return bar == std::wstring::npos ? key : key.substr(0, bar);
}

bool StageManager::HasKey(const Set& set, const std::wstring& key) { return std::ranges::find(set, key) != set.end(); }

void StageManager::ActionWorker()
{
    for (;;) {
        Action a;
        {
            std::unique_lock lk(actionLock_);
            actionCv_.wait(lk, [this] { return actionStop_ || !actions_.empty(); });
            if (actionStop_)
                return;
            a = actions_.front();
            actions_.pop_front();
        }
        if (!IsWindow(a.window))
            continue;
        if (a.kind != Action::Kind::MoveBeside) {
            a.kind == Action::Kind::Activate ? TaskModel::ActivateWindowDirect(a.window) : ShowQuietly(a.window);
            continue;
        }
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (!IsIconic(a.window) || !IsWindowEnabled(a.window) || IsZoomed(a.window) || !GetWindowPlacement(a.window, &wp) ||
            (wp.flags & WPF_RESTORETOMAXIMIZED))
            continue;
        RECT frame = IsRectEmpty(&a.frame) ? wp.rcNormalPosition : a.frame, to = frame;
        if (!StageBesideRect(to, a.zone, a.workArea, a.onRight))
            continue;
        wp.rcNormalPosition.left += to.left - frame.left;
        wp.rcNormalPosition.right += to.right - frame.right;
        wp.showCmd = SW_SHOWMINNOACTIVE;
        wp.flags = 0;
        SetWindowPlacement(a.window, &wp);
    }
}

StageManager::Set StageManager::SetOf(const std::wstring& key) const
{
    for (const Set& set : recent_)
        if (HasKey(set, key))
            return set;
    return Set{key};
}

void StageManager::RemoveKeysFromRecent(const Set& keys)
{
    for (const std::wstring& k : keys)
        for (Set& old : recent_)
            std::erase(old, k);
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
}

void StageManager::ParkReplacedWindows(const Set& keys, std::vector<Set>& left, std::vector<Set>& others)
{
    for (const TaskItem& t : items_) {
        if (HasKey(keys, t.key))
            continue;
        bool any = false;
        // Least recent first: the most recent window gets the latest number and shows in front.
        // A window filling the screen (a game, a video) is left alone: minimized, a full-screen
        // game comes back to its screen mode first, and flashes up before it goes.
        for (auto h = t.windows.rbegin(); h != t.windows.rend(); ++h) {
            if (ShouldParkStageWindow(parked_.contains(*h), IsWindowVisible(*h) != FALSE, IsIconic(*h) != FALSE, OnMonitor(*h, monitor_), Transient(*h),
                                      FillsScreen(*h), IsWindowEnabled(*h) != FALSE)) {
                Park(*h, t.key);
                any = true;
            }
        }
        if (any)
            (HasKey(stage_, t.key) ? left : others).push_back(Set{t.key});
    }
    // The most recently used first: the windows nearest the top of the screen's stack.
    std::unordered_map<HWND, int> depth;
    int n = 0;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        depth[h] = n++;
    auto byDepth = [&](const Set& a, const Set& b) { return NearestWindowDepth(depth, parked_, a) < NearestWindowDepth(depth, parked_, b); };
    std::ranges::stable_sort(left, byDepth);
    std::ranges::stable_sort(others, byDepth);
}

// `keys` take the stage (or join it: add). Their windows that were put away come back, `front` in
// front; then every other app's windows on this screen go to the strip: the apps that were on the
// stage at the top, each on a card of its own (only a drag puts windows together in one set).
void StageManager::Stage(const Set& keys, bool add, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly)
{
    Unpark(keys, front, activate, fly);
    if (!add) {
        openIntent_.Cancel(); // a different stage supersedes an app launch still waiting for a window
        std::vector<Set> left, others; // the stage's windows that were up; the rest
        ParkReplacedWindows(keys, left, others);
        others.insert(others.begin(), left.begin(), left.end());
        // An app is in one set only: the newest.
        for (const Set& set : others)
            RemoveKeysFromRecent(set);
        recent_.insert(recent_.begin(), others.begin(), others.end());
        stage_.clear();
        if (!keys.empty() || !others.empty())
            Log(L"stage: %s on the stage, %zu set(s) to the strip", keys.empty() ? L"the desktop" : keys.front().c_str(), others.size());
    } else if (!keys.empty()) {
        Log(L"stage: %s joins the stage", keys.front().c_str());
    }
    for (const std::wstring& k : keys)
        if (!HasKey(stage_, k))
            stage_.push_back(k);
    RemoveKeysFromRecent(keys);
    Save();
}

// The window's picture is taken first (a minimized window has none); it goes down once it has, or
// after kDownMs whatever happened.
void StageManager::Park(HWND window, const std::wstring& key)
{
    restoring_.erase(window);
    Parked p;
    p.key = key;
    p.seq = ++seq_;
    p.since = GetTickCount64();
    p.down = false;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &p.frame, sizeof(p.frame))))
        GetWindowRect(window, &p.frame);
    parked_[window] = p;
    SetTransitions(window, false); // it goes at once: its picture flies instead
    if (auto fresh = pictures_.find(window);
        fresh != pictures_.end() && fresh->second.picture && fresh->second.picture->ok && GetTickCount64() - fresh->second.taken < kFreshMs) {
        PutDown(window, true);
        return;
    }
    RequestPicture(window, true);
    SetTimer(surface_.Hwnd(), kDownTimer, kDownMs, nullptr);
}

// The stage's windows are pictured while the pointer is on the strip: picked there, they go at once.
void StageManager::Foresee()
{
    const ULONGLONG now = GetTickCount64();
    for (const TaskItem& t : items_) {
        if (!HasKey(stage_, t.key))
            continue;
        for (HWND h : t.windows) {
            if (parked_.contains(h) || !IsWindowVisible(h) || IsIconic(h) || !OnMonitor(h, monitor_))
                continue;
            if (auto p = pictures_.find(h); p != pictures_.end()) {
                const bool hasPicture = p->second.picture && p->second.picture->ok;
                const ULONGLONG minAge = hasPicture ? kFreshMs / 2 : StagePictureRetryMs(p->second.misses);
                if (now - p->second.taken < minAge)
                    continue;
            }
            pictures_[h].taken = now; // asked for: not again meanwhile
            RequestPicture(h);
        }
    }
}

void StageManager::RequestPicture(HWND window, bool urgent)
{
    const float s = PopupScale(WorkArea());
    snapshots_.Request(window, int(kBoxW * kPictureScale * s), int(kBoxH * kPictureScale * s), kPictureRadius * kPictureScale * s, kLargeMax, urgent);
}

void StageManager::SetLarge(Flight& f)
{
    auto it = pictures_.find(f.window);
    if (it == pictures_.end() || !it->second.large || !it->second.large->ok || !f.key.starts_with(L"stage|w|"))
        return;
    f.large = it->second.large;
    f.largeKey = f.key + L"|L";
}

void StageManager::PutDown(HWND window, bool fly)
{
    auto it = parked_.find(window);
    if (it == parked_.end() || it->second.down)
        return;
    it->second.down = true;
    it->second.since = GetTickCount64();
    std::wstring key;
    auto picture = fly ? PictureOf(window, it->second.key, key) : nullptr;
    if (!picture || !picture->ok || !key.starts_with(L"stage|w|")) {
        SetTransitions(window, true); // no picture to fly: Windows' own animation
        ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
        return;
    }
    std::erase_if(flights_, [&](const Flight& f) { return f.window == window; });
    Flight f;
    f.window = window;
    f.picture = picture;
    f.key = key;
    f.toStrip = true;
    f.place = PlacePose(it->second.frame);
    SetLarge(f);
    flights_.push_back(std::move(f));
    Launch();
    ShowWindowAsync(window, SW_SHOWMINNOACTIVE);
    surface_.Invalidate();
}

// The stage's windows get their own minimize animation back (the option off, Stage Manager off, exit):
// also those just back from the strip and not looked at yet.
void StageManager::ReleaseUp()
{
    for (const auto& [h, frame] : up_)
        if (IsWindow(h) && !parked_.contains(h) && !Flying(h))
            SetTransitions(h, true);
    for (const TaskItem& t : items_)
        for (HWND h : t.windows)
            if (!up_.contains(h) && !parked_.contains(h) && !Flying(h) && IsWindow(h))
                SetTransitions(h, true);
    up_.clear();
    restoring_.clear();
}

void StageManager::SyncUpTransitions(const std::unordered_map<HWND, RECT>& up)
{
    for (const auto& [h, frame] : up)
        if ((!up_.contains(h) || Flying(h)) && HasPicture(h))
            SetTransitions(h, false);
    for (const auto& [h, frame] : up_)
        if (!up.contains(h) && !parked_.contains(h) && IsWindow(h))
            SetTransitions(h, true);
}

void StageManager::FlyAdopted(const std::vector<HWND>& flyIn)
{
    for (HWND h : flyIn) {
        std::wstring key;
        auto picture = PictureOf(h, parked_[h].key, key);
        if (!picture || !picture->ok || !key.starts_with(L"stage|w|"))
            continue; // no picture yet: the card just comes
        std::erase_if(flights_, [&](const Flight& f) { return f.window == h; });
        Flight f;
        f.window = h;
        f.picture = picture;
        f.key = key;
        f.toStrip = true;
        f.place = PlacePose(parked_[h].frame);
        SetLarge(f);
        flights_.push_back(std::move(f));
    }
}

bool StageManager::CollectAdoptedForTask(const TaskItem& t, ULONGLONG now, std::unordered_map<HWND, RECT>& up, std::vector<HWND>& flyIn)
{
    bool anyUp = false;
    std::vector<HWND> down;
    for (HWND h : t.windows) {
        const bool iconic = IsIconic(h);
        const auto action =
            DecideStageAdopt(parked_.contains(h), IsWindowVisible(h) != FALSE, iconic, !iconic && OnMonitor(h, monitor_), up_.contains(h), IsRestoring(h, now));
        if (action == StageAdoptAction::Up) {
            anyUp = true;
            RECT r{};
            if (auto seen = up_.find(h); seen != up_.end())
                r = seen->second;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
                GetWindowRect(h, &r);
            up[h] = r;
        } else if (action == StageAdoptAction::Adopt) {
            down.push_back(h);
        }
    }
    if (anyUp || down.empty())
        return false;
    for (HWND h : down) {
        Parked p{t.key, ++seq_, now, true, {}};
        if (auto seen = up_.find(h); seen != up_.end())
            p.frame = seen->second;
        else if (WINDOWPLACEMENT wp{sizeof(wp)}; GetWindowPlacement(h, &wp))
            p.frame = wp.rcNormalPosition;
        parked_[h] = p;
        SetTransitions(h, !HasPicture(h)); // picked in the strip, its picture flies back: no zoom of its own
        flyIn.push_back(h);
    }
    return true;
}

bool StageManager::AdoptMinimized()
{
    if (!minimizeToStrip_ || !enabled_) {
        for (const auto& [h, frame] : up_)
            if (IsWindow(h) && !parked_.contains(h))
                SetTransitions(h, true);
        up_.clear();
        restoring_.clear();
        return false;
    }
    std::unordered_map<HWND, RECT> up;
    Set gone;
    std::vector<HWND> flyIn;
    const ULONGLONG now = GetTickCount64();
    PruneRestoring(now);
    for (const TaskItem& t : items_)
        if (HasKey(stage_, t.key) && CollectAdoptedForTask(t, now, up, flyIn))
            gone.push_back(t.key);
    SyncUpTransitions(up);
    up_ = std::move(up);
    if (gone.empty())
        return false;
    for (const std::wstring& k : gone)
        std::erase(stage_, k);
    RemoveKeysFromRecent(gone);
    for (const std::wstring& k : gone)
        recent_.insert(recent_.begin(), Set{k}); // each on a card of its own
    FlyAdopted(flyIn);
    const bool hasPicture = std::ranges::any_of(flyIn, [&](HWND h) { return Flying(h); });
    if (hasPicture)
        Launch();
    Log(L"stage: %s minimized to the strip (%s)", gone.front().c_str(), hasPicture ? L"picture" : L"placeholder");
    Save();
    return true;
}

// The window in front, if on the stage: its picture kept fresh (every few seconds, and as the pointer
// nears its title bar, where it is minimized), since a minimized window has none to take.
void StageManager::KeepPicture()
{
    if (!minimizeToStrip_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    auto seen = root ? up_.find(root) : up_.end();
    const ULONGLONG now = GetTickCount64();
    if (root != front_) {
        front_ = root;
        frontSince_ = now;
    }
    if (seen == up_.end() || IsIconic(root))
        return;
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) && !GetWindowRect(root, &r))
        return;
    seen->second = r; // where it comes back to
    POINT p{};
    GetCursorPos(&p);
    const float s = PopupScale(WorkArea());
    // Near the caption buttons (where a minimize comes from) the picture is kept fresh; elsewhere
    // seldom: PrintWindow makes the app draw itself whole, costly for a browser (#50).
    const bool titleBar = PtInRect(&r, p) && p.y < r.top + LONG(56 * s) && p.x > r.right - LONG(180 * s);
    const auto it = pictures_.find(root);
    const bool hasPicture = it != pictures_.end() && it->second.picture && it->second.picture->ok;
    const int misses = it == pictures_.end() ? 0 : it->second.misses;
    const ULONGLONG age = it == pictures_.end() || it->second.taken == 0 ? ULLONG_MAX : now - it->second.taken;
    if (!ShouldTakeStagePicture(hasPicture, titleBar, misses, now - frontSince_, age))
        return;
    pictures_[root].taken = now; // asked for: not again meanwhile
    RequestPicture(root);
}

void StageManager::UpdateIconVersions(const std::vector<TaskItem>& items)
{
    std::unordered_set<std::wstring> active;
    for (const TaskItem& t : items) {
        active.insert(t.key);
        auto [it, inserted] = iconVersions_.emplace(t.key, t.iconVersion);
        if (inserted || it->second == t.iconVersion)
            continue;
        it->second = t.iconVersion;
        const std::wstring prefix = L"stage|" + t.key + L"|";
        const std::wstring altPrefix = L"stage|i|" + t.key + L"|";
        for (auto ki = issuedIconKeys_.begin(); ki != issuedIconKeys_.end();) {
            if (ki->starts_with(prefix) || ki->starts_with(altPrefix)) {
                surface_.ForgetImage(*ki);
                ki = issuedIconKeys_.erase(ki);
            } else {
                ++ki;
            }
        }
    }
    std::erase_if(iconVersions_, [&](const auto& kv) { return !active.contains(kv.first); });
}

void StageManager::RecordActiveIconKeys()
{
    const int iconPx = int(std::lround(kIcon * surface_.Scale()));
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list)
            CollectEntryIconKeys(e.keys, iconPx, kStack, issuedIconKeys_);
}

void StageManager::PruneSurfaceCaches()
{
    std::unordered_set<std::wstring> liveWindows;
    for (const auto& [h, p] : pictures_)
        if (p.gen != 0)
            liveWindows.insert(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L"|" + std::to_wstring(p.gen));
    for (const Flight& f : flights_)
        if (f.key.starts_with(L"stage|w|"))
            liveWindows.insert(f.key);
    PruneStringSet(issuedWindowKeys_, liveWindows, [this](const std::wstring& key) { ForgetImages(key, true); });

    std::unordered_set<std::wstring> liveApps;
    for (const auto& [h, p] : parked_)
        liveApps.insert(AppKeyOf(p.key));
    for (const Set& set : recent_)
        for (const std::wstring& k : set)
            liveApps.insert(AppKeyOf(k));
    for (const auto* list : {&entries_, &leaving_})
        for (const Entry& e : *list)
            for (const std::wstring& k : e.keys)
                liveApps.insert(AppKeyOf(k));
    for (const Flight& f : flights_)
        if (f.key.starts_with(L"stage|p|"))
            liveApps.insert(f.key.substr(8));
    for (auto it = placeholders_.begin(); it != placeholders_.end();) {
        if (liveApps.contains(it->first)) {
            ++it;
        } else {
            ForgetImages(L"stage|p|" + it->first, false);
            issuedPlaceholderApps_.erase(it->first);
            it = placeholders_.erase(it);
        }
    }
    PruneStringSet(issuedPlaceholderApps_, liveApps, [this](const std::wstring& app) { ForgetImages(L"stage|p|" + app, false); });

    std::unordered_set<std::wstring> liveIcons;
    const int iconPx = int(std::lround(kIcon * surface_.Scale()));
    if (enabled_) {
        for (const auto* list : {&entries_, &leaving_})
            for (const Entry& e : *list)
                CollectEntryIconKeys(e.keys, iconPx, kStack, liveIcons);
    }
    PruneStringSet(issuedIconKeys_, liveIcons, [this](const std::wstring& key) { surface_.ForgetImage(key); });
    if (!enabled_ || (entries_.empty() && leaving_.empty()))
        surface_.ForgetImage(L"stage|shadow");
}

// Windows that went (closed, hidden to the tray) or came back some other way are no longer waiting.
void StageManager::Prune()
{
    const ULONGLONG now = GetTickCount64();
    std::erase_if(parked_, [&](const auto& kv) {
        HWND h = kv.first;
        if (!IsWindow(h))
            return true;
        if (!IsWindowVisible(h)) {
            SetTransitions(h, true);
            return true;
        }
        // Minimizing is asynchronous (and waits for the picture): a window just put away may not be down yet.
        if (kv.second.down && !IsIconic(h) && now - kv.second.since > kSettleMs) {
            SetTransitions(h, true); // came back some other way
            return true;
        }
        return false;
    });
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) { return std::ranges::none_of(parked_, [&](const auto& kv) { return kv.second.key == k; }); });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
    // Pictures of windows no longer waiting are old by the time they wait again; but not those of the
    // stage's (on their way back from the strip, or up): minimized, they go back with them.
    std::unordered_set<HWND> staged;
    if (minimizeToStrip_)
        for (const TaskItem& t : items_)
            if (HasKey(stage_, t.key))
                staged.insert(t.windows.begin(), t.windows.end());
    std::erase_if(pictures_, [&](const auto& kv) {
        if (parked_.contains(kv.first) || (IsWindow(kv.first) && now - kv.second.taken < kFreshMs) || up_.contains(kv.first) || Flying(kv.first) ||
            (staged.contains(kv.first) && IsWindow(kv.first)))
            return false;
        ForgetImages(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(kv.first)) + L"|" + std::to_wstring(kv.second.gen), true);
        return true;
    });
    PruneSurfaceCaches();
    Save();
}

// ---- beside the strip --------------------------------------------------------------------------

// A window over the strip's place moves beside it as it takes the stage: up, at once; minimized,
// its place to come back to (its picture lands there). Maximized windows stay as they are, under
// the strip, and so do minimized ones that come back maximized (IsZoomed is false while they are down):
// moved, the picture would land beside the strip and the window then come up over all of the screen.
void StageManager::MoveBeside(HWND window)
{
    if (!window || !IsWindow(window) || !IsWindowEnabled(window) || IsZoomed(window) || !OnMonitor(window, monitor_))
        return;
    const bool down = IsIconic(window) != FALSE;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (down ? (GetWindowPlacement(window, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED)) : Transient(window))
        return;
    auto parked = parked_.find(window);
    RECT frame{};
    if (down) {
        if (parked == parked_.end() || !enabled_ || autoHide_ || IsRectEmpty(&rect_))
            return;
        frame = parked->second.frame;
        if (IsRectEmpty(&frame)) {
            EnqueueAction({Action::Kind::MoveBeside, window, {}, Zone(), WorkArea(), onRight_});
            return;
        }
    } else if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        return;
    }
    RECT to = frame;
    if (!Beside(to))
        return;
    const LONG dl = to.left - frame.left, dr = to.right - frame.right;
    if (parked != parked_.end())
        parked->second.frame = to;
    if (down) {
        EnqueueAction({Action::Kind::MoveBeside, window, frame, Zone(), WorkArea(), onRight_});
    } else {
        RECT r{};
        GetWindowRect(window, &r);
        SetWindowPos(window, nullptr, r.left + dl, r.top, r.right + dr - r.left - dl, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }
    Log(L"stage: %p moved beside the strip", window);
}

} // namespace atrium
