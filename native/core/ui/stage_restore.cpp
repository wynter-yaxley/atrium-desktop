// Stage Manager bringing windows back from the strip: picks, restore-all, and the restore bookkeeping
// that keeps a window on its way back out of re-adoption.
#include "ui/stage_manager.h"

#include "log.h"
#include "shell/process_integrity.h"

#include <algorithm>

namespace atrium {

// Back without taking the foreground. Never a synchronous call into another app's window: one that
// hangs must not hang the bar. A window minimized from maximized comes back maximized:
// SW_SHOWNOACTIVATE alone restores its normal size, a maximized app back at half the screen (#19).
void StageManager::ShowQuietly(HWND h)
{
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (IsIconic(h) && GetWindowPlacement(h, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED)) {
        ShowWindowAsync(h, SW_SHOWMAXIMIZED);
        return;
    }
    ShowWindowAsync(h, SW_SHOWNOACTIVATE);
}

// The windows of `keys` that were put away come back, least recent first; `front` (by default the
// most recent of them) last, and in front, activated if asked (a pick in the strip).
void StageManager::Unpark(const Set& keys, HWND front, bool activate, const std::unordered_map<HWND, Pose>* fly, bool moveBeside)
{
    std::vector<std::pair<uint64_t, HWND>> mine;
    for (const auto& [h, p] : parked_)
        if (HasKey(keys, p.key))
            mine.emplace_back(p.seq, h);
    std::ranges::sort(mine);
    if (!front && !mine.empty())
        front = mine.back().second;
    // One of them comes back maximized: the bar goes with the flight, not after it lands.
    if (onCoverSoon && std::ranges::any_of(mine, [](const auto& m) {
            WINDOWPLACEMENT wp{sizeof(wp)};
            return IsZoomed(m.second) || (IsIconic(m.second) && GetWindowPlacement(m.second, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED));
        }))
        onCoverSoon(monitor_);
    // Kept out of the strip's band: they come back beside it.
    if (moveBeside)
        for (const auto& [seq, h] : mine)
            MoveBeside(h);
    // Picked in the strip: the pictures fly back to the windows' places, the windows come up as they land.
    bool flying = false;
    for (const auto& [seq, h] : mine) {
        const Pose* from = nullptr;
        if (fly)
            if (auto it = fly->find(h); it != fly->end())
                from = &it->second;
        std::wstring key;
        auto picture = from ? PictureOf(h, parked_[h].key, key) : nullptr;
        std::erase_if(flights_, [&](const Flight& f) { return f.window == h; });
        if (!picture || !IsWindow(h) || !key.starts_with(L"stage|w|"))
            continue;
        Flight f{h, picture, key, {}, {}, false, *from, PlacePose(parked_[h].frame), 0, 0, false, activate && h == front};
        SetLarge(f);
        flights_.push_back(std::move(f));
        flying = true;
    }
    if (flying)
        Launch();
    const ULONGLONG now = GetTickCount64();
    for (const auto& [seq, h] : mine) {
        parked_.erase(h);
        if (Flying(h))
            restoring_[h] = now; // wait out the flight until StepFlight triggers restore
        else
            UnparkDirect(h, h != front, now);
    }
    if (!front)
        return;
    parked_.erase(front);
    if (!Flying(front) && (activate || IsIconic(front)))
        EnqueueAction({activate ? Action::Kind::Activate : Action::Kind::ShowQuietly, front});
    if (!mine.empty()) {
        // The others come back asynchronously: `front` goes on top again once they have.
        raise_ = front;
        SetTimer(surface_.Hwnd(), kRaiseTimer, kRaiseMs, nullptr);
    }
}

void StageManager::RestoreAll()
{
    openIntent_.Cancel();
    std::unique_lock lk(actionLock_);
    actions_.clear();
    lk.unlock();
    ReleaseUp();
    items_.clear();
    excludedWindows_.clear();
    higherIntegrityLogged_.clear();
    shrinkAfterFlight_ = false;
    // Pictures on their way back: their windows come up now.
    for (Flight& f : flights_)
        if (!f.toStrip && !f.restored && IsWindow(f.window)) {
            SetTransitions(f.window, true);
            ShowQuietly(f.window);
        }
    flights_.clear();
    flight_.Hide(false);
    for (const auto& [h, p] : parked_)
        if (IsWindow(h))
            SetTransitions(h, true);
    for (HWND h : ParkedInOrder())
        if (IsWindow(h) && IsIconic(h))
            ShowQuietly(h);
    parked_.clear();
    recent_.clear();
    stage_.clear();
    pending_.clear();
    for (const auto& [h, p] : pictures_)
        if (p.gen)
            ForgetImages(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(h)) + L"|" + std::to_wstring(p.gen), true);
    pictures_.clear();
    for (const auto& [app, ph] : placeholders_)
        ForgetImages(L"stage|p|" + app, false);
    placeholders_.clear();
    Save();
    Rebuild();
}

bool StageManager::IsHigherIntegrity(HWND window, const std::wstring& appKey)
{
    if (!WindowHasHigherIntegrity(window))
        return false;
    if (higherIntegrityLogged_.insert(window).second)
        Log(L"stage: skip %s (%p): higher integrity level (UIPI)", appKey.c_str(), window);
    return true;
}

void StageManager::UnparkDirect(HWND h, bool showQuietly, ULONGLONG now)
{
    if (!minimizeToStrip_ || !HasPicture(h))
        SetTransitions(h, true);
    if (IsIconic(h))
        restoring_[h] = now;
    if (showQuietly && IsWindow(h))
        EnqueueAction({Action::Kind::ShowQuietly, h});
}

bool StageManager::IsRestoring(HWND window, ULONGLONG now) const
{
    const auto it = restoring_.find(window);
    const bool hasEntry = it != restoring_.end();
    const uint64_t ageMs = hasEntry && now >= it->second ? now - it->second : 0;
    return ShouldWaitStageRestore(hasEntry, ageMs, kSettleMs, FlyingFromStrip(window));
}

void StageManager::PruneRestoring(ULONGLONG now)
{
    std::erase_if(restoring_, [this, now](const auto& kv) {
        return !IsWindow(kv.first) || !IsWindowVisible(kv.first) || !IsIconic(kv.first) || (!FlyingFromStrip(kv.first) && now - kv.second > kSettleMs);
    });
}

void StageManager::ApplyShrinkResult(const WindowSnapshots::Result& r)
{
    auto it = pictures_.find(r.window);
    if (it == pictures_.end())
        return;
    if (it->second.gen == r.shrinkGen && r.large.ok && Flying(r.window))
        shrinkAfterFlight_ = true;
    if (it->second.gen == r.shrinkGen && r.large.ok && !Flying(r.window)) {
        it->second.large = std::make_shared<const WindowPicture>(r.large);
        it->second.shrinkingGen = 0;
        flight_.ForgetImage(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(r.window)) + L"|" + std::to_wstring(it->second.gen) + L"|L");
    } else if (it->second.shrinkingGen == r.shrinkGen) {
        it->second.shrinkingGen = 0;
    }
}

void StageManager::ScheduleOldLargeShrinks()
{
    shrinkAfterFlight_ = false;
    std::vector<std::pair<ULONGLONG, HWND>> full;
    for (const auto& [h, q] : pictures_)
        if (q.large && std::max(q.large->width, q.large->height) > kLargeOld)
            full.emplace_back(q.taken, h);
    std::ranges::sort(full, std::greater<>());
    for (size_t i = kLargeKept; i < full.size(); ++i) {
        Picture& q = pictures_[full[i].second];
        if (!q.large || q.shrinkingGen == q.gen)
            continue;
        if (Flying(full[i].second)) {
            shrinkAfterFlight_ = true;
            continue;
        }
        q.shrinkingGen = q.gen;
        snapshots_.RequestShrink(full[i].second, q.gen, q.large, kLargeOld);
    }
}

void StageManager::PictureTaken()
{
    for (WindowSnapshots::Result& r : snapshots_.Take()) {
        if (r.shrinkGen != 0) {
            ApplyShrinkResult(r);
            continue;
        }
        if (!enabled_ || !IsWindow(r.window))
            continue;
        if (parked_.contains(r.window))
            Log(L"stage: picture of %p %s", r.window,
                r.picture.ok ? (std::to_wstring(r.picture.width) + L"x" + std::to_wstring(r.picture.height)).c_str() : L"(none)");
        if (!r.picture.ok) {
            // None taken (or a blank one): an earlier picture flies, if there is one; otherwise back off retries.
            auto old = pictures_.find(r.window);
            const bool hadPicture = old != pictures_.end() && old->second.picture && old->second.picture->ok;
            if (!hadPicture) {
                Picture& p = pictures_[r.window];
                ++p.misses;
                p.taken = GetTickCount64();
            }
            PutDown(r.window, hadPicture);
            continue;
        }
        Picture& p = pictures_[r.window];
        if (p.gen)
            ForgetImages(L"stage|w|" + std::to_wstring(reinterpret_cast<uintptr_t>(r.window)) + L"|" + std::to_wstring(p.gen), true);
        p.picture = std::make_shared<const WindowPicture>(std::move(r.picture));
        p.large = r.large.ok ? std::make_shared<const WindowPicture>(std::move(r.large)) : nullptr;
        p.gen = ++pictureGen_;
        p.shrinkingGen = 0;
        p.taken = GetTickCount64();
        p.misses = 0;
        if (minimizeToStrip_ && enabled_ && up_.contains(r.window))
            SetTransitions(r.window, false);
        ScheduleOldLargeShrinks();
        PutDown(r.window, true);
    }
    surface_.Invalidate();
}

} // namespace atrium
