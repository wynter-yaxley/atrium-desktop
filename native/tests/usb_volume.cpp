// Reconciliation and worker regressions. No real disks or system settings change.

#include "shell/usb_volume.h"
#include "win32_path.h"

#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr unsigned kC = 1u << 2, kE = 1u << 4;

atrium::UsbVolumeSnapshot Attached(const wchar_t* identity = L"volume-A") { return {kC | kE, {{L'E', {L"BACKUP", identity}}}}; }

void CheckSanitization()
{
    using namespace atrium;
    // Build a deterministic CP936 mojibake string from UTF-8 bytes of "资料" (6 bytes) and 11-byte truncated UTF-8.
    auto makeMojibake = [](const std::string& utf8, UINT cp) -> std::wstring {
        const int n = MultiByteToWideChar(cp, 0, utf8.data(), int(utf8.size()), nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring out(size_t(n), L'\0');
        MultiByteToWideChar(cp, 0, utf8.data(), int(utf8.size()), out.data(), n);
        return out;
    };
    const std::string utf8Data = "\xE8\xB5\x84\xE6\x96\x99"; // "资料"
    const std::wstring mojibake936 = makeMojibake(utf8Data, 936);
    if (!mojibake936.empty()) {
        Check(SanitizeUsbVolumeLabel(mojibake936 + L"   ", 936, 936) == L"\x8D44\x6599", "repairs UTF-8 decoded as CP936 volume label");
        Check(SanitizeAutoPlayText(mojibake936 + L" (\x2068"
                                                 L"E:\x2069)",
                                   936, 936) == L"\x8D44\x6599 (E:)",
              "repairs mojibake AutoPlay toast title and strips BiDi isolates");
    }
    // Clean Chinese volume label is never garbled further.
    Check(SanitizeUsbVolumeLabel(L"我的U盘  ", 936, 936) == L"我的U盘", "preserves valid CJK USB label and trims spaces");
    Check(StripInvisibleUiChars(L"工作盘 (\x2068"
                                L"F:\x2069)") == L"工作盘 (F:)",
          "strips U+2068/U+2069 BiDi isolates");
    // 11-byte FAT32 label truncated mid-UTF8 character ("资料ABCD"+truncated lead+cont):
    const std::string fat11 = "AB\xE8\xB5\x84\xE6\x96\x99\xE7\x9B\xAE"; // 2 + 3 + 3 + 3 = 11 bytes valid, test cut 11 bytes:
    const std::string fat11Cut = "ABC\xE8\xB5\x84\xE6\x96\x99\xE7\x9B"; // 3 + 3 + 3 + 2 incomplete = 11 bytes
    const std::wstring fat11Cut936 = makeMojibake(fat11Cut, 936);
    if (!fat11Cut936.empty())
        Check(SanitizeUsbVolumeLabel(fat11Cut936, 936, 936) == L"ABC\x8D44\x6599", "repairs 11-byte FAT32 label cut mid-UTF8");
    (void)fat11;
}

void CheckReconciliation()
{
    using namespace atrium;
    Check(WatchUsbVolume(DRIVE_FIXED, true), "a fixed-media USB disk is watched");
    Check(!WatchUsbVolume(DRIVE_FIXED, false), "an internal fixed disk is not watched");
    Check(WatchUsbVolume(DRIVE_REMOVABLE, false), "removable media is watched");
    Check(!WatchUsbVolume(DRIVE_CDROM, true), "a media-change flag cannot turn a CD into a USB disk");
    Check(!WatchUsbVolume(DRIVE_REMOTE, false), "network volumes are not USB devices");

    UsbVolumes volumes;
    Check(volumes.Sync(Attached()).empty(), "startup records already-attached USB disks silently");
    volumes.Observe({DBT_DEVICEARRIVAL, kE}); // no DBTF_MEDIA requirement
    Check(volumes.Sync(Attached()).empty(), "a duplicate arrival does not post a removal");
    auto removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && removed[0].letter == L'E' && removed[0].label == L"BACKUP" && !removed[0].ejected,
          "a fixed USB disk removal uses its cached label without a media event");
    Check(volumes.Sync({kC, {}}).empty(), "interface and volume removal broadcasts do not duplicate the card");

    volumes.Sync(Attached());
    volumes.Observe({DBT_DEVICEREMOVEPENDING, kE});
    removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && removed[0].ejected, "an identified eject keeps its removal hint");
    volumes.Sync(Attached());
    volumes.Observe({DBT_DEVICEREMOVEPENDING, kE});
    volumes.Observe({DBT_DEVICEQUERYREMOVEFAILED, kE});
    removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && !removed[0].ejected, "a cancelled eject is not reused for a later unplug");

    volumes.Sync(Attached());
    Check(volumes.Sync({kC | kE, {}}).empty(), "a temporarily unreadable but still-present disk is retained");
    removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && removed[0].label == L"BACKUP", "a failed query does not erase the cached label");

    volumes.Sync(Attached());
    volumes.Observe({DBT_DEVICEREMOVEPENDING, kE});
    Check(volumes.Sync({kC | kE, {{L'E', {}}}}).empty(), "an unreadable ejecting volume stays watched");
    removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && removed[0].label == L"BACKUP" && removed[0].ejected, "a failed query during eject keeps the cached label and eject hint");

    volumes.Sync(Attached());
    volumes.Observe({DBT_DEVICEREMOVEPENDING, kE});
    Check(volumes.Sync({kC | kE, {{L'E', {L"", L"volume-A"}}}}).empty(), "a partial query during eject stays watched");
    removed = volumes.Sync({kC, {}});
    Check(removed.size() == 1 && removed[0].label == L"BACKUP" && removed[0].ejected,
          "a failed label query with identity still present keeps the cached label");

    volumes.Sync(Attached());
    volumes.Observe({DBT_DEVICEREMOVEPENDING, kE});
    removed = volumes.Sync(Attached(L"volume-B"));
    Check(removed.size() == 1 && !removed[0].ejected, "drive-letter reuse removes the old identity without a safe-eject claim");
}

void CheckWorker()
{
    using namespace atrium;
    std::mutex mutex;
    std::condition_variable changed;
    bool reading = false, release = false, ready = false;
    unsigned reads = 0;
    std::thread::id readerThread;
    UsbVolumeMonitor monitor(
        [&] {
            std::lock_guard lock(mutex);
            ready = true;
            changed.notify_all();
        },
        [&]() -> std::optional<UsbVolumeSnapshot> {
            std::unique_lock lock(mutex);
            const unsigned call = ++reads;
            readerThread = std::this_thread::get_id();
            changed.notify_all();
            if (call == 1) {
                reading = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; }); // a slow disk, held until Changed has returned
                return Attached();
            }
            if (call == 2)
                return std::nullopt; // transient enumeration failure
            return UsbVolumeSnapshot{kC, {}};
        });
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return reading; }), "startup disk query runs asynchronously");
        Check(readerThread != std::this_thread::get_id(), "device reads run off the caller thread");
        monitor.Changed({DBT_DEVICEREMOVECOMPLETE, kE});
        Check(reads == 1, "Changed returns while the disk read is still blocked");
        release = true;
        changed.notify_all();
        Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return reads >= 2; }), "events arriving during the startup read are retained");
        Check(!ready, "failed enumeration does not claim the disks were unplugged");
    }
    Check(monitor.TakeRemoved().empty(), "failed enumeration keeps prior device state");
    monitor.Changed({DBT_DEVICEREMOVECOMPLETE, 0});
    {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, std::chrono::seconds(3), [&] { return ready; }), "the UI is notified after a successful background reconciliation");
    }
    const auto removed = monitor.TakeRemoved();
    Check(removed.size() == 1 && removed[0].letter == L'E' && removed[0].label == L"BACKUP", "background results retain the original disk identity and label");
    Check(monitor.TakeRemoved().empty(), "results are consumed once");
}

} // namespace

int main(int argc, char** argv)
{
    using namespace atrium;

    // Optional read-only integration probe. Only counts are printed; no volume
    // labels, identities or device paths leave the process.
    if (argc == 2 && std::string(argv[1]) == "--scan") {
        DWORD before = 0;
        const bool restore = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &before) != FALSE;
        const ULONGLONG start = GetTickCount64();
        const auto snapshot = ReadUsbVolumeSnapshot();
        if (restore)
            SetThreadErrorMode(before, nullptr);
        if (!snapshot)
            return 1;
        std::printf("read-only system scan: %zu drive letters, %zu removable/USB volumes, %llu ms\n", RemovedLetters(snapshot->present, 0).size(),
                    snapshot->volumes.size(), GetTickCount64() - start);
        return 0;
    }

    auto letters = RemovedLetters(0x000A, 0x0002); // B and D were watched, B stayed
    Check(letters.size() == 1 && letters[0] == L'D', "the letter that went is found");

    Check(RemovedLetters(0x03FFFFFF, 0).size() == 26, "every letter can go at once");
    Check(RemovedLetters(0x0002, 0x000A).empty(), "a letter that arrived is no removal");

    Check(VolumeName(L"BACKUP", L'E') == L"BACKUP (E:)", "a labelled volume is named by its label");
    Check(VolumeName(L"", L'E') == L"E:", "a volume without a label is named by its letter");

    Check(JoinNames({L"E:"}) == L"E:", "a single name is not joined");
    Check(JoinNames({L"E:", L"F:"}) == std::wstring(L"E:") + Tr(L", ") + L"F:", "the names are joined");

    CheckReconciliation();
    CheckSanitization();
    CheckWorker();

    if (g_failures) {
        std::fprintf(stderr, "%d usb volume check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("usb volume checks passed");
    return 0;
}
