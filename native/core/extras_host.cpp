#include "extras_host.h"

#include "bar.h"
#include "log.h"
#include "posted.h"
#include "settings.h"

namespace atrium {

namespace {

// The workers' news, on ExtrasHost's own window.
constexpr UINT WM_MEDIA_STATE = WM_APP + 1; // lParam: MediaSnapshot*
constexpr UINT WM_STACKS_READY = WM_APP + 2;
constexpr UINT WM_MEDIA_WAKE = WM_APP + 3;
constexpr wchar_t kWindowClass[] = L"AtriumExtras";

// nowPlayingMotion as the bars' reach.
float Intensity(const std::wstring& motion)
{
    if (motion == L"off")
        return 0.f;
    if (motion == L"subtle")
        return 0.55f;
    return motion == L"vivid" ? 1.45f : 1.f;
}

// A click on an entry whose popup is open closes it; the click that just closed it (the popup lost
// the focus to the same bar) does not open it again.
template<class Popup> bool JustClosed(const Popup& popup, bool sameBar) { return sameBar && GetTickCount64() - popup.LastClosed() < 400; }

} // namespace

HWND ExtrasHost::CreateHost(HINSTANCE instance, ExtrasHost* self)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc); // a second host (never made) would find it registered
    const HWND h = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (h)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

LRESULT CALLBACK ExtrasHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<ExtrasHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_MEDIA_STATE) {
        const auto snapshot = TakePosted<MediaSnapshot>(l); // always taken back; a stale one is dropped
        if (self && self->media_)
            self->OnMedia(*snapshot);
        return 0;
    }
    if (self && m == WM_STACKS_READY) {
        self->stacks_.Refresh();
        return 0;
    }
    if (self && m == WM_MEDIA_WAKE) {
        self->wakeQueued_->store(false, std::memory_order_release);
        self->WakeMusic();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

ExtrasHost::ExtrasHost(HINSTANCE instance, AppCatalog& catalog, ForEachBar forBars)
    : forBars_(std::move(forBars)),
      host_(CreateHost(instance, this)),
      feed_(std::make_shared<MusicFeed>()),
      launchpad_(instance, catalog),
      card_(instance),
      stackModel_(host_, WM_STACKS_READY),
      stacks_(instance, stackModel_)
{
    card_.SetFeed(feed_);
    card_.onCommand = [this](MediaCommand command, int64_t positionUs) {
        if (media_)
            media_->Command(command, positionUs);
    };
    card_.onClose = [this] { UpdateSampling(); };
    stacks_.onManageFolders = [this] {
        if (onOpenSettings)
            onOpenSettings(L"stacks");
    };
}

ExtrasHost::~ExtrasHost()
{
    Shutdown();
    if (host_) {
        SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        DestroyWindow(host_);
    }
}

void ExtrasHost::Apply(const Settings& s, float radius)
{
    const float intensity = Intensity(s.nowPlayingMotion);
    const bool loop = s.nowPlayingRhythm == L"loop";
    const bool motionChanged = intensity != feed_->intensity.exchange(intensity, std::memory_order_relaxed);
    const bool rhythmChanged = loop != feed_->loop.exchange(loop, std::memory_order_relaxed);
    liveRhythm_ = s.nowPlayingMotion != L"off" && !loop;
    musicOn_ = s.showNowPlaying;
    if (musicOn_ && !media_) {
        samplingActive_.reset();
        const std::shared_ptr<MusicFeed> feed = feed_;
        const std::shared_ptr<std::atomic<bool>> wakeQueued = wakeQueued_;
        const HWND host = host_;
        bool wasAudible = false;
        media_ = std::make_unique<MediaSessionWatcher>(host_, WM_MEDIA_STATE, [feed, wakeQueued, host, wasAudible](float peak, bool sampling) mutable {
            feed->peak.store(peak, std::memory_order_relaxed);
            if (sampling)
                feed->playing.store(true, std::memory_order_relaxed);
            const bool audible = feed->playing.load(std::memory_order_relaxed) && MusicBars::Loudness(feed->EffectivePeak()) > 0.f;
            if (audible && (!wasAudible || feed->needWake.exchange(false, std::memory_order_acq_rel)) && host &&
                !wakeQueued->exchange(true, std::memory_order_acq_rel)) {
                if (!PostMessageW(host, WM_MEDIA_WAKE, 0, 0))
                    wakeQueued->store(false, std::memory_order_release);
            }
            wasAudible = audible;
        });
    } else if (!musicOn_ && media_) {
        media_.reset();
        samplingActive_.reset();
        OnMedia(MediaSnapshot{}); // the entry and its card go with the setting
    }
    if (media_) {
        media_->SetFilter(s.nowPlayingShowPaused, s.nowPlayingAppMode, s.nowPlayingApps);
        UpdateSampling();
    }
    feed_->playing.store(musicOn_ && last_.active && last_.state == MediaPlaybackState::Playing, std::memory_order_relaxed);
    if (motionChanged || rhythmChanged)
        WakeMusic();
    const bool stacksChanged =
        !stacksCached_ || stacksOn_ != s.showFolderStacks || lastFolderStacks_ != s.folderStacks || lastFolderStackFiles_ != s.folderStackFiles;
    stacksCached_ = true;
    stacksOn_ = s.showFolderStacks;
    lastFolderStacks_ = s.folderStacks;
    lastFolderStackFiles_ = s.folderStackFiles;
    stacks_.SetOptions(s.folderStackLayout != L"column", size_t(s.folderStackMax));
    stacks_.SetPinned(s.folderStackPinned);
    if (stacksChanged)
        stackModel_.SetFolders(stacksOn_ ? s.folderStacks : std::vector<std::wstring>{}, stacksOn_ ? s.folderStackFiles : std::vector<std::wstring>{},
                               96); // read once at a size every scale draws from
    if (!stacksOn_)
        stacks_.Close();
    launchpadOn_ = s.showLaunchpad;
    launchpad_.SetTint(s.launchpadMaterial == L"tint");
    launchpad_.SetFullscreen(s.launchpadLayout == L"fullscreen");
    if (!launchpadOn_)
        launchpad_.Close(false);
    PrewarmLaunchpad();
    card_.SetRadius(radius);
    launchpad_.SetRadius(radius);
    stacks_.SetRadius(radius);
}

void ExtrasHost::ConfigureBar(Bar& bar)
{
    bar.SetMusic(feed_, musicShown_, MusicTip());
    bar.onExtraClick = [this, &bar](const std::wstring& key, RECT entry) { Click(bar, key, entry); };
    bar.onTuckChanged = [this] { UpdateSampling(); };
}

void ExtrasHost::OnBarRetiring(const Bar* bar)
{
    if (!bar || lastBar_ != bar)
        return;
    lastBar_ = nullptr;
    launchpad_.Close(false);
    card_.Close();
    stacks_.Close();
    UpdateSampling();
}

void ExtrasHost::UpdateSampling()
{
    if (!media_)
        return;
    const bool cardVisible = card_.Visible();
    media_->SetCardVisible(cardVisible);
    bool seen = cardVisible;
    forBars_([&](Bar& bar) { seen = seen || bar.Seen(); });
    const bool want = liveRhythm_ && seen;
    if (samplingActive_ == want)
        return;
    samplingActive_ = want;
    media_->SetSamplingActive(want);
}

void ExtrasHost::WakeMusic()
{
    forBars_([](Bar& b) { b.WakeRender(); });
    card_.WakeBars();
}

void ExtrasHost::Click(Bar& bar, const std::wstring& key, RECT entry)
{
    const bool sameBar = &bar == lastBar_;
    if (key == L"#launchpad") {
        if (sameBar && (launchpad_.Visible() || JustClosed(launchpad_, sameBar))) {
            launchpad_.Close(false);
            return;
        }
        lastBar_ = &bar;
        MONITORINFO mi{sizeof(mi)};
        RECT barRect = bar.BarRect();
        GetMonitorInfoW(MonitorFromRect(&barRect, MONITOR_DEFAULTTONEAREST), &mi);
        launchpad_.Open(mi.rcWork, bar.PreviousForeground());
    } else if (key == L"#music") {
        if (sameBar && (card_.Visible() || JustClosed(card_, sameBar))) {
            card_.Close();
            UpdateSampling();
            return;
        }
        lastBar_ = &bar;
        card_.Open(entry, bar.BarRect());
    } else if (key == L"#stacks") {
        if (sameBar && (stacks_.Visible() || JustClosed(stacks_, sameBar))) {
            stacks_.Close();
            return;
        }
        lastBar_ = &bar;
        stacks_.Open(entry, bar.BarRect());
    } else {
        return;
    }
    UpdateSampling();
    if (onPanelsChanged)
        onPanelsChanged();
}

void ExtrasHost::OnMedia(const MediaSnapshot& snapshot)
{
    const bool playing = snapshot.active && snapshot.state == MediaPlaybackState::Playing;
    if (!playing)
        feed_->peak.store(0.f);
    feed_->playing.store(playing);
    const bool shown = musicOn_ && snapshot.active;
    const bool changed = shown != musicShown_;
    musicShown_ = shown;
    last_ = snapshot;
    // Every bar learns whether the entry is there (a layout) and wakes for the bars' motion.
    const std::wstring tip = MusicTip();
    forBars_([&](Bar& bar) { bar.SetMusic(feed_, shown, tip); });
    if (changed)
        Log(L"now playing: entry %s", shown ? L"shown" : L"gone");
    card_.Update(snapshot);
    UpdateSampling();
}

std::wstring ExtrasHost::MusicTip() const
{
    if (last_.title.empty())
        return last_.appName;
    return last_.artist.empty() ? last_.title : last_.title + L" \u00b7 " + last_.artist;
}

void ExtrasHost::OnCatalogChanged(bool listChanged)
{
    launchpad_.Refresh(listChanged);
    if (listChanged)
        PrewarmLaunchpad();
}

// Every app's icon read ahead for the main display, so Launchpad opens with them all.
void ExtrasHost::PrewarmLaunchpad()
{
    if (!launchpadOn_)
        return;
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi))
        launchpad_.Prewarm(mi.rcWork);
}

void ExtrasHost::ClosePopups()
{
    lastBar_ = nullptr;
    launchpad_.Close(false);
    card_.Close();
    stacks_.Close();
    UpdateSampling();
}

bool ExtrasHost::AnyPopupVisible() const { return launchpad_.Visible() || card_.Visible() || stacks_.Visible(); }

void ExtrasHost::Shutdown()
{
    ClosePopups();
    media_.reset();
}

} // namespace atrium
