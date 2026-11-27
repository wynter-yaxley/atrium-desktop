// WeatherSource worker exit, WaitWeatherAsync, and notification tests. No network or real cache files are touched.
#include "shell/weather.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace {
constexpr UINT kWeatherMsg = WM_APP + 1;
std::atomic_bool g_notified{false};

LRESULT CALLBACK NotifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return msg == kWeatherMsg ? (g_notified = true, 0) : DefWindowProcW(hwnd, msg, wp, lp);
}

HWND CreateNotifyWindow()
{
    g_notified = false;
    return CreateWindowExW(0, L"AtriumWeatherTestNotify", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
}

struct FakeAsync {
    using AsyncStatus = winrt::Windows::Foundation::AsyncStatus;
    using Handler = std::function<void(const FakeAsync&, AsyncStatus)>;
    mutable std::mutex lock;
    mutable AsyncStatus status = AsyncStatus::Started;
    mutable Handler handler;
    mutable bool canceled = false;

    void Completed(Handler h) const
    {
        std::lock_guard g(lock);
        handler = std::move(h);
    }
    AsyncStatus Status() const
    {
        std::lock_guard g(lock);
        return status;
    }
    void Cancel() const
    {
        Handler h;
        {
            std::lock_guard g(lock);
            canceled = true;
            status = AsyncStatus::Canceled;
            h = handler;
        }
        if (h)
            h(*this, AsyncStatus::Canceled); // synchronous callback while caller must not hold wait lock
    }
    void Complete() const
    {
        Handler h;
        {
            std::lock_guard g(lock);
            status = AsyncStatus::Completed;
            h = handler;
        }
        if (h)
            h(*this, AsyncStatus::Completed);
    }
};
} // namespace

int main()
{
    using Clock = std::chrono::steady_clock;
    using Status = winrt::Windows::Foundation::AsyncStatus;
    auto ev = [] { return std::shared_ptr<void>(CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle); };
    int fail = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok && ++fail)
            std::fprintf(stderr, "FAIL: %s\n", what);
    };
    WNDCLASSW wc{.lpfnWndProc = NotifyProc, .lpszClassName = L"AtriumWeatherTestNotify"};
    RegisterClassW(&wc);
    auto ms = [](Clock::time_point t0) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); };

    // 1. Teardown signals quit, joins the worker cleanly, and never posts even if the fetcher returns true.
    HWND n1 = CreateNotifyWindow();
    auto entered = ev(), release = ev(), finished = ev();
    auto s1 = std::make_unique<atrium::WeatherSource>(n1, kWeatherMsg, [entered, release, finished](const std::atomic_bool& quit, atrium::Weather& out) {
        SetEvent(entered.get());
        while (!quit && WaitForSingleObject(release.get(), 10) == WAIT_TIMEOUT) {}
        out = {.ok = true, .city = L"CancelledCity", .temp = 10.0f};
        SetEvent(finished.get());
        return true;
    });
    s1->SetActive(true);
    check(WaitForSingleObject(entered.get(), 3000) == WAIT_OBJECT_0, "worker entered blocked fetcher");
    auto t0 = Clock::now();
    s1.reset();
    check(ms(t0) < 800, "destructor joins cleanly once quit interrupts the fetcher");
    check(WaitForSingleObject(finished.get(), 0) == WAIT_OBJECT_0, "worker finished before destructor returned");
    for (auto dl = Clock::now() + std::chrono::milliseconds(30); Clock::now() < dl;) {
        for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); DispatchMessageW(&msg)) {}
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    check(!g_notified, "cancelled worker does not post to live notify window");
    DestroyWindow(n1);

    // 2. Cooperative cancel joins well below the 250 ms detach threshold.
    std::atomic_bool step2{false};
    auto step1 = ev(), idle = ev();
    auto s2 = std::make_unique<atrium::WeatherSource>(nullptr, kWeatherMsg, [step1, idle, &step2](const std::atomic_bool& quit, atrium::Weather&) {
        SetEvent(step1.get());
        for (int i = 0; i < 500 && !quit; ++i)
            WaitForSingleObject(idle.get(), 10);
        if (!quit)
            step2 = true;
        return false;
    });
    s2->SetActive(true);
    check(WaitForSingleObject(step1.get(), 3000) == WAIT_OBJECT_0, "worker started step 1");
    t0 = Clock::now();
    s2.reset();
    check(ms(t0) < 200 && !step2, "cooperative quit joins < 200 ms without running step 2");

    // 3. Normal fetch notifies and updates Latest().
    HWND n3 = CreateNotifyWindow();
    atrium::WeatherSource s3(n3, kWeatherMsg, [](const std::atomic_bool&, atrium::Weather& out) {
        out = {.ok = true, .city = L"TestCity", .temp = 23.5f};
        return true;
    });
    s3.SetActive(true);
    for (auto dl = Clock::now() + std::chrono::seconds(3); !g_notified && Clock::now() < dl;) {
        for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); DispatchMessageW(&msg)) {}
        if (!g_notified)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    check(g_notified && s3.Latest().ok && s3.Latest().city == L"TestCity" && s3.Latest().temp == 23.5f, "notification and Latest() match");
    DestroyWindow(n3);

    // 4. WaitWeatherAsync: completion, timeout, and quit with synchronous Cancel()->Completed().
    struct FakeState {
        std::mutex lock;
        std::condition_variable wake;
        std::atomic_bool quit{false};
    };
    auto fs = std::make_shared<FakeState>();
    FakeAsync okOp;
    std::thread tOk([&] { okOp.Complete(); });
    check(atrium::WaitWeatherAsync(okOp, fs, fs->lock, fs->wake, fs->quit, std::chrono::seconds(1)) && !okOp.canceled,
          "WaitWeatherAsync returns true on completion");
    tOk.join();

    FakeAsync timeoutOp;
    bool timedOut = false;
    check(!atrium::WaitWeatherAsync(timeoutOp, fs, fs->lock, fs->wake, fs->quit, std::chrono::milliseconds(10), &timedOut) && timedOut && timeoutOp.canceled &&
              timeoutOp.Status() == Status::Canceled,
          "WaitWeatherAsync cancels and reports timeout");

    FakeAsync quitOp;
    std::thread tQuit([&] {
        std::lock_guard g(fs->lock);
        fs->quit.store(true);
        fs->wake.notify_all();
    });
    check(!atrium::WaitWeatherAsync(quitOp, fs, fs->lock, fs->wake, fs->quit, std::chrono::seconds(1)) && quitOp.canceled,
          "WaitWeatherAsync cancels and returns false on quit without deadlock");
    tQuit.join();
    return fail ? 1 : 0;
}
