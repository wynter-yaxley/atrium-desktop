#include "ui/control_center.h"

#include "clock.h"
#include "log.h"
#include "shell/shell_launch.h"
#include "ui/control_layout.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace atrium {

namespace {

using control::kGap;
using control::kRowH;
using control::kTopH;
using control::kW;
constexpr UINT WM_WIFI_LIST = WM_APP + 0x50;   // wParam generation, lParam WifiListResult*
constexpr UINT WM_CC_RADIOS = WM_APP + 0x52;   // lParam RadioRead*
constexpr UINT WM_CC_BRIGHT = WM_APP + 0x53;   // the brightness worker is done (wParam: the value it set last)
constexpr UINT WM_CC_AUDIO = WM_APP + 0x54;    // lParam AudioRead*
constexpr UINT WM_CC_DISPLAYS = WM_APP + 0x55; // the wireless displays in range changed
constexpr UINT WM_CC_PROJECTED = WM_APP + 0x56; // wParam (gen << 1) | ok, lParam std::wstring* the display (null: stopped)
constexpr UINT WM_CC_RADIO_SET = WM_APP + 0x57, WM_CC_OUTPUT_SET = WM_APP + 0x58, WM_CC_MODE = WM_APP + 0x59;
} // namespace

struct RadioRead {
    unsigned gen = 0;
    RadioState radios;
    bool devicesRead = false;
    std::vector<BluetoothDevice> devices;
    int brightness = -3; // -3: not read this time
};

struct AudioRead {
    unsigned gen = 0;
    std::vector<AudioOutput> outputs;
    std::vector<AudioApp> apps;
    bool icons = false;
};

namespace {

bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
void OpenUri(const wchar_t* uri)
{
    LaunchRequest req;
    req.target = uri;
    req.verb = L"open";
    req.kind = LaunchTargetKind::Uri;
    req.privilege = LaunchPrivilege::ShellDefault;
    ShellLaunchAsync(std::move(req));
}

// Win+K: the system's casting panel (a public shortcut, D11).
void SendWinK()
{
    INPUT in[4] = {};
    for (auto& i : in)
        i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[3].ki.wVk = VK_LWIN;
    in[1].ki.wVk = in[2].ki.wVk = 'K';
    in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

} // namespace

ControlCenter::ControlCenter(HINSTANCE instance) : surface_(instance, L"Atrium Control Center", true), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    surface_.paint = [this](Canvas& c) { Paint(c); };
    surface_.message = [this](UINT m, WPARAM w, LPARAM l, LRESULT& r) { return Message(m, w, l, r); };
    surface_.onDismiss = [this] { Close(); };
    wchar_t privacy[8] = {};
    private_ = GetEnvironmentVariableW(L"ATRIUM_PRIVACY", privacy, 8) && privacy[0] == L'1';
    ReadRadios(false); // the brightness (WMI is slow) before the first opening
}

ControlCenter::~ControlCenter()
{
    StopTimers();
    if (stopEvent_)
        SetEvent(stopEvent_);
    {
        std::lock_guard lk(resultLock_);
        resultStop_ = true;
    }
    fastWorker_.Stop();
    slowWorker_.Stop();
    wifiWorker_.Stop();
    if (stopEvent_)
        CloseHandle(stopEvent_);
}

float ControlCenter::MainHeight() const { return kTopH + kGap + ModulesHeight() + kSliderH + kGap + kSliderH; }

float ControlCenter::PageHeight(Page page) const
{
    if (page == Page::Sound) {
        int outs = std::clamp(int(outputs_.size()), 1, kMaxOutputs), apps = std::clamp(int(apps_.size()), 1, kMaxApps);
        return kHeader + float(outs) * kRowH + kSection + float(apps) * kAppRowH + kFooter + 6;
    }
    if (page == Page::Cast)
        return kHeader + float(std::clamp(int(displays_.size()), 1, kMaxDisplays)) * kRowH + kSection + kModeH + kFooter + 6;
    size_t n = page == Page::Wifi ? networks_.size() : devices_.size();
    bool off = page == Page::Wifi ? radiosRead_ && !radios_.wifiOn : !radios_.btOn;
    int rows = off ? 1 : std::clamp(int(n), 1, kMaxRows);
    if (page == Page::Wifi && !off && networks_.empty() && scanned_ && wifiStatus_ == WifiListStatus::AccessDenied)
        rows = 2;
    return kHeader + float(rows) * kRowH + kFooter + 6;
}

float ControlCenter::MaxHeight() const
{
    return std::max({MainHeight(), kHeader + float(kMaxRows) * kRowH + kFooter + 6,
                     kHeader + float(kMaxOutputs) * kRowH + kSection + float(kMaxApps) * kAppRowH + kFooter + 6,
                     kHeader + float(kMaxDisplays) * kRowH + kSection + kModeH + kFooter + 6});
}

D2D1_SIZE_F ControlCenter::Size() const { return D2D1::SizeF(kW, MaxHeight()); }

std::vector<ShapeDip> ControlCenter::MainShapes() const
{
    const float modR = std::min(radius_, 22.f), colW = (kW - kGap) / 2;
    std::vector<ShapeDip> s;
    s.push_back({D2D1::RectF(0, 0, colW, kTopH), modR});
    s.push_back({D2D1::RectF(colW + kGap, 0, kW, (kTopH - kGap) / 2), modR});
    s.push_back({D2D1::RectF(colW + kGap, (kTopH - kGap) / 2 + kGap, kW, kTopH), modR});
    for (const D2D1_RECT_F& r : ModuleRects())
        s.push_back({r, modR});
    float y = kTopH + kGap + ModulesHeight();
    s.push_back({D2D1::RectF(0, y, kW, y + kSliderH), modR});
    y += kSliderH + kGap;
    s.push_back({D2D1::RectF(0, y, kW, y + kSliderH), modR});
    return s;
}

void ControlCenter::Place()
{
    RECT r = PopupRect(bar_, Size(), PopupAlign::Right);
    if (surface_.Visible())
        surface_.Move(r);
    else
        surface_.Show(r, true, PopupOrigin(bar_, r, anchor_));
}

void ControlCenter::Open(RECT anchor, RECT bar)
{
    BumpGenerations();
    anchor_ = anchor;
    bar_ = bar;
    hover_.clear();
    pressed_.clear();
    dragging_.clear();
    hoverAnim_.clear();
    page_ = fromPage_ = Page::Main;
    pageMs_ = ClockMs();
    pageDir_ = 0;
    shownH_ = fromH_ = toH_ = MainHeight();
    heightMs_ = 0;
    surface_.SetRegion({});
    brightRead_ = false; // changed meanwhile with the keyboard or in Settings
    wifiWriteDone_ = false;
    StopTimers();
    if (onChanged)
        onChanged();
    Place();
    ReadRadios(true);
    ReadModules();
    ReadAudio(true); // ready for the sound's page
    Scan(false); // what the adapter already knows, ready for the Wi-Fi page
    SetTimer(surface_.Hwnd(), kStatusTimer, 1500, nullptr);
    Log(L"control: opened");
}

bool ControlCenter::ReleasePointer(bool releaseCapture)
{
    const bool had = !dragging_.empty() || !pressed_.empty();
    dragging_.clear();
    pressed_.clear();
    if (releaseCapture && GetCapture() == surface_.Hwnd())
        ReleaseCapture();
    return had;
}

void ControlCenter::Close()
{
    ReleasePointer();
    if (!surface_.Visible())
        return;
    StopTimers();
    BumpGenerations();
    radioPending_ = audioPending_ = scanPending_ = false;
    radioDevices_ = audioIcons_ = scanRescan_ = false;
    finder_.Stop();
    surface_.Hide();
    lastClosed_ = GetTickCount64();
}

void ControlCenter::SetStatus(const SystemStatus& status)
{
    if (dragging_.empty() && !(status_ == status)) {
        status_ = status;
        surface_.Invalidate();
    }
}

void ControlCenter::ShowPage(Page page)
{
    if (page == page_)
        return;
    if (page_ == Page::Cast)
        finder_.Stop();
    pageDir_ = page == Page::Main ? -1 : 1;
    fromPage_ = page_;
    page_ = page;
    pageMs_ = ClockMs();
    hover_.clear();
    Place();
    surface_.Invalidate();
}

void ControlCenter::Scan(bool rescan)
{
    scanning_ = true;
    scanRescan_ |= rescan;
    if (scanBusy_) {
        scanPending_ = true;
        return;
    }
    scanBusy_ = true;
    scanPending_ = false;
    rescan = std::exchange(scanRescan_, false);
    unsigned gen = ++scanGen_;
    wifiWorker_.Post(L"scan", [this, rescan, gen] {
        WifiListResult res = ListWifi(rescan, stopEvent_);
        PostResult(WM_WIFI_LIST, [this, gen, &res] { wifiListSlot_ = {gen, std::move(res)}; });
    });
}

void ControlCenter::ReadRadios(bool devices)
{
    radioDevices_ |= devices;
    if (radioBusy_ || radioWriteBusy_) {
        radioPending_ = true;
        return;
    }
    radioBusy_ = true;
    radioPending_ = false;
    devices = std::exchange(radioDevices_, false);
    unsigned gen = radioGen_;
    bool bright = !brightRead_;
    slowWorker_.Post(L"radios", [this, devices, bright, gen] {
        auto r = std::make_unique<RadioRead>();
        r->gen = gen;
        r->radios = QueryRadios();
        if (devices && r->radios.btOn) {
            r->devices = ListBluetooth();
            r->devicesRead = true;
        }
        if (bright)
            r->brightness = GetBrightness();
        PostResult(WM_CC_RADIOS, [this, &r] { radioSlot_ = std::move(r); });
    });
}

void ControlCenter::SetRadioAsync(bool bluetooth, bool on)
{
    // Shown at once; the next reading confirms it (or puts it back).
    (bluetooth ? radios_.btOn : radios_.wifiOn) = on;
    surface_.Invalidate();
    (bluetooth ? bluetoothIntent_ : wifiIntent_) = on;
    ++radioGen_; // an older read cannot undo the optimistic switch
    KillTimer(surface_.Hwnd(), kRadioRefreshTimer);
    WriteRadios();
}

void ControlCenter::WriteRadios()
{
    if (radioWriteBusy_ || (!wifiIntent_ && !bluetoothIntent_))
        return;
    radioWriteBusy_ = true;
    auto wifi = std::exchange(wifiIntent_, std::nullopt);
    auto bluetooth = std::exchange(bluetoothIntent_, std::nullopt);
    // Reading WMI or enumerating devices may be slow. It never holds up the
    // ordered writes; their latest intent has its own single bounded worker.
    slowWorker_.Post({}, [this, wifi, bluetooth] {
        if (wifi)
            SetRadio(false, *wifi);
        if (bluetooth)
            SetRadio(true, *bluetooth);
        PostResult(WM_CC_RADIO_SET, [] {}, wifi.has_value() ? 1 : 0);
    });
}

void ControlCenter::ReadAudio(bool icons)
{
    audioIcons_ |= icons;
    if (audioBusy_ || outputWriteBusy_) {
        audioPending_ = true;
        return;
    }
    audioBusy_ = true;
    audioPending_ = false;
    icons = std::exchange(audioIcons_, false);
    unsigned gen = audioGen_;
    int px = int(std::lround(26 * surface_.Scale()));
    slowWorker_.Post(L"audio", [this, px, icons, gen] {
        auto r = std::make_unique<AudioRead>();
        r->gen = gen;
        r->outputs = ListAudioOutputs();
        r->apps = ListAudioApps(icons ? px : 0);
        r->icons = icons;
        PostResult(WM_CC_AUDIO, [this, &r] { audioSlot_ = std::move(r); });
    });
}

void ControlCenter::WriteOutput()
{
    if (outputWriteBusy_ || outputIntent_.empty())
        return;
    outputWriteBusy_ = true;
    auto output = std::exchange(outputIntent_, {});
    fastWorker_.Post(L"out", [this, output] {
        SetDefaultOutput(output);
        PostResult(WM_CC_OUTPUT_SET, [] {});
    });
}

void ControlCenter::ReadMode()
{
    fastWorker_.Post(L"mode", [this] {
        int mon = 1;
        ProjectionMode m = CurrentProjectionMode(&mon);
        PostResult(WM_CC_MODE, [] {}, WPARAM(m), LPARAM(mon));
    });
}

std::wstring ControlCenter::HitAt(POINT pt) const
{
    // The hits are the page's own coordinates: it hangs from the glass's top.
    float s = surface_.Scale(), x = float(pt.x) / s, y = float(pt.y) / s - cardTop_;
    // Small controls first: they sit on larger rows.
    for (const Hit& h : hits_)
        if ((h.id.starts_with(L"disconnect:") || h.id == L"mute" || h.id == L"wifi" || h.id == L"bt" || h.id == L"rescan" || h.id == L"castoff" ||
             h.id == L"castscan" || h.id.starts_with(L"amute:")) &&
            In(h.rect, x, y))
            return h.id;
    for (const Hit& h : hits_)
        if (In(h.rect, x, y))
            return h.id;
    return {};
}

void ControlCenter::SetSliderAt(const std::wstring& id, float x)
{
    if (id.starts_with(L"app:")) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i >= apps_.size() || i >= appTracks_.size() || appTracks_[i].right <= appTracks_[i].left)
            return;
        const D2D1_RECT_F& tr = appTracks_[i];
        int v = int(std::lround(std::clamp((x - tr.left) / (tr.right - tr.left), 0.f, 1.f) * 100));
        if (v == apps_[i].volume && (!apps_[i].muted || v == 0))
            return;
        apps_[i].volume = v;
        ++audioGen_;
        if (v > 0)
            apps_[i].muted = false;
        fastWorker_.Post(L"av:" + apps_[i].key, [key = apps_[i].key, v] { SetAppVolume(key, v); });
        surface_.Invalidate();
        return;
    }
    const D2D1_RECT_F& tr = id == L"bright" ? brightTrack_ : volumeTrack_;
    if (tr.right <= tr.left)
        return;
    int v = int(std::lround(std::clamp((x - tr.left) / (tr.right - tr.left), 0.f, 1.f) * 100));
    if (id == L"bright") {
        if (v == brightness_)
            return;
        SetBrightnessAsync(v);
    } else if (!SetVolumeIf(v, v > 0)) {
        return;
    }
    surface_.Invalidate();
}

void ControlCenter::SetBrightnessAsync(int percent)
{
    int v = std::clamp(percent, 0, 100);
    brightness_ = brightPending_ = v;
    if (!brightBusy_) {
        // Dragging and the wheel share one worker, which follows the latest value.
        brightBusy_ = true;
        fastWorker_.Post(L"bright", [this, v] {
            SetBrightness(v);
            PostResult(WM_CC_BRIGHT, [] {}, WPARAM(v));
        });
    }
}

void ControlCenter::Activate(const std::wstring& id)
{
    if (ActivateSubpage(id))
        return;
    if (id == L"wifi") {
        if (radios_.wifiPresent)
            SetRadioAsync(false, !radios_.wifiOn);
    } else if (id == L"bt") {
        if (radios_.btPresent)
            SetRadioAsync(true, !radios_.btOn);
    } else if (id == L"wifi-page") {
        ShowPage(Page::Wifi);
        Scan(true);
    } else if (id == L"bt-page") {
        ShowPage(Page::Bluetooth);
        ReadRadios(true);
    } else if (id == L"back") {
        ShowPage(Page::Main);
    } else if (id == L"rescan") {
        Scan(true);
        surface_.Invalidate();
    } else if (id == L"mute") {
        if (status_.audio) {
            status_.muted = !status_.muted;
            fastWorker_.Post({}, [] { ToggleMute(); });
        }
        surface_.Invalidate();
        if (onChanged)
            onChanged();
    } else if (id.starts_with(L"mod:")) {
        FlipModule(id.substr(4));
    } else if (id == L"stage") {
        stageOn_ = !stageOn_;
        surface_.Invalidate();
        if (onStage)
            onStage(stageOn_);
    } else if (id == L"cast") {
        ShowPage(Page::Cast);
        ReadMode();
        finder_.Start(surface_.Hwnd(), WM_CC_DISPLAYS);
    } else if (id == L"castscan") {
        finder_.Start(surface_.Hwnd(), WM_CC_DISPLAYS);
        surface_.Invalidate();
    } else if (id == L"castproject") {
        Close();
        OpenUri(L"ms-settings:project");
    } else if (id == L"castpanel") {
        Close();
        SendWinK();
    } else if (id == L"sound-page") {
        ShowPage(Page::Sound);
        ReadAudio(true);
    } else if (id == L"soundsettings") {
        Close();
        OpenUri(L"ms-settings:sound");
    } else if (id == L"mixer") {
        Close();
        OpenUri(L"ms-settings:apps-volume");
    } else if (id == L"netsettings") {
        Close();
        OpenUri(L"ms-settings:network");
    } else if (id == L"netlist") {
        Close();
        OpenUri(L"ms-availablenetworks:");
    } else if (id == L"btsettings" || id.starts_with(L"bt:")) {
        Close();
        OpenUri(L"ms-settings:bluetooth");
    } else if (id == L"battery") {
        Close();
        OpenUri(status_.battery ? L"ms-settings:batterysaver" : L"ms-settings:powersleep");
    }
    Log(L"control: %s", id.starts_with(L"net:") ? L"connect" : id.starts_with(L"bt:") ? L"device" : id.starts_with(L"disp:") ? L"project" : id.c_str());
}

void ControlCenter::SetStage(bool on, bool onRight)
{
    if (on == stageOn_ && onRight == stageRight_)
        return;
    stageOn_ = on;
    stageRight_ = onRight;
    if (surface_.Visible())
        surface_.Invalidate();
}

void ControlCenter::BrightnessChanged(int percent)
{
    // Not while the slider's own value is on its way, nor where WMI cannot set it.
    if (dragging_ == L"bright" || brightBusy_ || percent == brightness_ || brightness_ == -1)
        return;
    brightness_ = percent;
    if (!surface_.Visible())
        return;
    surface_.Invalidate();
}

void ControlCenter::OnProjected()
{
    auto r = TakeResult(castSlot_);
    if (!r || unsigned(r->first >> 1) != castGen_)
        return;
    connecting_.clear();
    if (r->first & 1) {
        projecting_ = std::move(r->second);
    } else if (surface_.Visible()) {
        // Windows refused (or the display wants a PIN): its own panel takes over.
        Close();
        SendWinK();
        return;
    }
    surface_.Invalidate();
}

void ControlCenter::OnRadiosResult()
{
    auto r = TakeResult(radioSlot_);
    radioBusy_ = false;
    if (!r || r->gen != radioGen_ || radioWriteBusy_) {
        if (radioPending_ && !radioWriteBusy_)
            ReadRadios(radioDevices_);
        return;
    }
    radios_ = r->radios;
    radiosRead_ = true;
    if (r->devicesRead) {
        devices_ = std::move(r->devices);
        devicesRead_ = true;
    }
    if (r->brightness != -3) {
        brightRead_ = true;
        if (dragging_ != L"bright" && !brightBusy_)
            brightness_ = r->brightness;
    }
    surface_.Invalidate();
    if (radioPending_ && !radioWriteBusy_)
        ReadRadios(radioDevices_);
}

void ControlCenter::OnAudioResult()
{
    auto r = TakeResult(audioSlot_);
    audioBusy_ = false;
    if (audioPending_ && !outputWriteBusy_)
        ReadAudio(audioIcons_);
    if (!r || r->gen != audioGen_ || outputWriteBusy_ || !surface_.Visible())
        return;
    if (!dragging_.empty() && dragging_ != L"slider")
        return; // an app's slider is held: keep what it shows
    bool missing = false;
    for (AudioApp& a : r->apps) {
        if (!a.icon.Empty())
            continue;
        auto old = std::ranges::find_if(apps_, [&](const AudioApp& o) { return o.key == a.key; });
        if (old != apps_.end() && !old->icon.Empty())
            a.icon = old->icon;
        else if (!r->icons && !a.path.empty() && !iconTried_.contains(a.key))
            missing = true;
        if (r->icons && iconTried_.size() > 256)
            iconTried_.clear();
        if (r->icons)
            iconTried_.insert(a.key);
    }
    outputs_ = std::move(r->outputs);
    apps_ = std::move(r->apps);
    audioRead_ = true;
    if (missing)
        ReadAudio(true);
    surface_.Invalidate();
}

void ControlCenter::OnWifiList()
{
    auto r = TakeResult(wifiListSlot_);
    scanBusy_ = false;
    bool again = scanPending_;
    if (again && surface_.Visible())
        Scan(scanRescan_);
    if (r && r->first > acceptedGen_ && surface_.Visible() && (r->first == scanGen_ || !r->second.networks.empty())) {
        acceptedGen_ = r->first;
        wifiStatus_ = r->second.status;
        networks_ = std::move(r->second.networks);
        scanned_ = true;
        scanning_ = again || r->first != scanGen_;
        surface_.Invalidate();
    }
}
bool ControlCenter::HandleWorkerMessage(UINT m, WPARAM w)
{
    switch (m) {
    case WM_CC_MODULES: TakeModules(); return true;
    case WM_CC_RADIO_SET:
        radioWriteBusy_ = false, wifiWriteDone_ |= w != 0;
        if (wifiIntent_ || bluetoothIntent_)
            WriteRadios();
        else if (surface_.Visible())
            SetTimer(surface_.Hwnd(), kRadioRefreshTimer, wifiWriteDone_ ? 1500 : 900, nullptr);
        else
            wifiWriteDone_ = false;
        return true;
    case WM_CC_OUTPUT_SET:
        outputWriteBusy_ = false;
        if (!outputIntent_.empty())
            WriteOutput();
        else {
            if (onChanged)
                onChanged();
            if (surface_.Visible())
                ReadAudio(true);
        }
        return true;
    case WM_CC_RADIOS: OnRadiosResult(); return true;
    case WM_CC_AUDIO: OnAudioResult(); return true;
    case WM_CC_DISPLAYS: displays_ = finder_.Displays(), surface_.Invalidate(); return true;
    case WM_CC_PROJECTED: OnProjected(); return true;
    case WM_CC_BRIGHT:
        brightBusy_ = false;
        if (brightPending_ >= 0 && brightPending_ != int(w))
            SetBrightnessAsync(brightPending_);
        else
            brightPending_ = -1;
        return true;
    case WM_WIFI_LIST: OnWifiList(); return true;
    default: return false;
    }
}

bool ControlCenter::HandleMouseMove(WPARAM w, POINT pt)
{
    if (!tracking_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, surface_.Hwnd(), 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }
    if (dragging_.empty() && pressed_ == L"mute" && status_.audio && (w & MK_LBUTTON) && std::abs(pt.x - downX_) > int(4 * surface_.Scale()))
        pressed_ = dragging_ = L"slider";
    if (!dragging_.empty() && (w & MK_LBUTTON)) {
        SetSliderAt(dragging_, float(pt.x) / surface_.Scale());
        return true;
    }
    if (!(w & MK_LBUTTON) && ReleasePointer())
        surface_.Invalidate();
    if (std::wstring h = HitAt(pt); h != hover_)
        hover_ = std::move(h), surface_.Invalidate();
    return true;
}

bool ControlCenter::HandleLeftDown(POINT pt)
{
    if (const float y = float(pt.y) / surface_.Scale(); y < cardTop_ || y >= cardTop_ + shownH_)
        return Close(), true;
    pressed_ = HitAt(pt);
    if ((pressed_ == L"slider" && status_.audio) || (pressed_ == L"bright" && CanSetBrightness()) || pressed_.starts_with(L"app:"))
        dragging_ = pressed_, SetCapture(surface_.Hwnd()), SetSliderAt(dragging_, float(pt.x) / surface_.Scale());
    else if (pressed_ == L"mute")
        downX_ = pt.x, SetCapture(surface_.Hwnd());
    else if (!pressed_.empty())
        surface_.Ripple(float(pt.x) / surface_.Scale(), float(pt.y) / surface_.Scale());
    surface_.Invalidate();
    return true;
}

bool ControlCenter::HandleLeftUp(POINT pt)
{
    const bool volume = dragging_ == L"slider", same = dragging_.empty() && !pressed_.empty() && HitAt(pt) == pressed_;
    const std::wstring h = pressed_;
    ReleasePointer();
    surface_.Invalidate();
    if (volume && onChanged)
        onChanged();
    else if (same)
        Activate(h);
    return true;
}

bool ControlCenter::HandleMouseWheel(WPARAM w, POINT pt)
{
    const int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
    if (!notches || page_ != Page::Main)
        return true;
    const std::wstring hit = HitAt(surface_.ScreenToContent(pt));
    if (hit == L"bright" && brightness_ >= 0) {
        if (const int v = std::clamp(brightness_ + 2 * notches, 0, 100); v != brightness_)
            SetBrightnessAsync(v), surface_.Invalidate();
    } else if (status_.audio && (hit == L"slider" || hit == L"mute" || hit == L"sound-page") &&
               SetVolumeIf(std::clamp(status_.volume + 2 * notches, 0, 100), notches > 0)) {
        surface_.Invalidate();
        if (onChanged)
            onChanged();
    }
    return true;
}

bool ControlCenter::Message(UINT m, WPARAM w, LPARAM l, LRESULT& /*result*/)
{
    if (HandleWorkerMessage(m, w))
        return true;
    const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    switch (m) {
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && surface_.Visible())
            Close();
        return false;
    case WM_TIMER:
        if (w != kStatusTimer)
            return OnAsync(m, w, l);
        if (onChanged)
            onChanged();
        ReadRadios(page_ == Page::Bluetooth);
        if (page_ == Page::Sound && dragging_.empty())
            ReadAudio(false);
        if (page_ == Page::Cast)
            ReadMode();
        return true;
    case WM_MOUSEMOVE: return HandleMouseMove(w, pt);
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (dragging_.empty())
            hover_.clear(), pressed_.clear();
        surface_.Invalidate();
        return true;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(l) != surface_.Hwnd() && ReleasePointer(false))
            surface_.Invalidate();
        return true;
    case WM_LBUTTONDOWN: return HandleLeftDown(pt);
    case WM_LBUTTONUP: return HandleLeftUp(pt);
    case WM_MOUSEWHEEL: return HandleMouseWheel(w, pt);
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || (w == VK_BACK && page_ != Page::Main))
            return (page_ != Page::Main ? ShowPage(Page::Main) : Close()), true;
        return false;
    default: return OnAsync(m, w, l);
    }
}

} // namespace atrium
