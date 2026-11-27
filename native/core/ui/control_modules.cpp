// The control centre's optional modules (controlModules): under the connections, two to a row (one
// alone in its row takes it whole), each a round switch with its name and state as macOS's tiles. Stage
// Manager's goes to the core; the system's (shell/quick_toggles) are read when the centre opens and
// flipped on the slow worker, shown flipped at once.
#include "ui/control_center.h"

#include "localization.h"
#include "log.h"
#include "shell/quick_toggles.h"
#include "shell/shell_launch.h"
#include "shell/system_chrome.h"
#include "ui/control_layout.h"

#include <algorithm>

namespace atrium {

namespace {

using control::kGap;
using control::kModuleH;
using control::kPad;
using control::kRowH;
using control::kTopH;
using control::kW;

void OpenControlUri(const wchar_t* uri)
{
    LaunchRequest req;
    req.target = uri;
    req.verb = L"open";
    req.kind = LaunchTargetKind::Uri;
    req.privilege = LaunchPrivilege::ShellDefault;
    ShellLaunchAsync(std::move(req));
}

struct ModuleSpec {
    const wchar_t* id;
    const wchar_t* glyph;
    const wchar_t* title;
};
constexpr ModuleSpec kSpecs[] = {
    {L"stage", L"\uE7C4", L"Stage Manager"},    {L"energysaver", L"\uEC0A", L"Energy saver"}, {L"darkmode", L"\uE708", L"Dark mode"},
    {L"hotspot", L"\uE88A", L"Mobile hotspot"}, {L"dnd", L"\uE7ED", L"Do Not Disturb"},       {L"airplane", L"\uE709", L"Airplane mode"},
};

const ModuleSpec* Spec(const std::wstring& id)
{
    const auto* const it = std::ranges::find_if(kSpecs, [&](const ModuleSpec& s) { return id == s.id; });
    return it == std::end(kSpecs) ? nullptr : &*it;
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// The system's switches; may block (worker thread).
std::optional<bool> ReadSwitch(const std::wstring& id)
{
    if (id == L"energysaver")
        return ReadEnergySaver();
    if (id == L"darkmode")
        return ReadDarkMode();
    if (id == L"hotspot")
        return ReadMobileHotspot();
    if (id == L"airplane")
        return ReadAirplaneMode();
    return std::nullopt;
}

bool SetSwitch(const std::wstring& id, bool on)
{
    if (id == L"energysaver")
        return SetEnergySaver(on);
    if (id == L"darkmode")
        return SetDarkMode(on);
    if (id == L"hotspot")
        return SetMobileHotspot(on);
    return id == L"airplane" && SetAirplaneMode(on);
}

std::wstring ModuleStateText(bool stage, bool wide, bool stageOn, bool stageRight, bool known, const std::optional<bool>& on)
{
    if (stage && wide)
        return stageOn ? (stageRight ? Tr(L"On · recent apps on the right") : Tr(L"On · recent apps on the left")) : Tr(L"Off");
    if (!known)
        return L"…";
    if (!on)
        return Tr(L"Unavailable");
    return *on ? Tr(L"On") : Tr(L"Off");
}

} // namespace

std::wstring WifiTileSubtitle(const SystemStatus& status, bool radiosRead, bool wifiPresent, bool wifiOn, bool privateMode)
{
    bool hasWired = status.links.empty() && status.connected && !status.wifi;
    bool hasWifi = status.links.empty() && status.connected && status.wifi;
    std::wstring wifiName = hasWifi ? status.network : L"";
    for (const auto& l : status.links) {
        if (l.kind == NetworkLink::Kind::Wifi) {
            hasWifi = true;
            wifiName = wifiName.empty() ? (l.network.empty() ? l.alias : l.network) : wifiName;
        } else
            hasWired = true;
    }
    if (radiosRead && (!wifiPresent || !wifiOn))
        return Tr(!wifiPresent ? (hasWired ? L"Ethernet" : L"No Wi-Fi adapter") : (hasWired ? L"Off · Ethernet" : L"Off"));
    std::wstring wifiLabel = privateMode ? std::wstring(Tr(L"Connected")) : std::move(wifiName);
    if (hasWired && hasWifi)
        return TrFormat(L"Ethernet · %s", wifiLabel.c_str());
    return hasWifi ? wifiLabel : Tr(hasWired ? L"Ethernet" : L"Not connected");
}

void ControlCenter::SetDoNotDisturb(bool on)
{
    if (dndOn_ == on)
        return;
    dndOn_ = on;
    moduleOn_[L"dnd"] = on;
    if (surface_.Visible())
        surface_.Invalidate();
}

void ControlCenter::SetModules(std::vector<std::wstring> ids)
{
    std::erase_if(ids, [](const std::wstring& id) { return !Spec(id); });
    if (ids == modules_)
        return;
    modules_ = std::move(ids);
    if (surface_.Visible())
        Close(); // its size changes: the next opening lays it out anew
}

float ControlCenter::ModulesHeight() const
{
    const size_t rows = (modules_.size() + 1) / 2;
    return float(rows) * (kModuleH + kGap);
}

std::vector<D2D1_RECT_F> ControlCenter::ModuleRects() const
{
    const float colW = (kW - kGap) / 2;
    std::vector<D2D1_RECT_F> rects;
    for (size_t i = 0; i < modules_.size(); ++i) {
        const size_t row = i / 2;
        const float top = kTopH + kGap + float(row) * (kModuleH + kGap);
        const bool alone = i + 1 == modules_.size() && i % 2 == 0;
        const float left = i % 2 ? colW + kGap : 0;
        rects.push_back(D2D1::RectF(left, top, alone ? kW : left + colW, top + kModuleH));
    }
    return rects;
}

void ControlCenter::PaintModules(Canvas& c, const Theme& t)
{
    const float modR = std::min(radius_, 22.f);
    const std::vector<D2D1_RECT_F> rects = ModuleRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const std::wstring& id = modules_[i];
        const ModuleSpec& spec = *Spec(id);
        const D2D1_RECT_F r = rects[i];
        const bool stage = id == L"stage", dnd = id == L"dnd", wide = r.right - r.left > kW / 2;
        const auto read = moduleOn_.find(id);
        const bool known = stage || dnd || read != moduleOn_.end();
        const std::optional<bool> on = stage                   ? std::optional<bool>(stageOn_) :
                                       read != moduleOn_.end() ? read->second :
                                       dnd                     ? std::optional<bool>(dndOn_) :
                                                                 std::nullopt;
        const bool live = stage || dnd || on.has_value(); // unknown yet or unavailable: nothing to press
        const std::wstring hit = stage ? std::wstring(L"stage") : L"mod:" + id;
        const float h = live ? Hover(c, hit, hover_ == hit) : 0;
        if (h > 0.001f || pressed_ == hit)
            c.Fill(r, modR, pressed_ == hit ? t.press : Alpha(t.hover, h));
        const float cx = r.left + 32, cy = (r.top + r.bottom) / 2;
        const bool lit = on.value_or(false);
        c.Circle(cx, cy, 18, lit ? t.accent : Alpha(t.text, 0.16f + 0.08f * h));
        const bool halo = c.halo;
        c.halo = false;
        c.Glyph(spec.glyph, D2D1::RectF(cx - 14, cy - 14, cx + 14, cy + 14), 14, lit ? D2D1::ColorF(1, 1, 1, 0.98f) : known && !on ? t.faint : t.text);
        c.halo = halo;
        const std::wstring state = ModuleStateText(stage, wide, stageOn_, stageRight_, known, on);
        c.Text(Tr(spec.title), D2D1::RectF(cx + 28, cy - 18, r.right - kPad, cy), 13.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(state, D2D1::RectF(cx + 28, cy, r.right - kPad, cy + 18), 11.5f, t.subtext);
        if (live)
            hits_.push_back({r, hit});
    }
}

void ControlCenter::ReadModules()
{
    std::vector<std::wstring> ids = modules_;
    std::erase(ids, L"stage");
    if (ids.empty())
        return;
    const bool dndLocal = dndOn_;
    const bool ours = quietIsOurs && quietIsOurs();
    slowWorker_.Post(L"modules", [this, ids = std::move(ids), dndLocal, ours] {
        std::map<std::wstring, std::optional<bool>> read;
        for (const std::wstring& id : ids) {
            if (id == L"dnd")
                read[id] = dndLocal ? true : (ours ? false : IsQuietProfileActive(QuietProfile()));
            else
                read[id] = ReadSwitch(id);
        }
        PostResult(WM_CC_MODULES, [&] { moduleSlot_ = std::move(read); });
    });
}

void ControlCenter::FlipModule(const std::wstring& id)
{
    if (id == L"dnd") {
        if (moduleBusy_.contains(id))
            return;
        const auto it = moduleOn_.find(id);
        const bool cur = it != moduleOn_.end() && it->second.has_value() ? *it->second : dndOn_;
        const bool want = !cur;
        dndOn_ = want;
        moduleOn_[id] = want;
        moduleBusy_.insert(id);
        surface_.Invalidate();
        if (onDnd)
            onDnd(want);
        const bool ours = quietIsOurs && quietIsOurs();
        slowWorker_.Post({}, [this, want, ours] {
            if (!ours && !SetQuietProfile(want ? kQuietProfile : kQuietProfileOff))
                Log(L"control: dnd could not be switched %s in Windows", want ? L"on" : L"off");
            PostResult(WM_CC_MODULES, [&] {
                if (!moduleSlot_)
                    moduleSlot_.emplace();
                (*moduleSlot_)[L"dnd"] = want;
            });
        });
        return;
    }
    const auto it = moduleOn_.find(id);
    if (it == moduleOn_.end() || !it->second || moduleBusy_.contains(id))
        return;
    const bool want = !*it->second;
    it->second = want; // shown flipped at once
    moduleBusy_.insert(id);
    surface_.Invalidate();
    slowWorker_.Post({}, [this, id, want] {
        const bool switched = SetSwitch(id, want);
        if (!switched)
            Log(L"control: %s could not be switched %s", id.c_str(), want ? L"on" : L"off");
        if (id == L"airplane" && !switched)
            OpenControlUri(L"ms-settings:network-airplanemode");
        std::optional<bool> now = ReadSwitch(id);
        // Plugged in, Windows keeps energy saver to its own switch (the threshold only counts on battery):
        // the threshold goes back and its settings page opens instead.
        if (id == L"energysaver" && want && now == false) {
            SetEnergySaver(false);
            OpenControlUri(L"ms-settings:batterysaver");
        }
        PostResult(WM_CC_MODULES, [&] {
            if (!moduleSlot_)
                moduleSlot_.emplace();
            (*moduleSlot_)[id] = now;
        });
    });
}

void ControlCenter::TakeModules()
{
    auto read = TakeResult(moduleSlot_);
    if (!read)
        return;
    bool airplaneChanged = false;
    for (auto& [id, on] : *read) {
        if (id == L"dnd" && on.has_value())
            dndOn_ = *on;
        if (id == L"airplane" && moduleBusy_.contains(id))
            airplaneChanged = true;
        moduleOn_[id] = on;
        moduleBusy_.erase(id);
    }
    if (airplaneChanged) {
        if (onChanged)
            onChanged();
        if (surface_.Visible())
            SetTimer(surface_.Hwnd(), kRadioRefreshTimer, 900, nullptr);
    }
    if (surface_.Visible())
        surface_.Invalidate();
}

bool ControlCenter::ActivateSubpage(const std::wstring& id)
{
    if (id.starts_with(L"disp:")) {
        size_t i = size_t(std::stoul(id.substr(5)));
        if (i < displays_.size() && connecting_.empty() && displays_[i].id != projecting_) {
            connecting_ = displays_[i].id;
            CastOp(connecting_);
            surface_.Invalidate();
        }
        return true;
    }
    if (id == L"castoff") {
        CastOp({});
        return true;
    }
    if (id.starts_with(L"mode:")) {
        const ProjectionMode modes[4] = {ProjectionMode::Internal, ProjectionMode::Clone, ProjectionMode::Extend, ProjectionMode::External};
        ProjectionMode m = modes[std::clamp(int(std::stoul(id.substr(5))), 0, 3)];
        Close();
        fastWorker_.Post(L"sm", [m] { SetProjectionMode(m); });
        return true;
    }
    if (id.starts_with(L"out:")) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i < outputs_.size() && !outputs_[i].isDefault) {
            for (size_t k = 0; k < outputs_.size(); ++k)
                outputs_[k].isDefault = k == i;
            outputIntent_ = outputs_[i].id;
            ++audioGen_;
            WriteOutput();
            surface_.Invalidate();
        }
        return true;
    }
    if (id.starts_with(L"amute:")) {
        size_t i = size_t(std::stoul(id.substr(6)));
        if (i < apps_.size()) {
            apps_[i].muted = !apps_[i].muted;
            ++audioGen_;
            fastWorker_.Post(L"am:" + apps_[i].key, [key = apps_[i].key, m = apps_[i].muted] { SetAppMute(key, m); });
            surface_.Invalidate();
        }
        return true;
    }
    if (id == L"locsettings") {
        Close();
        OpenControlUri(L"ms-settings:privacy-location");
        return true;
    }
    if (id.starts_with(L"disconnect:")) {
        size_t i = size_t(std::stoul(id.substr(11)));
        if (i < networks_.size() && networks_[i].connected)
            WifiOp(networks_[i], false, 1500);
        return true;
    }
    if (id.starts_with(L"net:")) {
        size_t i = size_t(std::stoul(id.substr(4)));
        if (i >= networks_.size() || networks_[i].connected)
            return true;
        WifiNetwork n = networks_[i];
        if (n.profile.empty()) {
            Close();
            OpenControlUri(L"ms-availablenetworks:");
        } else {
            WifiOp(n, true, 4000);
            scanning_ = true;
            surface_.Invalidate();
        }
        return true;
    }
    return false;
}

void ControlCenter::PaintWifiEmpty(Canvas& c, const Theme& t, float w, float& y)
{
    auto message = [&](const wchar_t* text) {
        c.Text(text, D2D1::RectF(kPad, y, w - kPad, y + kRowH), 13, t.subtext, DWRITE_FONT_WEIGHT_NORMAL, 1);
        y += kRowH;
    };
    if (radiosRead_ && !radios_.wifiOn)
        message(Tr(L"Wi-Fi is off"));
    else if (!scanned_)
        message(Tr(L"Searching for networks…"));
    else if (wifiStatus_ == WifiListStatus::AccessDenied) {
        message(Tr(L"Location access is needed to list Wi-Fi networks"));
        D2D1_RECT_F row = D2D1::RectF(kPad - 4, y + 2, w - kPad + 4, y + kRowH - 2);
        if (float hv = Hover(c, L"locsettings", hover_ == L"locsettings"); hv > 0.001f)
            c.Fill(row, (row.bottom - row.top) / 2, Alpha(pressed_ == L"locsettings" ? t.press : t.hover, hv));
        c.Text(Tr(L"Open location settings"), D2D1::RectF(row.left + 12, row.top, row.right - 12, row.bottom), 13, t.accent, DWRITE_FONT_WEIGHT_SEMI_BOLD, 1);
        hits_.push_back({row, L"locsettings"});
        y += kRowH;
    } else
        message(Tr(wifiStatus_ == WifiListStatus::Error ? L"Could not read Wi-Fi networks" : L"No Wi-Fi networks found"));
}

} // namespace atrium
