// Still pictures of windows for Stage Manager's strip: taken just before a window is put away
// (a minimized window has nothing to show), on worker threads, since PrintWindow waits for the
// window's app. A picture is small (about the size it is shown at), its corners rounded, with a soft
// shadow of its own; both premultiplied BGRA, ready for Canvas::ImagePerspective.
#pragma once

#include "icon_image.h"

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atrium {

struct WindowPicture {
    IconImage image;  // the window, with kPicturePad transparent px around it
    IconImage shadow; // its shadow at half the size, kShadowPad of the picture's px around it
    int width = 0, height = 0; // the window's part of `image`, px
    float corner = 0; // its corner radius, px
    bool ok = false;
};

constexpr int kPicturePad = 2;
constexpr int kShadowPad = 24;

// The soft shadow of a w x h rounded rectangle, at half its size, kShadowPad of its px around it.
IconImage SoftShadow(int w, int h);

// The picture of a placeholder window: `icon` in the middle of a plain rounded panel (a window
// put away before Atrium started has no picture).
WindowPicture PlaceholderPicture(const IconImage& icon, int width, int height, bool dark);

// `picture` (a large one, no shadow) at most `largeMax` px either way.
WindowPicture Smaller(const WindowPicture& picture, int largeMax);

// Builds a premultiplied rounded snapshot from BGRA pixels (`sw` x `sh`, byte `stride`)
// at size `tw` x `th`, preserving legacy area-average math without full-image temp buffers.
WindowPicture BuildWindowPictureFromBgra(const uint8_t* src, int sw, int sh, int stride, int tw, int th, float radius, bool shadow = true);

struct WindowSnapshotKey {
    HWND window = nullptr;
    DWORD pid = 0;
    FILETIME creationTime{};
    uint64_t generation = 0;
};

// Queries the owning process PID and creation time for `window` to guard against recycled HWND reuse.
bool QueryWindowSnapshotKey(HWND window, uint64_t generation, WindowSnapshotKey& out);

// Bounded-byte LRU cache of large window snapshots keyed by HWND, PID, and process creation time.
class WindowPictureCache {
public:
    static constexpr size_t kDefaultMaxBytes = 64ULL * 1024ULL * 1024ULL;

    struct Entry {
        WindowSnapshotKey key;
        RECT frame{};
        std::shared_ptr<const WindowPicture> picture;
        size_t byteSize = 0;
        uint64_t lastUsedSeq = 0;
    };

    explicit WindowPictureCache(size_t maxBytes = kDefaultMaxBytes);

    void Put(const WindowSnapshotKey& key, const RECT& frame, std::shared_ptr<const WindowPicture> picture);
    const Entry* Find(HWND window, DWORD pid, FILETIME creationTime, uint64_t minGeneration = 0);
    bool Touch(HWND window, DWORD pid, FILETIME creationTime);
    bool Erase(HWND window);
    void Clear();

    size_t TotalBytes() const { return totalBytes_; }
    size_t Count() const { return entries_.size(); }
    size_t MaxBytes() const { return maxBytes_; }
    void SetMaxBytes(size_t maxBytes);

private:
    void EvictToBudget();

    size_t maxBytes_ = kDefaultMaxBytes;
    size_t totalBytes_ = 0;
    uint64_t nextSeq_ = 0;
    std::unordered_map<HWND, Entry> entries_;
};

class WindowSnapshots {
public:
    // Results are announced with `message` posted to `notify`, then Take()n.
    WindowSnapshots(HWND notify, UINT message);
    ~WindowSnapshots();
    WindowSnapshots(const WindowSnapshots&) = delete;
    WindowSnapshots& operator=(const WindowSnapshots&) = delete;

    // Pictures `window` at most maxWidth x maxHeight px, corners of `radius` px; with largeMax, also
    // a large one (at most largeMax px either way, the window's own corners, no shadow).
    void Request(HWND window, int maxWidth, int maxHeight, float radius, int largeMax = 0, bool urgent = false);
    // Downsamples an existing `large` snapshot on the worker thread, announcing completion with `shrinkGen == gen`.
    void RequestShrink(HWND window, unsigned gen, std::shared_ptr<const WindowPicture> large, int largeMax);
    void SignalStop();
    bool WaitStop(DWORD timeoutMs) const;
    struct Result {
        HWND window;
        WindowPicture picture;
        WindowPicture large; // ok only if asked for and taken
        unsigned shrinkGen = 0;
    };
    std::vector<Result> Take();

private:
    struct Job {
        HWND window;
        int maxWidth, maxHeight, largeMax;
        float radius;
    };
    struct ShrinkJob {
        HWND window = nullptr;
        unsigned gen = 0;
        std::shared_ptr<const WindowPicture> large;
        int largeMax = 0;
    };
    void Worker();

    std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::deque<ShrinkJob> shrinkQueue_;
    std::unordered_set<HWND> pending_; // queued or in-flight capture jobs: at most one entry per window
    std::vector<Result> done_;
    HWND notify_ = nullptr;
    UINT message_ = 0;
    bool stop_ = false;
    HANDLE stopEvent_ = nullptr;
    HANDLE doneEvent_ = nullptr;
    std::thread worker_;
};

} // namespace atrium
