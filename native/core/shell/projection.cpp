#include "shell/projection.h"

#include "log.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>

namespace atrium {

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using winrt::Windows::UI::ViewManagement::ProjectionManager;

namespace {
struct ApartmentGuard {
    ApartmentGuard() { init_apartment(apartment_type::multi_threaded); }
    ~ApartmentGuard() { uninit_apartment(); }
    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;
};

template<class Async> bool AwaitAsync(const Async& op, const wchar_t* what)
{
    auto status = op.wait_for(std::chrono::seconds(8));
    if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
        return true;
    op.Cancel();
    Log(L"cast: %s %s (%d)", what, status == winrt::Windows::Foundation::AsyncStatus::Started ? L"timed out" : L"failed", int(status));
    return false;
}

struct DisplaySubscription {
    DeviceWatcher watcher{nullptr};
    event_token added{}, updated{}, removed{}, completed{}, stopped{};
    DisplaySubscription() = default;
    DisplaySubscription(const DisplaySubscription&) = delete;
    DisplaySubscription& operator=(const DisplaySubscription&) = delete;
    ~DisplaySubscription()
    {
        if (!watcher)
            return;
        // Revoke before Stop, outside State::lock: a callback may be finishing,
        // and the watcher may go away under any one step.
        auto step = [](auto&& f) {
            try {
                f();
            } catch (const winrt::hresult_error& error) {
                Log(L"cast: display watcher cleanup failed (0x%08X)", unsigned(error.code()));
            }
        };
        if (added.value)
            step([&] { watcher.Added(added); });
        if (updated.value)
            step([&] { watcher.Updated(updated); });
        if (removed.value)
            step([&] { watcher.Removed(removed); });
        if (completed.value)
            step([&] { watcher.EnumerationCompleted(completed); });
        if (stopped.value)
            step([&] { watcher.Stopped(stopped); });
        step([&] {
            auto status = watcher.Status();
            if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted)
                watcher.Stop();
        });
    }
};
} // namespace

struct DisplayFinder::State {
    mutable std::mutex lock;
    std::condition_variable cv;
    std::map<std::wstring, std::wstring> found; // id -> name
    HWND hwnd = nullptr;
    UINT msg = 0;
    bool searching = false;
    bool stop = false;
    unsigned gen = 0;
    void Notify(unsigned expected)
    {
        std::lock_guard g(lock);
        if (!stop && gen == expected && hwnd)
            PostMessageW(hwnd, msg, 0, 0);
    }
};

DisplayFinder::DisplayFinder() : state_(std::make_shared<State>()) {}

DisplayFinder::~DisplayFinder() { Stop(); }

void DisplayFinder::Start(HWND hwnd, UINT msg)
{
    Stop();
    unsigned gen;
    {
        std::lock_guard g(state_->lock);
        state_->found.clear();
        state_->hwnd = hwnd;
        state_->msg = msg;
        state_->searching = true;
        state_->stop = false;
        gen = ++state_->gen;
    }
    std::weak_ptr<State> weak = state_;
    worker_ = std::thread([weak, gen] {
        auto changed = [weak, gen](auto&& apply) {
            if (auto s = weak.lock()) {
                {
                    std::lock_guard g(s->lock);
                    if (s->gen != gen || s->stop)
                        return;
                    apply(*s);
                }
                s->Notify(gen);
            }
        };
        std::optional<ApartmentGuard> apartment;
        DisplaySubscription subscription;
        bool started = false;
        bool failed = false;
        try {
            apartment.emplace();
            auto w = DeviceInformation::CreateWatcher(ProjectionManager::GetDeviceSelector());
            subscription.watcher = w;
            subscription.added = w.Added([changed](const DeviceWatcher&, const DeviceInformation& d) {
                std::wstring id(d.Id()), name(d.Name());
                changed([&](State& s) { s.found[id] = name; });
            });
            subscription.updated = w.Updated([](const DeviceWatcher&, const DeviceInformationUpdate&) {}); // needed for the enumeration to complete
            subscription.removed = w.Removed([changed](const DeviceWatcher&, const DeviceInformationUpdate& u) {
                std::wstring id(u.Id());
                changed([&](State& s) { s.found.erase(id); });
            });
            subscription.completed = w.EnumerationCompleted(
                [changed](const DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) { changed([](State& s) { s.searching = false; }); });
            subscription.stopped =
                w.Stopped([changed](const DeviceWatcher&, const winrt::Windows::Foundation::IInspectable&) { changed([](State& s) { s.searching = false; }); });
            if (auto s = weak.lock()) {
                bool stopped = false;
                {
                    std::lock_guard g(s->lock);
                    stopped = s->stop || s->gen != gen;
                }
                if (!stopped) {
                    w.Start();
                    started = true;
                }
            }
        } catch (const winrt::hresult_error& error) {
            Log(L"cast: watching for wireless displays failed (0x%08X)", unsigned(error.code()));
            failed = true;
        }
        if (failed) {
            if (auto s = weak.lock()) {
                {
                    std::lock_guard g(s->lock);
                    if (s->gen != gen)
                        return;
                    s->searching = false;
                }
                s->Notify(gen);
            }
        } else if (started) {
            if (auto s = weak.lock()) {
                std::unique_lock lk(s->lock);
                s->cv.wait(lk, [&] { return s->stop || s->gen != gen; });
            }
        }
    });
}

void DisplayFinder::Stop()
{
    {
        std::lock_guard g(state_->lock);
        ++state_->gen;
        state_->stop = true;
        state_->hwnd = nullptr;
        state_->searching = false;
    }
    state_->cv.notify_all();
    if (worker_.joinable())
        worker_.join();
}

std::vector<WirelessDisplay> DisplayFinder::Displays() const
{
    std::lock_guard g(state_->lock);
    std::vector<WirelessDisplay> list;
    for (const auto& [id, name] : state_->found)
        list.push_back({id, name});
    std::ranges::sort(list, [](const WirelessDisplay& a, const WirelessDisplay& b) { return a.name < b.name; });
    return list;
}

bool DisplayFinder::Searching() const
{
    std::lock_guard g(state_->lock);
    return state_->searching;
}

bool ProjectTo(const std::wstring& id)
{
    bool ok = false;
    try {
        auto createOp = DeviceInformation::CreateFromIdAsync(id);
        if (!AwaitAsync(createOp, L"create"))
            return false;
        DeviceInformation d = createOp.GetResults();
        // No views of our own: Windows projects the desktop.
        auto projOp = ProjectionManager::StartProjectingAsync(0, 0, d);
        if (!AwaitAsync(projOp, L"projecting"))
            return false;
        projOp.GetResults();
        ok = true;
    } catch (const winrt::hresult_error& error) {
        Log(L"cast: projecting failed (0x%08X)", unsigned(error.code()));
        return false;
    }
    if (ok)
        Log(L"cast: projecting");
    return ok;
}

bool StopProjecting()
{
    bool ok = false;
    try {
        auto stopOp = ProjectionManager::StopProjectingAsync(0, 0);
        if (!AwaitAsync(stopOp, L"stopping"))
            return false;
        stopOp.GetResults();
        ok = true;
    } catch (const winrt::hresult_error& error) {
        Log(L"cast: stopping failed (0x%08X)", unsigned(error.code()));
        return false;
    }
    if (ok)
        Log(L"cast: stopped");
    return ok;
}

ProjectionMode CurrentProjectionMode(int* displays)
{
    ProjectionMode mode = ProjectionMode::Unknown;
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes) == ERROR_SUCCESS) {
        std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
        std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, p.data(), &modes, m.data(), &topology) == ERROR_SUCCESS) {
            switch (topology) {
            case DISPLAYCONFIG_TOPOLOGY_INTERNAL: mode = ProjectionMode::Internal; break;
            case DISPLAYCONFIG_TOPOLOGY_CLONE: mode = ProjectionMode::Clone; break;
            case DISPLAYCONFIG_TOPOLOGY_EXTEND: mode = ProjectionMode::Extend; break;
            case DISPLAYCONFIG_TOPOLOGY_EXTERNAL: mode = ProjectionMode::External; break;
            default: break;
            }
        }
    }
    if (displays) {
        // Connected targets, whether in use or not.
        std::set<std::tuple<LONG, DWORD, UINT32>> targets;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &paths, &modes) == ERROR_SUCCESS) {
            std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);
            std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
            if (QueryDisplayConfig(QDC_ALL_PATHS, &paths, p.data(), &modes, m.data(), nullptr) == ERROR_SUCCESS)
                for (UINT32 i = 0; i < paths; ++i)
                    if (p[i].targetInfo.targetAvailable)
                        targets.insert({p[i].targetInfo.adapterId.HighPart, p[i].targetInfo.adapterId.LowPart, p[i].targetInfo.id});
        }
        *displays = int(targets.size());
    }
    return mode;
}

bool SetProjectionMode(ProjectionMode mode)
{
    UINT32 flag = mode == ProjectionMode::Internal ? SDC_TOPOLOGY_INTERNAL :
                  mode == ProjectionMode::Clone    ? SDC_TOPOLOGY_CLONE :
                  mode == ProjectionMode::Extend   ? SDC_TOPOLOGY_EXTEND :
                  mode == ProjectionMode::External ? SDC_TOPOLOGY_EXTERNAL :
                                                     0;
    if (!flag)
        return false;
    LONG r = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flag);
    Log(L"cast: projection mode %d (%ld)", int(mode), r);
    return r == ERROR_SUCCESS;
}

} // namespace atrium
