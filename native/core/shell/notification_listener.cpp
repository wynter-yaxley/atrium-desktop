#include "shell/notification_listener.h"

#include "log.h"
#include "shell/messengers.h"
#include "shell/task_model.h"
#include "../posted.h"
#include "../win32_path.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

namespace atrium {

namespace wun = winrt::Windows::UI::Notifications;
namespace wunm = winrt::Windows::UI::Notifications::Management;

namespace {

const wchar_t* AccessName(wunm::UserNotificationListenerAccessStatus s)
{
    switch (s) {
    case wunm::UserNotificationListenerAccessStatus::Allowed: return L"Allowed";
    case wunm::UserNotificationListenerAccessStatus::Denied: return L"Denied";
    default: return L"Unspecified";
    }
}

wun::NotificationBinding FindBinding(const wun::NotificationVisual& visual)
{
    if (auto generic = visual.GetBinding(wun::KnownNotificationBindings::ToastGeneric()))
        return generic;
    auto bindings = visual.Bindings();
    return bindings.Size() > 0 ? bindings.GetAt(0) : nullptr;
}

// A toast's lines (title, then the body), each poll: senders update a toast in place. Reading its
// app (AppInfo, DisplayInfo: the app's resources) costs far more, so only a toast to post does.
std::wstring TextOf(const wun::UserNotification& un)
{
    try {
        std::wstring all;
        if (auto binding = FindBinding(un.Notification().Visual()))
            for (const auto& text : binding.GetTextElements())
                all += std::wstring(text.Text()) + L"\x1f";
        return all;
    } catch (const winrt::hresult_error&) {
        return {}; // dismissed or malformed while reading
    }
}

Notice ToNotice(const wun::UserNotification& un)
{
    Notice n;
    n.source = L"windows";
    n.externalId = un.Id();
    try {
        auto info = un.AppInfo();
        n.appId = std::wstring(info.AppUserModelId());
        n.app = StripInvisibleUiChars(info.DisplayInfo().DisplayName());
    } catch (const winrt::hresult_error&) {
        n.app.clear(); // unpackaged senders have no AppInfo
    }
    if (!n.appId.empty())
        n.action = L"app:" + n.appId;
    const bool isAutoPlay = FoldCase(n.appId).find(L"autoplay") != std::wstring::npos;
    try {
        if (auto binding = FindBinding(un.Notification().Visual())) {
            for (const auto& text : binding.GetTextElements()) {
                std::wstring s = StripInvisibleUiChars(text.Text());
                if (std::ranges::all_of(s, [](wchar_t c) { return std::iswspace(static_cast<wint_t>(c)) != 0; }))
                    continue;
                const bool hasDriveTag = s.size() >= 5 && s[s.size() - 4] == L'(' && iswalpha(s[s.size() - 3]) && s[s.size() - 2] == L':' && s.back() == L')';
                if (isAutoPlay || hasDriveTag)
                    s = SanitizeAutoPlayText(s);
                if (n.title.empty())
                    n.title = std::move(s);
                else
                    n.body += (n.body.empty() ? L"" : L"\n") + s;
            }
        }
    } catch (const winrt::hresult_error&) {
        return n; // dismissed mid-read: keep whatever text and metadata arrived
    }
    return n;
}

struct ForegroundApp {
    std::wstring exe, aumid;
    ULONGLONG tick = 0;
};

void RecordForegroundApp(std::vector<ForegroundApp>& recent, ULONGLONG nowTick)
{
    std::erase_if(recent, [nowTick](const ForegroundApp& f) { return nowTick - f.tick > 5000; });
    auto exeOf = [](HWND w) -> std::wstring {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        std::wstring exe = proc ? FoldCase(detail::TaskProcessImagePath(proc)) : L"";
        if (proc)
            CloseHandle(proc);
        return exe;
    };
    const HWND fg = GetForegroundWindow();
    std::wstring exe = fg ? exeOf(fg) : L"";
    if (exe.ends_with(L"\\applicationframehost.exe")) {
        if (HWND core = FindWindowExW(fg, nullptr, L"Windows.UI.Core.CoreWindow", nullptr))
            if (std::wstring coreExe = exeOf(core); !coreExe.empty())
                exe = std::move(coreExe);
    }
    if (exe.empty())
        return;
    const std::wstring aumid = FoldCase(detail::PackagedAumidForExe(exe));
    if (auto it = std::ranges::find_if(recent, [&](const ForegroundApp& f) { return f.exe == exe && f.aumid == aumid; }); it != recent.end())
        it->tick = nowTick;
    else
        recent.push_back({std::move(exe), aumid, nowTick});
}

bool SenderInFront(const SeenToast& toast, const std::vector<ForegroundApp>& recent)
{
    const Messenger m = MessengerOfNotice(L"windows", toast.app, toast.appId);
    const std::wstring appId = FoldCase(toast.appId), app = FoldCase(toast.app);
    return std::ranges::any_of(recent, [&](const ForegroundApp& fg) {
        const std::wstring file = fg.exe.substr(fg.exe.find_last_of(L"\\/") + 1);
        const std::wstring stem = (file.size() > 4 && file.ends_with(L".exe")) ? file.substr(0, file.size() - 4) : file;
        return (m != Messenger::None && MessengerOfProgram(fg.exe) == m) || (!fg.aumid.empty() && fg.aumid == appId) || (!appId.empty() && fg.exe == appId) ||
               (stem.size() >= 2 && (appId == stem || (!app.empty() && app == stem)));
    });
}

void PollOnce(const wunm::UserNotificationListener& listener, HWND notify, UINT msg, std::map<uint32_t, SeenToast>& seen, bool first, bool notifyAlways,
              std::vector<ForegroundApp>& recentForeground, int& posted)
{
    if (notifyAlways) // only kept cards need to know who was in front
        RecordForegroundApp(recentForeground, GetTickCount64());
    auto list = listener.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
    std::map<uint32_t, SeenToast> now;
    const auto clock = winrt::clock::now();
    for (const auto& un : list) {
        // Reading a toast's text parses its XML: every second for every toast in the
        // notification centre, that was most of Atrium's idle CPU. A toast is read when
        // it is new, and while it is recent (senders update theirs in place soon after).
        auto old = seen.find(un.Id());
        const bool recent = clock - un.CreationTime() < std::chrono::minutes(2);
        if (old != seen.end() && !recent) {
            now[un.Id()] = old->second;
            continue;
        }
        const size_t hash = first && !recent ? 0 : std::hash<std::wstring>{}(TextOf(un));
        SeenToast& entry = now[un.Id()];
        entry = {hash, old != seen.end() && old->second.posted, old != seen.end() ? old->second.app : L"", old != seen.end() ? old->second.appId : L""};
        if (first || (old != seen.end() && old->second.hash == hash))
            continue;
        Notice n = ToNotice(un);
        if (n.title.empty() && n.body.empty())
            continue;
        entry.app = n.app;
        entry.appId = n.appId;
        std::wstring app = n.app; // the UI thread owns the notice once posted
        if (!PostOwned(notify, msg, 0, std::make_unique<Notice>(std::move(n))))
            continue;
        entry.posted = true;
        ++posted;
        if (posted <= 3 || posted % 50 == 0)
            Log(L"listener: %s toast from %s (%d so far)", old == seen.end() ? L"new" : L"updated", app.c_str(), posted);
    }
    for (Notice& gone : DiffGoneToasts(seen, now, first, notifyAlways, [&](const SeenToast& t) { return SenderInFront(t, recentForeground); }))
        PostOwned(notify, msg, 0, std::make_unique<Notice>(std::move(gone)));
    seen = std::move(now);
}

} // namespace

std::vector<Notice> DiffGoneToasts(const std::map<uint32_t, SeenToast>& seen, const std::map<uint32_t, SeenToast>& now, bool first, bool notifyAlways,
                                   const std::function<bool(const SeenToast&)>& senderInFront)
{
    std::vector<Notice> out;
    if (first)
        return out;
    for (const auto& [id, toast] : seen)
        if (toast.posted && !now.contains(id) && (!notifyAlways || (senderInFront && senderInFront(toast))))
            out.push_back(Notice{.source = L"windows", .externalId = id, .unread = false});
    return out;
}

NotificationListener::NotificationListener(HWND notify, UINT msg) : notify_(notify), msg_(msg), wake_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    thread_ = std::thread([this] { Run(); });
}

NotificationListener::~NotificationListener()
{
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
}

void NotificationListener::Run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        auto listener = wunm::UserNotificationListener::Current();
        auto access = listener.GetAccessStatus();
        if (access != wunm::UserNotificationListenerAccessStatus::Allowed) {
            // Asking needs a user prompt, which belongs to the settings, not to a
            // background thread; the user grants it in Windows privacy settings.
            Log(L"listener: access %s, not mirroring Windows notifications", AccessName(access));
        }
        // NotificationChanged fails for unpackaged processes:
        // poll instead. The first pass only records what is already there;
        // later a toast is posted when it appears and again when its text
        // changes (senders update a toast in place).
        std::map<uint32_t, SeenToast> seen; // id -> hash, posted flag, sender metadata
        std::vector<ForegroundApp> recentForeground;
        bool first = true;
        int posted = 0;
        if (access == wunm::UserNotificationListenerAccessStatus::Allowed)
            Log(L"listener: mirroring Windows notifications (polling every second)");
        while (!stop_) {
            try {
                // The user can withdraw the access in Windows' privacy settings at any time.
                auto nowAccess = listener.GetAccessStatus();
                if (nowAccess != access) {
                    Log(L"listener: access %s", AccessName(nowAccess));
                    access = nowAccess;
                }
                if (access == wunm::UserNotificationListenerAccessStatus::Allowed) {
                    PollOnce(listener, notify_, msg_, seen, first, always_.load(std::memory_order_relaxed), recentForeground, posted);
                    first = false;
                } else {
                    seen.clear();
                    recentForeground.clear();
                    first = true; // access restored later: existing notifications form a new baseline
                }
            } catch (const winrt::hresult_error& e) {
                Log(L"listener: read failed 0x%08X", unsigned(e.code()));
            }
            WaitForSingleObject(wake_, access == wunm::UserNotificationListenerAccessStatus::Allowed ? 1000 : 10000);
        }
    } catch (const winrt::hresult_error& e) {
        Log(L"listener: unavailable 0x%08X", unsigned(e.code()));
    }
    winrt::uninit_apartment();
}

} // namespace atrium
