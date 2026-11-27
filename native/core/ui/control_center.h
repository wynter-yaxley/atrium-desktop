// The control centre, opened from the bar's status ring, after macOS's: small
// pieces of the bar's glass. Top left the connections (Wi-Fi and Bluetooth,
// each a round switch and its state); top right the battery and screen
// casting; below, sliders for the screen's brightness and the sound. The
// connections open their own page in place: the Wi-Fi networks in range
// (connecting to saved ones) or the paired Bluetooth devices; so do the sound
// (the output device and each app's volume, as on the Windows taskbar) and
// casting (wireless displays and the projection modes of Win+P). Drawn by
// Atrium; only the system's dialogs and settings pages are left to Windows.
#pragma once

#include "motion.h"
#include "shell/audio_devices.h"
#include "shell/projection.h"
#include "shell/radios.h"
#include "shell/system_status.h"
#include "shell/wifi.h"
#include "ui/surface.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace atrium {

struct RadioRead;
struct AudioRead;

std::wstring WifiTileSubtitle(const SystemStatus& status, bool radiosRead, bool wifiPresent, bool wifiOn, bool privateMode);

class ControlCenter {
public:
    explicit ControlCenter(HINSTANCE instance);
    ~ControlCenter();
    ControlCenter(const ControlCenter&) = delete;
    ControlCenter& operator=(const ControlCenter&) = delete;

    // anchor: the status ring on screen; bar: the bar's glass shape.
    void Open(RECT anchor, RECT bar);
    void Close();
    bool Visible() const { return surface_.Visible(); }
    ULONGLONG LastClosed() const { return lastClosed_; }
    void SetRadius(float dip)
    {
        radius_ = dip;
        surface_.SetRadius(dip);
    }
    void SetBarHeight(float dip) { barH_ = dip; }
    void SetStatus(const SystemStatus& status);
    // Windows reported the built-in screen's brightness (keys, quick settings).
    void BrightnessChanged(int percent);
    // Stage Manager's state, for its switch.
    void SetStage(bool on, bool onRight);
    void SetDoNotDisturb(bool on);
    // The optional modules under the connections (controlModules): Stage Manager's switch and the
    // system's (energy saver, dark mode, mobile hotspot, Do Not Disturb, airplane mode), two to a row.
    void SetModules(std::vector<std::wstring> ids);

    std::function<void()> onChanged; // volume, mute or the network changed: the bar's icon updates
    std::function<void(bool)> onStage; // Stage Manager's switch was pressed: on or off
    std::function<void(bool)> onDnd;   // Do Not Disturb switch was pressed: on or off
    std::function<bool()> quietIsOurs; // True when Windows DND is held by Atrium's hideBanners setting

private:
    enum class Page { Main, Wifi, Bluetooth, Sound, Cast };
    struct Hit {
        D2D1_RECT_F rect;
        std::wstring id;
    };
    static constexpr UINT_PTR kStatusTimer = 1, kRadioRefreshTimer = 2, kWifiAgainTimer = 3;
    static constexpr UINT WM_CC_MODULES = WM_APP + 0x5A; // the system's switches were read (moduleSlot_)
    static constexpr float kSliderH = 68, kHeader = 58, kFooter = 46, kSection = 30, kAppRowH = 50, kModeH = 78;
    static constexpr int kMaxRows = 6, kMaxOutputs = 4, kMaxApps = 5, kMaxDisplays = 4;
    static constexpr double kPageMs = 320;
    struct Worker {
        void Post(std::wstring key, std::function<void()> task)
        {
            std::lock_guard lk(lock);
            if (stop)
                return;
            if (!thread.joinable())
                thread = std::thread([this] {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    for (;;) {
                        std::function<void()> fn;
                        {
                            std::unique_lock qlk(lock);
                            cv.wait(qlk, [this] { return stop || !tasks.empty(); });
                            if (stop)
                                break;
                            fn = std::move(tasks.front().second);
                            tasks.pop_front();
                        }
                        fn();
                    }
                    winrt::uninit_apartment();
                });
            if (!key.empty())
                std::erase_if(tasks, [&key](const auto& e) { return e.first == key; });
            tasks.emplace_back(std::move(key), std::move(task));
            cv.notify_one();
        }
        void Stop()
        {
            std::unique_lock lk(lock);
            stop = true;
            lk.unlock();
            cv.notify_all();
            if (thread.joinable())
                thread.join();
        }
        std::mutex lock;
        std::condition_variable cv;
        std::deque<std::pair<std::wstring, std::function<void()>>> tasks;
        bool stop = false;
        std::thread thread;
    };

    void Paint(Canvas& c);
    std::vector<ShapeDip> BuildMorphShapes(D2D1_RECT_F card, float e) const;
    void PaintMain(Canvas& c, const Theme& t);
    void PaintMainConnections(Canvas& c, const Theme& t, D2D1_RECT_F conn, float modR);
    void PaintMainBattery(Canvas& c, const Theme& t, D2D1_RECT_F batt, float modR);
    void PaintMainCast(Canvas& c, const Theme& t, D2D1_RECT_F cast, float modR);
    void PaintWifi(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintWifiRow(Canvas& c, const Theme& t, const WifiNetwork& n, int idx, float w, float y);
    void PaintWifiEmpty(Canvas& c, const Theme& t, float w, float& y);
    void PaintBluetooth(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintSound(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintSoundOutputRow(Canvas& c, const Theme& t, const AudioOutput& o, int idx, float w, float y);
    void PaintSoundAppRow(Canvas& c, const Theme& t, const AudioApp& a, int idx, float w, float y);
    void PaintCast(Canvas& c, const Theme& t, D2D1_RECT_F card);
    void PaintCastDisplayRow(Canvas& c, const Theme& t, const WirelessDisplay& d, int idx, float w, float y);
    void PaintCastModes(Canvas& c, const Theme& t, float w, float y);
    void Link(Canvas& c, const Theme& t, const std::wstring& id, const std::wstring& label, float left, float right, float y, int align);
    void PaintSlider(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& id, const std::wstring& title, const std::wstring& glyph, int value,
                     bool dim, const std::wstring& right, D2D1_RECT_F& track, bool enabled = true);
    float PaintHeader(Canvas& c, const Theme& t, D2D1_RECT_F card, const std::wstring& title, bool on, const std::wstring& toggleId);
    void Switch(Canvas& c, const Theme& t, D2D1_RECT_F r, bool on, float hv);
    bool Message(UINT m, WPARAM w, LPARAM l, LRESULT& result);
    bool HandleWorkerMessage(UINT m, WPARAM w);
    bool HandleMouseMove(WPARAM w, POINT pt);
    bool HandleLeftDown(POINT pt);
    bool HandleLeftUp(POINT pt);
    bool HandleMouseWheel(WPARAM w, POINT pt);
    bool ReleasePointer(bool releaseCapture = true);
    std::wstring HitAt(POINT pt) const;
    void Activate(const std::wstring& id);
    void Scan(bool rescan);
    void ReadRadios(bool devices);
    void ReadAudio(bool icons);
    void ReadMode();
    void SetRadioAsync(bool bluetooth, bool on);
    void WriteRadios();
    void WriteOutput();
    void Place();
    void ShowPage(Page page);
    void SetSliderAt(const std::wstring& id, float x);
    void SetBrightnessAsync(int percent);
    bool SetVolumeIf(int v, bool unmute)
    {
        if (v == status_.volume && (!unmute || !status_.muted))
            return false;
        status_.volume = v;
        status_.muted = status_.muted && !unmute;
        fastWorker_.Post(L"vol", [v] { SetVolume(v); });
        return true;
    }
    void WifiOp(WifiNetwork n, bool connect, UINT delayMs)
    {
        KillTimer(surface_.Hwnd(), kWifiAgainTimer);
        wifiWorker_.Post({}, [this, n = std::move(n), connect, gen = scanGen_, delayMs] {
            connect ? ConnectWifi(n) : DisconnectWifi(&n.interfaceGuid);
            PostResult(WM_APP + 0x51, [] {}, gen, LPARAM(delayMs));
        });
    }
    void CastOp(std::wstring t)
    {
        slowWorker_.Post({}, [this, t = std::move(t), g = castGen_]() mutable {
            bool ok = t.empty() ? StopProjecting() : ProjectTo(t);
            PostResult(WM_APP + 0x56, [this, g, ok, &t] { castSlot_.emplace((WPARAM(g) << 1) | WPARAM(ok), std::move(t)); });
        });
    }
    void OnProjected();
    void OnRadiosResult();
    void OnAudioResult();
    void OnWifiList();
    bool ActivateSubpage(const std::wstring& id);
    bool OnAsync(UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_APP + 0x59 && (ProjectionMode(w) != mode_ || int(l) != monitors_)) {
            mode_ = ProjectionMode(w);
            monitors_ = int(l);
            surface_.Invalidate();
        } else if (m == WM_APP + 0x51 && unsigned(w) == scanGen_ && surface_.Visible()) {
            SetTimer(surface_.Hwnd(), kWifiAgainTimer, UINT(l), nullptr);
        } else if (m == WM_TIMER && (w == kRadioRefreshTimer || w == kWifiAgainTimer)) {
            KillTimer(surface_.Hwnd(), w);
            if (surface_.Visible()) {
                if (w == kRadioRefreshTimer)
                    ReadRadios(true);
                else if (onChanged)
                    onChanged();
                if (w == kWifiAgainTimer || std::exchange(wifiWriteDone_, false))
                    Scan(false);
            }
        } else if (m != WM_APP + 0x59 && m != WM_APP + 0x51) {
            return false;
        }
        return true;
    }
    void StopTimers()
    {
        if (HWND h = surface_.Hwnd())
            for (UINT_PTR t : {kStatusTimer, kRadioRefreshTimer, kWifiAgainTimer})
                KillTimer(h, t);
    }
    float WinH(float s) const { return surface_.Rect().bottom > surface_.Rect().top ? float(surface_.Rect().bottom - surface_.Rect().top) / s : Size().height; }
    template<class T> T TakeResult(T& slot)
    {
        std::lock_guard lk(resultLock_);
        return std::exchange(slot, {});
    }
    template<class F> void PostResult(UINT msg, F&& fill, WPARAM w = 0, LPARAM l = 0)
    {
        std::lock_guard lk(resultLock_);
        if (resultStop_)
            return;
        fill();
        PostMessageW(surface_.Hwnd(), msg, w, l);
    }
    void BumpGenerations()
    {
        ++radioGen_;
        ++audioGen_;
        ++scanGen_;
        ++castGen_;
        connecting_.clear();
    }
    float Hover(Canvas& c, const std::wstring& id, bool on);
    float MainHeight() const;
    float PageHeight(Page page) const;
    float HeightOf(Page page) const
    {
        float h = page == Page::Main ? MainHeight() : PageHeight(page);
        return winH_ > 0 && winH_ < h ? winH_ : h;
    }
    float MaxHeight() const;
    std::vector<ShapeDip> MainShapes() const; // the main page's pieces of glass, from its top
    // The modules (control_modules.cpp): their rows' height, their tiles (DIP, from the main page's top).
    float ModulesHeight() const;
    std::vector<D2D1_RECT_F> ModuleRects() const;
    void PaintModules(Canvas& c, const Theme& t);
    void ReadModules();                      // the system's switches, on the slow worker
    void FlipModule(const std::wstring& id); // a press on a system switch
    void TakeModules();                      // WM_CC_MODULES
    void PaintPage(Canvas& c, const Theme& t, Page page);
    D2D1_SIZE_F Size() const; // the window's: as tall as the tallest page, the glass standing on its bottom

    Surface surface_;
    SystemStatus status_;
    RadioState radios_;
    bool radiosRead_ = false;
    bool brightRead_ = false; // the brightness, once per opening (WMI is slow)
    std::vector<BluetoothDevice> devices_;
    bool devicesRead_ = false;
    int brightness_ = -2; // -1: not adjustable (no built-in screen), -2: not read yet
    // The display's slider: read, or (before the first reading) a laptop's built-in screen, so the
    // first opening is laid out at once instead of the sound's slider moving down when WMI answers.
    // The display's slider is always there; greyed out where the screen's brightness cannot be set
    // (most desktop monitors) or is not read yet.
    bool CanSetBrightness() const { return brightness_ >= 0; }
    std::vector<WifiNetwork> networks_;
    WifiListStatus wifiStatus_ = WifiListStatus::Ok;
    bool scanning_ = false, scanned_ = false;
    unsigned scanGen_ = 0, acceptedGen_ = 0;
    bool scanBusy_ = false, scanPending_ = false, scanRescan_ = false;
    bool radioBusy_ = false, radioPending_ = false, radioDevices_ = false;
    bool radioWriteBusy_ = false, wifiWriteDone_ = false;
    unsigned radioGen_ = 0;
    std::optional<bool> wifiIntent_, bluetoothIntent_;
    bool audioBusy_ = false, audioPending_ = false, audioIcons_ = false;
    bool outputWriteBusy_ = false;
    unsigned audioGen_ = 0;
    std::wstring outputIntent_;
    Page page_ = Page::Main;
    double pageMs_ = 0;
    int pageDir_ = 0;
    Page fromPage_ = Page::Main; // the page leaving while the new one comes in
    // The glass's height glides between pages (and when a page's rows change);
    // the window keeps its size, so nothing jumps while it does.
    float shownH_ = 0, fromH_ = 0, toH_ = 0;
    double heightMs_ = 0;
    float cardTop_ = 0; // DIP from the window's top: where the glass begins; the content hangs from it
    std::vector<Hit> hits_;
    std::wstring hover_, pressed_, dragging_;
    bool tracking_ = false;
    int downX_ = 0; // where a press on the volume's symbol began: moving off it drags the slider
    D2D1_RECT_F volumeTrack_{}, brightTrack_{}; // the sliders' tracks, DIP
    RECT anchor_{}, bar_{};
    float radius_ = 30, barH_ = 60;
    bool stageOn_ = false, stageRight_ = false;
    bool dndOn_ = false;
    std::vector<std::wstring> modules_{L"stage"};
    std::map<std::wstring, std::optional<bool>> moduleOn_; // the system's switches as last read (none: unavailable)
    std::unordered_set<std::wstring> moduleBusy_;          // switches on their way
    std::optional<std::map<std::wstring, std::optional<bool>>> moduleSlot_;
    ULONGLONG lastClosed_ = 0;
    bool private_ = false;
    std::map<std::wstring, motion::Follow> hoverAnim_;
    float winH_ = 0;
    int brightPending_ = -1; // a brightness the worker has yet to set
    bool brightBusy_ = false;
    // Sound output page.
    std::vector<AudioOutput> outputs_;
    std::vector<AudioApp> apps_;
    bool audioRead_ = false;
    std::unordered_set<std::wstring> iconTried_; // apps whose icon was asked for
    std::vector<D2D1_RECT_F> appTracks_;   // the apps' sliders, DIP
    // Cast page.
    DisplayFinder finder_;
    std::vector<WirelessDisplay> displays_;
    std::wstring projecting_, connecting_; // display ids
    unsigned castGen_ = 0;
    ProjectionMode mode_ = ProjectionMode::Unknown;
    int monitors_ = 1;
    std::mutex resultLock_;
    bool resultStop_ = false;
    std::unique_ptr<RadioRead> radioSlot_;
    std::unique_ptr<AudioRead> audioSlot_;
    std::optional<std::pair<unsigned, WifiListResult>> wifiListSlot_;
    std::optional<std::pair<WPARAM, std::wstring>> castSlot_;
    HANDLE stopEvent_ = nullptr;
    Worker fastWorker_, slowWorker_, wifiWorker_;
};

} // namespace atrium
