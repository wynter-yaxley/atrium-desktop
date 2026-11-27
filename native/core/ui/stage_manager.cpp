#include "ui/stage_manager.h"

#include "log.h"
#include "settings.h"
#include "shell/games.h"

#include <windowsx.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

constexpr UINT WM_STAGE_PICTURE = WM_APP + 0x5C; // a window's picture was taken

std::wstring StatePath()
{
    std::wstring path = SettingsPath();
    return path.substr(0, path.find_last_of(L'\\')) + L"\\stage.json";
}

StageManager* g_stage = nullptr; // the one Stage Manager, for its WinEvent hooks

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG object, LONG, DWORD, DWORD)
{
    if (object != OBJID_WINDOW || !h || !g_stage)
        return;
    if (event == EVENT_SYSTEM_MINIMIZESTART)
        g_stage->Minimized(h);
    else
        g_stage->MoveSize(h, event == EVENT_SYSTEM_MOVESIZESTART);
}

std::vector<std::pair<HWND, std::wstring>> ParseParkedArray(const json::JsonObject& root)
{
    std::vector<std::pair<HWND, std::wstring>> out;
    if (!root.HasKey(L"parked") || root.GetNamedValue(L"parked").ValueType() != json::JsonValueType::Array)
        return out;
    for (const auto& v : root.GetNamedArray(L"parked")) {
        if (v.ValueType() != json::JsonValueType::Object)
            continue;
        auto o = v.GetObject();
        if (!o.HasKey(L"hwnd") || !o.HasKey(L"key") || o.GetNamedValue(L"hwnd").ValueType() != json::JsonValueType::Number ||
            o.GetNamedValue(L"key").ValueType() != json::JsonValueType::String)
            continue;
        HWND h = reinterpret_cast<HWND>(uintptr_t(o.GetNamedNumber(L"hwnd")));
        // A window handle is only reused once its window is gone; one still down is the same.
        if (!IsWindow(h) || !IsWindowVisible(h) || !IsIconic(h))
            continue;
        std::wstring key(o.GetNamedString(L"key"));
        if (key.find(L'|') == std::wstring::npos)
            key = StageManager::WindowKey(key, h); // kept by an earlier version, by app
        out.emplace_back(h, std::move(key));
    }
    return out;
}

std::vector<std::vector<std::wstring>> ParseRecentSets(const json::JsonObject& root)
{
    std::vector<std::vector<std::wstring>> out;
    if (!root.HasKey(L"sets") || root.GetNamedValue(L"sets").ValueType() != json::JsonValueType::Array)
        return out;
    for (const auto& v : root.GetNamedArray(L"sets")) {
        if (v.ValueType() != json::JsonValueType::Array)
            continue;
        std::vector<std::wstring> raw;
        for (const auto& k : v.GetArray())
            if (k.ValueType() == json::JsonValueType::String)
                raw.emplace_back(k.GetString());
        if (!raw.empty())
            out.push_back(std::move(raw));
    }
    return out;
}

} // namespace

StageManager::StageManager(HINSTANCE instance, TaskModel& tasks)
    : tasks_(tasks),
      surface_(instance, L"Atrium Stage Manager", false),
      flight_(instance, L"Atrium Stage Flight", false, 0, false, true),
      snapshots_(surface_.Hwnd(), WM_STAGE_PICTURE),
      actionWorker_([this] { ActionWorker(); })
{
    surface_.SetGlassless(true);
    flight_.SetGlassless(true);
    flight_.paint = [this](Canvas& c) { PaintFlights(c); };
    flight_.message = [this](UINT m, WPARAM w, LPARAM, LRESULT&) {
        if (m != WM_TIMER || w != kFlightTimer)
            return false;
        KillTimer(flight_.Hwnd(), kFlightTimer);
        if (flights_.empty()) {
            flight_.Hide(false); // idle for a while: its buffers go
            PruneSurfaceCaches();
        }
        SyncWatchTimer(GetTickCount64());
        return true;
    };
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    if (FAILED(CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(desktops_.put()))))
        desktops_ = nullptr;
    Load(); // windows a previous run put away (it may not have ended well)
}

StageManager::~StageManager()
{
    if (g_stage == this)
        g_stage = nullptr;
    if (minimizeHook_)
        UnhookWinEvent(minimizeHook_);
    if (moveHook_)
        UnhookWinEvent(moveHook_);
    std::unique_lock lk(actionLock_);
    actionStop_ = true;
    lk.unlock();
    actionCv_.notify_all();
    if (actionWorker_.joinable())
        actionWorker_.join();
}

// ---- the stage ---------------------------------------------------------------------------------

void StageManager::ReleaseExcludedUpWindows()
{
    if (up_.empty())
        return;
    const std::vector<TaskItem> snapshot = tasks_.Snapshot(true);
    std::unordered_set<HWND> excludedHwnds;
    for (const TaskItem& t : snapshot)
        if (StageAppExcluded(excluded_, t.key))
            excludedHwnds.insert(t.windows.begin(), t.windows.end());
    for (auto it = up_.begin(); it != up_.end();) {
        if (excludedHwnds.contains(it->first)) {
            SetTransitions(it->first, true);
            it = up_.erase(it);
        } else {
            ++it;
        }
    }
}

void StageManager::SetExcluded(std::vector<std::wstring> apps)
{
    const bool changed = apps != excluded_;
    if (changed)
        excluded_ = std::move(apps);
    // Anything an excluded app already put away comes back and leaves the stage model. The sweep
    // also runs when the list did not change, since saved state may hold entries from before.
    bool touched = false;
    Set release;
    for (const auto& [h, p] : parked_)
        if (StageAppExcluded(excluded_, p.key) && !HasKey(release, p.key))
            release.push_back(p.key);
    if (!release.empty()) {
        Unpark(release, nullptr, false, nullptr, false);
        touched = true;
    }
    touched |= std::erase_if(stage_, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    for (Set& set : recent_)
        touched |= std::erase_if(set, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    touched |= std::erase_if(recent_, [](const Set& set) { return set.empty(); }) > 0;
    touched |= std::erase_if(pending_, [&](const std::wstring& key) { return StageAppExcluded(excluded_, key); }) > 0;
    const size_t beforeUp = up_.size();
    ReleaseExcludedUpWindows();
    touched |= up_.size() != beforeUp;
    if (!changed && !touched)
        return;
    openIntent_.Cancel();
    Save();
    Update(tasks_.Snapshot(true));
    Rebuild();
}

void StageManager::SetEnabled(bool on)
{
    if (on == enabled_ && applied_)
        return;
    applied_ = true;
    enabled_ = on;
    openIntent_.Cancel();
    pressed_ = -1;
    SyncMoveHook();
    if (on) {
        stage_.clear();
        pending_.clear();
        watchMs_ = 0;
        Log(L"stage: on (%s, %s), %zu window(s) waiting from before", onRight_ ? L"right" : L"left", autoHide_ ? L"auto-hidden" : L"always shown",
            parked_.size());
        Update(tasks_.Snapshot(true));
        SyncWatchTimer(GetTickCount64());
    } else {
        KillTimer(surface_.Hwnd(), kWatchTimer);
        watchMs_ = 0;
        size_t n = parked_.size();
        HWND front = GetForegroundWindow();
        RestoreAll();
        // The windows coming back must not cover the one in front.
        if (n && front) {
            raise_ = front;
            SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
        }
        Log(L"stage: off, %zu window(s) back", n);
    }
}

void StageManager::SetOnRight(bool onRight)
{
    if (onRight == onRight_)
        return;
    onRight_ = onRight;
    // It comes out again on the other side.
    shown_ = peek_ = false;
    surface_.Hide(false);
    Rebuild();
}

void StageManager::SetBarEdge(BarEdge edge)
{
    if (edge == barEdge_)
        return;
    barEdge_ = edge;
    coveredAt_ = 0;
    Rebuild();
}

void StageManager::SetAutoHide(bool autoHide)
{
    if (autoHide == autoHide_)
        return;
    autoHide_ = autoHide;
    peek_ = false;
    Log(L"stage: %s", autoHide ? L"auto-hidden" : L"always shown");
    if (enabled_)
        Watch();
}

void StageManager::SetScrollWrap(bool on)
{
    if (on == scrollWrap_)
        return;
    scrollWrap_ = on;
    scroll_.SetWrap(on);
    scroll_.Fit(scrollCount_, size_t(Capacity()));
    Rebuild(false);
}

void StageManager::SetMinimizeToStrip(bool on)
{
    if (on == minimizeToStrip_)
        return;
    minimizeToStrip_ = on;
    ReleaseUp();
    // Out of context, on this thread: the picture leaves the window's place as it goes down.
    if (on && !minimizeHook_) {
        g_stage = this;
        minimizeHook_ =
            SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!on && minimizeHook_) {
        UnhookWinEvent(minimizeHook_);
        minimizeHook_ = nullptr;
    }
    Log(L"stage: minimized windows %s", on ? L"go to the strip" : L"stay minimized");
}

void StageManager::SyncMoveHook()
{
    const bool want = enabled_ && dragMerge_;
    if (want && !moveHook_) {
        g_stage = this;
        moveHook_ =
            SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    } else if (!want && moveHook_) {
        UnhookWinEvent(moveHook_);
        moveHook_ = nullptr;
    }
    if (!want && (dragWindow_ || dragHover_ >= 0))
        CancelDrag();
}

void StageManager::SetMonitor(HMONITOR monitor, bool refresh)
{
    const bool changed = monitor != monitor_;
    if (!changed && !refresh)
        return;
    // Release windows on the old display before managing another one.
    if (changed && monitor_)
        RestoreAll();
    monitor_ = monitor;
    coveredAt_ = 0;
    if (changed && enabled_)
        Update(tasks_.Snapshot(true));
    Rebuild();
    if (changed && enabled_)
        Watch();
}

void StageManager::ProcessOpenIntent()
{
    Set opened, openedApps;
    for (const TaskItem& t : items_) {
        if (!openIntent_.Matches(AppKeyOf(t.key), GetTickCount64()))
            continue;
        for (HWND h : t.windows) {
            BOOL here = TRUE;
            if (IsWindow(h) && IsWindowVisible(h) && OnMonitor(h, monitor_) && !Transient(h) &&
                (!desktops_ || FAILED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) || here)) {
                opened.push_back(t.key);
                if (!HasKey(openedApps, AppKeyOf(t.key)))
                    openedApps.push_back(AppKeyOf(t.key));
            }
        }
    }
    if (opened.empty())
        return;
    // Only these launches are done: another app added meanwhile still waits for its window.
    for (const std::wstring& app : openedApps)
        openIntent_.Done(app);
    pending_ = opened;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    Stage(opened, true, nullptr, false);
}

void StageManager::BuildTaskItems(const std::vector<TaskItem>& items, HWND root)
{
    // Window by window: each its own item, as each has its own card. Excluded apps never
    // enter the stage model at all.
    items_.clear();
    excludedWindows_.clear();
    size_t totalWindows = 0;
    for (const TaskItem& t : items)
        totalWindows += t.windows.size();
    items_.reserve(totalWindows);
    for (const TaskItem& t : items) {
        // Games too: their windows stay on the screen, whatever opens.
        if (StageAppExcluded(excluded_, t.key) || std::ranges::any_of(t.windows, IsGameWindow)) {
            // Never put away, but still a window the strip keeps out of the way of.
            excludedWindows_.insert(excludedWindows_.end(), t.windows.begin(), t.windows.end());
            continue;
        }
        const bool rootHere = std::ranges::find(t.windows, root) != t.windows.end();
        const TaskItem base{L"", t.name, t.pinned, t.pinIndex, {}, false, t.attention, t.badge, t.iconVersion, false};
        for (HWND h : t.windows) {
            if (IsHigherIntegrity(h, t.key)) {
                excludedWindows_.push_back(h);
                continue;
            }
            TaskItem w = base;
            w.key = WindowKey(t.key, h);
            w.windows.push_back(h);
            // In front: the window itself, or the app's most recent one when what is in front is not one of them.
            w.active = t.active && (rootHere ? h == root : h == t.windows.front());
            items_.push_back(std::move(w));
        }
    }
}

void StageManager::Update(const std::vector<TaskItem>& items)
{
    if (!enabled_)
        return;
    HWND fg = GetForegroundWindow();
    HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    UpdateIconVersions(items);
    BuildTaskItems(items, root);
    coveredAt_ = 0; // windows moved: look again
    Prune();
    ProcessOpenIntent();
    AdoptMinimized();
    if (!pending_.empty() && GetTickCount64() > pendingUntil_)
        pending_.clear();
    auto active = std::ranges::find_if(items_, [](const TaskItem& t) { return t.active; });
    // A window in front, on this screen, of an app not on the stage: its set takes the stage. Not
    // while a set picked in the strip is on its way to the front.
    if (active != items_.end() && root && root != dragWindow_ && !IsIconic(root) && OnMonitor(root, monitor_) && !Transient(root)) {
        if (pending_.empty() || HasKey(pending_, active->key)) {
            pending_.clear();
            if (!HasKey(stage_, active->key)) {
                // Another window of an app on the stage (a chat opened from the main window) joins
                // it; the app's other windows stay up.
                const std::wstring app = AppKeyOf(active->key);
                const bool sameApp = std::ranges::any_of(stage_, [&](const std::wstring& k) { return AppKeyOf(k) == app; });
                Stage(sameApp ? Set{active->key} : SetOf(active->key), sameApp, root, false);
                MoveBeside(root); // a window taking the stage over the strip moves beside it
            }
        }
    }
    AdoptMinimized(); // the stage's windows as they are now, ready to be minimized
    Rebuild();
}

void StageManager::Pick(const Set& keys, bool add, HWND front)
{
    pending_ = keys;
    pendingUntil_ = GetTickCount64() + kPendingMs;
    hover_ = pressed_ = -1;
    // Where the card's pictures are now: they fly from there.
    std::unordered_map<HWND, Pose> from;
    for (const Entry& e : entries_)
        if (e.keys == keys)
            for (HWND h : e.windows) {
                Pose pose;
                if (StripPose(h, pose))
                    from[h] = pose;
            }
    pendingUntil_ += ULONGLONG(kFlightMs);
    Stage(keys, add, front, true, &from);
    Rebuild();
}

void StageManager::OpenInCurrentGroup(const std::wstring& app, const std::function<void()>& launch)
{
    if (!enabled_ || app.empty() || StageAppExcluded(excluded_, app))
        return; // excluded apps are left where they are (their own open command still works)
    Update(tasks_.Snapshot(true)); // the menu's snapshot may already be out of date
    Set keys;
    HWND front = nullptr;
    for (const TaskItem& t : items_)
        if (AppKeyOf(t.key) == app)
            for (HWND h : t.windows) {
                BOOL here = TRUE;
                if (!IsWindow(h) || !IsWindowVisible(h) || !OnMonitor(h, monitor_) || Transient(h) ||
                    (desktops_ && SUCCEEDED(desktops_->IsWindowOnCurrentVirtualDesktop(h, &here)) && !here))
                    continue;
                keys.push_back(t.key);
                if (!front)
                    front = h;
            }
    if (!keys.empty())
        Pick(keys, true, front);
    else {
        openIntent_.Arm(app, GetTickCount64());
        launch ? launch() : tasks_.Launch(app);
    }
}

// Minimized on the stage (setting stageMinimize): a window on the stage that is down now (it was up
// at the last look) leaves the stage and waits at the top of the strip, put away as if Atrium had
// (a pick there brings it back, where it was).
void StageManager::Minimized(HWND window)
{
    if (minimizeToStrip_ && enabled_ && (up_.contains(window) || InStage(window)) && AdoptMinimized())
        Rebuild();
}

std::vector<HWND> StageManager::ParkedInOrder() const
{
    std::vector<std::pair<uint64_t, HWND>> bySeq;
    bySeq.reserve(parked_.size());
    for (const auto& [h, p] : parked_)
        bySeq.emplace_back(p.seq, h);
    std::ranges::sort(bySeq);
    std::vector<HWND> out;
    out.reserve(bySeq.size());
    for (const auto& [seq, h] : bySeq)
        out.push_back(h);
    return out;
}

void StageManager::Save()
{
    json::JsonArray list;
    for (HWND h : ParkedInOrder()) {
        json::JsonObject o;
        o.SetNamedValue(L"hwnd", json::JsonValue::CreateNumberValue(double(reinterpret_cast<uintptr_t>(h))));
        o.SetNamedValue(L"key", json::JsonValue::CreateStringValue(parked_[h].key));
        list.Append(o);
    }
    json::JsonArray sets;
    for (const Set& set : recent_) {
        json::JsonArray keys;
        for (const std::wstring& k : set)
            keys.Append(json::JsonValue::CreateStringValue(k));
        sets.Append(keys);
    }
    json::JsonObject root;
    root.SetNamedValue(L"parked", list);
    root.SetNamedValue(L"sets", sets);
    const std::wstring path = StatePath();
    if (!SaveStageState(std::wstring(root.Stringify()), saved_, [&](const std::wstring& text) { return WriteTextFile(path, text); }))
        Log(L"stage: could not write %s", path.c_str());
}

void StageManager::Load()
{
    json::JsonObject root{nullptr};
    std::wstring text = ReadTextFile(StatePath());
    if (text.empty() || !json::JsonObject::TryParse(text, root))
        return;
    saved_ = text;
    for (auto& [h, key] : ParseParkedArray(root))
        parked_[h] = Parked{std::move(key), ++seq_, 0, true, {}};
    for (const auto& raw : ParseRecentSets(root)) {
        Set set;
        for (const std::wstring& key : raw) {
            // An app (an earlier version's set): its windows.
            for (const auto& [h, p] : parked_)
                if ((p.key == key || (key.find(L'|') == std::wstring::npos && AppKeyOf(p.key) == key)) && !HasKey(set, p.key))
                    set.push_back(p.key);
        }
        for (const std::wstring& k : set) // each on a card of its own (an earlier version kept the stage's windows together)
            recent_.push_back(Set{k});
    }
    // Apps only known from their windows: each a set of its own.
    for (const auto& [h, p] : parked_)
        if (std::ranges::none_of(recent_, [&](const Set& set) { return HasKey(set, p.key); }))
            recent_.push_back(Set{p.key});
    for (Set& set : recent_)
        std::erase_if(set, [&](const std::wstring& k) { return std::ranges::none_of(parked_, [&](const auto& kv) { return kv.second.key == k; }); });
    std::erase_if(recent_, [](const Set& set) { return set.empty(); });
}

bool StageManager::OnTimerMessage(WPARAM w)
{
    if (w == kWatchTimer) {
        Watch();
        return true;
    }
    if (w == kDownTimer) {
        KillTimer(surface_.Hwnd(), kDownTimer);
        std::vector<HWND> late;
        for (const auto& [h, p] : parked_)
            if (!p.down && GetTickCount64() - p.since >= kDownMs - 20)
                late.push_back(h);
        for (HWND h : late)
            PutDown(h, false); // no picture in time: it waits without one
        if (std::ranges::any_of(parked_, [](const auto& kv) { return !kv.second.down; }))
            SetTimer(surface_.Hwnd(), kDownTimer, kDownMs / 4, nullptr);
        SyncWatchTimer(GetTickCount64());
        return true;
    }
    if (w == kHideTimer) {
        KillTimer(surface_.Hwnd(), kHideTimer);
        if (shown_ && entries_.empty() && leaving_.empty()) {
            Hide(); // slides away, then comes back here
        } else if (!shown_) {
            surface_.Hide(false);
            PruneSurfaceCaches();
        }
        SyncWatchTimer(GetTickCount64());
        return true;
    }
    if (w == kRaiseTimer) {
        KillTimer(surface_.Hwnd(), kRaiseTimer);
        if (raise_ && IsWindow(raise_) && IsWindowVisible(raise_) && !IsIconic(raise_))
            SetWindowPos(raise_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        raise_ = nullptr;
        return true;
    }
    return false;
}

bool StageManager::Message(UINT m, WPARAM w, LPARAM l, LRESULT& result)
{
    (void)result;
    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_STAGE_PICTURE: PictureTaken(); return true;
    case WM_TIMER: return OnTimerMessage(w);
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // Wheel messages carry screen pixels, unlike the other Surface mouse messages.
        const POINT content = surface_.ScreenToContent(pt);
        if (!shown_ || EntryAt(content) < 0)
            return false;
        pressed_ = -1;
        if (scroll_.Wheel(GET_WHEEL_DELTA_WPARAM(w), m == WM_MOUSEHWHEEL, scrollCount_, size_t(Capacity())))
            hover_ = -1, Rebuild(false); // an explicit scroll changes the anchor; model-only rebuilds keep it
        surface_.Invalidate();
        SyncWatchTimer(GetTickCount64());
        return true;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
            tracking_ = TrackMouseEvent(&tme) != FALSE;
        }
        if (!inside_)
            Foresee();
        inside_ = true;
        int i = shown_ ? EntryAt(pt) : -1;
        if (i != hover_) {
            hover_ = i;
            surface_.Invalidate();
        }
        CloseInput(m, pt);
        SyncWatchTimer(GetTickCount64());
        return true;
    }
    case WM_MOUSELEAVE:
        tracking_ = false, inside_ = false;
        hover_ = pressed_ = closeHot_ = closePressed_ = -1;
        surface_.Invalidate();
        SyncWatchTimer(GetTickCount64());
        return true;
    case WM_LBUTTONDOWN:
        if (CloseInput(m, pt)) {
            SyncWatchTimer(GetTickCount64());
            return true;
        }
        Foresee();
        pressed_ = shown_ ? EntryAt(pt) : -1;
        surface_.Invalidate();
        SyncWatchTimer(GetTickCount64());
        return true;
    case WM_LBUTTONUP: {
        if (CloseInput(m, pt)) {
            SyncWatchTimer(GetTickCount64());
            return true;
        }
        int i = EntryAt(pt);
        bool same = shown_ && i >= 0 && i == pressed_;
        pressed_ = -1;
        surface_.Invalidate();
        if (same) {
            Set keys = entries_[size_t(i)].keys;
            Log(L"stage: %s picked in the strip%s", keys.front().c_str(), (w & MK_SHIFT) ? L" (added)" : L"");
            Pick(keys, (w & MK_SHIFT) != 0);
        }
        SyncWatchTimer(GetTickCount64());
        return true;
    }
    default: return false;
    }
}

} // namespace atrium
