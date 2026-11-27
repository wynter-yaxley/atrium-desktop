// Background icon and display-name worker for TaskModel.

#include "shell/task_model.h"
#include "shell/app_catalog.h"
#include "shell/shell_icon.h"
#include "shell/task_rules.h"

#include "log.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <winrt/base.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace atrium::detail {

namespace {

constexpr ULONGLONG kRetryFailedIconMs = 10000;
constexpr size_t kMaxIconCacheBytes = 16 * 1024 * 1024;
constexpr size_t kMaxIconCacheEntries = 512;

struct ComScope {
    bool owned;
    explicit ComScope(DWORD flags = COINIT_APARTMENTTHREADED) : owned(SUCCEEDED(CoInitializeEx(nullptr, flags))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

struct Job {
    bool name = false; // false: icon
    std::wstring key;
    int size = 0;
    std::wstring sig, aumid, pinPath, exePath;
    std::vector<HWND> hwnds;
};

struct IconEntry {
    bool requested = false, loading = false, have = false;
    std::wstring sig;
    ULONGLONG failedAt = 0;
    uint64_t used = 0;
    std::optional<TaskIconSource> lastSource;
    IconImage image;
};

struct IconKey {
    std::wstring key;
    int size = 0;
    bool operator==(const IconKey&) const = default;
};

struct IconKeyHash {
    size_t operator()(const IconKey& k) const { return std::hash<std::wstring>()(k.key) * 31 + static_cast<size_t>(k.size); }
};

bool HasExtension(const std::wstring& path, const wchar_t* ext)
{
    const size_t extLen = wcslen(ext);
    return path.size() > extLen && _wcsicmp(path.c_str() + path.size() - extLen, ext) == 0;
}

bool IsExePath(const std::wstring& path) { return HasExtension(path, L".exe"); }

bool IsDllPath(const std::wstring& path) { return HasExtension(path, L".dll"); }

// Rejects an .exe (or a .lnk to an iconless .exe without a custom icon location) so
// IShellItemImageFactory does not return the generic blank-page shell icon before better sources run.
bool PinPathHasUsableIcon(const std::wstring& pinPath)
{
    if (pinPath.empty())
        return false;
    if (IsExePath(pinPath))
        return HasEmbeddedExeIcon(pinPath);
    if (IsLink(pinPath)) {
        winrt::com_ptr<IShellLinkW> link;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) {
            if (auto file = link.try_as<IPersistFile>(); file && SUCCEEDED(file->Load(pinPath.c_str(), STGM_READ))) {
                wchar_t iconPath[MAX_PATH * 2]{};
                int index = 0;
                if (SUCCEEDED(link->GetIconLocation(iconPath, ARRAYSIZE(iconPath), &index)) && *iconPath) {
                    if (IsExePath(iconPath) || IsDllPath(iconPath))
                        return HasEmbeddedExeIcon(iconPath);
                    return true;
                }
            }
        }
        const std::wstring target = AppExecutable(pinPath);
        if (IsExePath(target))
            return HasEmbeddedExeIcon(target);
    }
    return true;
}

bool TryWindowIcons(const std::vector<HWND>& hwnds, int size, IconImage& out)
{
    for (HWND hwnd : hwnds) {
        IconImage raw;
        if (TaskWindowIcon(hwnd, size, raw)) {
            out = ApplySquircleMaskIfFullBleed(FitToSquare(raw, size));
            return true;
        }
    }
    return false;
}

bool TryExeOrWindowDiffIcon(const Job& job, IconImage& out)
{
    IconImage exeImg;
    if (!LoadShellImage(job.exePath, job.size, exeImg))
        return false;
    if (!job.hwnds.empty() && job.pinPath.empty()) {
        IconImage rawWin;
        if (TaskWindowDirectIcon(job.hwnds[0], job.size, rawWin) && rawWin.width >= 24 && rawWin.height >= 24) {
            IconImage winImg = ApplySquircleMaskIfFullBleed(FitToSquare(rawWin, job.size));
            if (ImagesVisuallyDifferent(winImg, exeImg)) {
                out = std::move(winImg);
                return true;
            }
        }
    }
    out = std::move(exeImg);
    return true;
}

bool LoadIconForJob(const Job& job, IconImage& img)
{
    if (LoadAtriumIcon(job.exePath, job.size, img))
        return true;
    // Shortcuts (.lnk) and non-exe pin targets may carry custom icons; bare .exe pins wait for AUMID first.
    if (!job.pinPath.empty() && !IsExePath(job.pinPath) && PinPathHasUsableIcon(job.pinPath) && LoadShellImage(job.pinPath, job.size, img))
        return true;
    const std::wstring effectiveAumid = !job.aumid.empty() ? job.aumid : PackagedAumidForExe(job.exePath);
    if (!effectiveAumid.empty() && LoadShellImage(AppsFolderPath(effectiveAumid), job.size, img))
        return true;
    const bool exeIcon = HasEmbeddedExeIcon(job.exePath);
    if (exeIcon && TryExeOrWindowDiffIcon(job, img))
        return true;
    if (!job.pinPath.empty() && IsExePath(job.pinPath) && HasEmbeddedExeIcon(job.pinPath) && LoadShellImage(job.pinPath, job.size, img))
        return true;
    if (TryWindowIcons(job.hwnds, job.size, img))
        return true;
    const std::wstring catalogTarget = AppCatalog::IconTargetFor(effectiveAumid, job.exePath, job.pinPath);
    if (!catalogTarget.empty() && PinPathHasUsableIcon(catalogTarget) && LoadShellImage(catalogTarget, job.size, img))
        return true;
    if (!exeIcon && !job.exePath.empty() && !AppCatalog::HasPendingInitialRefresh())
        return LoadShellImage(job.exePath, job.size, img);
    return false;
}

} // namespace

std::wstring TaskIconSource::Signature() const
{
    std::wstring sig = aumid + L"|" + pinPath + L"|" + exePath;
    if (pinPath.empty() && !hwnds.empty())
        sig += L"|" + std::to_wstring(reinterpret_cast<uintptr_t>(*std::ranges::min_element(hwnds)));
    return sig;
}

class TaskIconWorker::Impl {
public:
    Impl(HWND ui, UINT readyMsg) : ui_(ui), readyMsg_(readyMsg), worker_([this] { WorkerMain(); }) {}

    ~Impl()
    {
        {
            std::lock_guard g(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    IconImage GetOrRequest(const std::wstring& key, int sizePx, const TaskIconSource& source)
    {
        const int px = std::clamp(sizePx, 8, 512);
        const std::wstring sig = source.Signature();

        std::unique_lock g(mutex_);
        latestIconSize_[key] = px;
        PruneIcons();
        IconEntry& e = icons_[IconKey{key, px}];
        e.used = ++iconUse_;
        bool want = !e.requested;
        if (e.requested && !e.loading && e.sig != sig)
            want = true; // identity changed (pinned, relaunched from elsewhere): refresh, keep showing the old one
        if (e.requested && !e.loading && !e.have && e.sig == sig && GetTickCount64() - e.failedAt > kRetryFailedIconMs)
            want = true;
        if (want) {
            e.requested = e.loading = true;
            e.sig = sig;
            e.lastSource = source;
            jobs_.push_back(Job{false, key, px, sig, source.aumid, source.pinPath, source.exePath, source.hwnds});
            cv_.notify_one();
        }
        return e.have ? e.image : IconImage{};
    }

    void RequestName(const std::wstring& key, const TaskIconSource& source)
    {
        Job j;
        j.name = true;
        j.key = key;
        j.aumid = source.aumid;
        j.exePath = source.exePath;
        j.pinPath = source.pinPath;
        {
            std::lock_guard g(mutex_);
            jobs_.push_back(std::move(j));
        }
        cv_.notify_one();
    }

    void SyncLiveKeys(const std::unordered_set<std::wstring>& liveKeys)
    {
        std::lock_guard g(mutex_);
        liveIconKeys_ = liveKeys;
        std::erase_if(latestIconSize_, [&](const auto& entry) { return !liveIconKeys_.contains(entry.first); });
        PruneIcons();
    }

    void RetryFailedIcons()
    {
        bool woke = false;
        {
            std::lock_guard g(mutex_);
            for (auto& [iconKey, e] : icons_) {
                if (!liveIconKeys_.contains(iconKey.key) || e.have || e.loading || !e.lastSource)
                    continue;
                e.failedAt = 0;
                e.loading = true;
                jobs_.push_back(
                    Job{false, iconKey.key, iconKey.size, e.sig, e.lastSource->aumid, e.lastSource->pinPath, e.lastSource->exePath, e.lastSource->hwnds});
                woke = true;
            }
        }
        if (woke)
            cv_.notify_one();
    }

    void TakeReady(std::vector<std::wstring>& doneIcons, std::vector<std::pair<std::wstring, std::wstring>>& doneNames)
    {
        std::lock_guard g(mutex_);
        doneIcons.swap(readyIcons_);
        doneNames.swap(readyNames_);
    }

private:
    HWND ui_ = nullptr;
    UINT readyMsg_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::unordered_map<IconKey, IconEntry, IconKeyHash> icons_;
    std::unordered_set<std::wstring> liveIconKeys_;
    std::unordered_map<std::wstring, int> latestIconSize_;
    uint64_t iconUse_ = 0;
    std::vector<std::wstring> readyIcons_;
    std::vector<std::pair<std::wstring, std::wstring>> readyNames_;
    bool stop_ = false;
    std::thread worker_;

    // Under mutex_. Keep the latest size of every live/pinned item and every
    // in-flight lookup. Historical sizes share a byte/count budget.
    void PruneIcons()
    {
        std::erase_if(icons_, [&](const auto& entry) { return !entry.second.loading && !liveIconKeys_.contains(entry.first.key); });
        size_t bytes = 0;
        for (const auto& [key, entry] : icons_)
            bytes += entry.image.ByteSize();
        while (bytes > kMaxIconCacheBytes || icons_.size() > kMaxIconCacheEntries) {
            auto victim = icons_.end();
            for (auto it = icons_.begin(); it != icons_.end(); ++it) {
                const auto current = latestIconSize_.find(it->first.key);
                if (it->second.loading || (current != latestIconSize_.end() && current->second == it->first.size))
                    continue;
                if (victim == icons_.end() || it->second.used < victim->second.used)
                    victim = it;
            }
            if (victim == icons_.end())
                break; // protected live working set only
            bytes -= victim->second.image.ByteSize();
            icons_.erase(victim);
        }
    }

    void WorkerMain()
    {
        ComScope workerCom; // STA is fine for IShellItemImageFactory
        for (;;) {
            Job job;
            {
                std::unique_lock g(mutex_);
                cv_.wait(g, [&] { return stop_ || !jobs_.empty(); });
                if (stop_)
                    return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
                if (!liveIconKeys_.contains(job.key)) {
                    if (!job.name)
                        icons_.erase(IconKey{job.key, job.size});
                    continue;
                }
            }
            if (job.name)
                RunNameJob(job);
            else
                RunIconJob(job);
        }
    }

    void RunNameJob(const Job& job)
    {
        std::wstring name;
        if (!job.aumid.empty())
            name = TaskShellDisplayName(AppsFolderPath(job.aumid));
        if (name.empty() && !job.exePath.empty())
            name = TaskFileDescription(job.exePath);
        if (name.empty() && !job.pinPath.empty() && IsLink(job.pinPath))
            name = FileStem(job.pinPath);
        if (name.empty() && !job.exePath.empty())
            name = FileStem(job.exePath);
        if (name.empty())
            return;
        {
            std::lock_guard g(mutex_);
            readyNames_.emplace_back(job.key, std::move(name));
        }
        PostMessageW(ui_, readyMsg_, 0, 0);
    }

    void RunIconJob(const Job& job)
    {
        IconImage img;
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceCounter(&t0);
        const bool ok = LoadIconForJob(job, img);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        if (!ok)
            Log(L"tasks: no icon for %s (%.1f ms)", job.key.c_str(), static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart));

        bool changed = false;
        {
            std::lock_guard g(mutex_);
            auto found = icons_.find(IconKey{job.key, job.size});
            if (found == icons_.end())
                return;
            if (!liveIconKeys_.contains(job.key)) {
                icons_.erase(found);
                return;
            }
            IconEntry& e = found->second;
            if (e.sig != job.sig)
                return;
            e.loading = false;
            if (ok) {
                changed = !e.have || (e.image.pixels != img.pixels && e.image.Bytes() != img.Bytes());
                e.image = std::move(img);
                e.have = true;
            } else {
                e.failedAt = GetTickCount64();
            }
            if (changed)
                readyIcons_.push_back(job.key);
            PruneIcons();
        }
        if (changed)
            PostMessageW(ui_, readyMsg_, 0, 0);
    }
};

TaskIconWorker::TaskIconWorker(HWND ui, UINT readyMsg) : impl_(std::make_unique<Impl>(ui, readyMsg)) {}

TaskIconWorker::~TaskIconWorker() = default;

IconImage TaskIconWorker::GetOrRequest(const std::wstring& key, int sizePx, const TaskIconSource& source) { return impl_->GetOrRequest(key, sizePx, source); }

void TaskIconWorker::RequestName(const std::wstring& key, const TaskIconSource& source) { impl_->RequestName(key, source); }

void TaskIconWorker::SyncLiveKeys(const std::unordered_set<std::wstring>& liveKeys) { impl_->SyncLiveKeys(liveKeys); }

void TaskIconWorker::RetryFailedIcons() { impl_->RetryFailedIcons(); }

void TaskIconWorker::TakeReady(std::vector<std::wstring>& doneIcons, std::vector<std::pair<std::wstring, std::wstring>>& doneNames)
{
    impl_->TakeReady(doneIcons, doneNames);
}

} // namespace atrium::detail
