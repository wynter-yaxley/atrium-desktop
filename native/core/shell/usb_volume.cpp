#include "shell/usb_volume.h"

#include "log.h"
#include "win32_path.h"

#include <shellapi.h>
#include <winioctl.h>

#include <cstddef>
#include <iterator>
#include <memory>

namespace atrium {

namespace {

std::wstring RootOf(wchar_t letter) { return std::wstring(1, letter) + L":\\"; }

bool OnUsbBus(wchar_t letter)
{
    const std::wstring path = L"\\\\.\\" + std::wstring(1, letter) + L":";
    HANDLE raw = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return false;
    std::unique_ptr<void, decltype(&CloseHandle)> volume(raw, CloseHandle);
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD bytes = 0;
    bool usb = false;
    if (DeviceIoControl(volume.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &header, sizeof(header), &bytes, nullptr) &&
        bytes >= sizeof(header) && header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR) && header.Size <= 65536) {
        std::vector<BYTE> buffer(header.Size);
        if (DeviceIoControl(volume.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer.data(), DWORD(buffer.size()), &bytes, nullptr) &&
            bytes >= offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties)) {
            const auto* device = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
            usb = device->BusType == BusTypeUsb;
        }
    }
    return usb;
}

} // namespace

std::optional<UsbVolumeSnapshot> ReadUsbVolumeSnapshot()
{
    const DWORD drives = GetLogicalDrives();
    if (!drives) {
        Log(L"usb: GetLogicalDrives failed (%lu)", GetLastError());
        return std::nullopt; // a failed enumeration must not look like all disks were unplugged
    }
    UsbVolumeSnapshot snapshot;
    snapshot.present = drives;
    for (wchar_t letter : RemovedLetters(drives, 0)) {
        const std::wstring root = RootOf(letter);
        const UINT type = GetDriveTypeW(root.c_str());
        if (!WatchUsbVolume(type, type == DRIVE_FIXED && OnUsbBus(letter)))
            continue;
        UsbVolume volume;
        wchar_t name[MAX_PATH + 1]{};
        if (GetVolumeInformationW(root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
            volume.labelRead = true;
            volume.label = SanitizeUsbVolumeLabel(name);
            if (name[0] != L'\0' && volume.label.empty()) {
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) && sfi.szDisplayName[0]) {
                    std::wstring fallback = StripInvisibleUiChars(sfi.szDisplayName);
                    const std::wstring suffix = L"(" + std::wstring(1, letter) + L":)";
                    if (fallback.ends_with(suffix))
                        fallback.resize(fallback.size() - suffix.size());
                    volume.label = SanitizeUsbVolumeLabel(fallback);
                }
            }
        }
        wchar_t identity[MAX_PATH + 1]{};
        if (GetVolumeNameForVolumeMountPointW(root.c_str(), identity, MAX_PATH + 1))
            volume.identity = identity;
        snapshot.volumes.emplace(letter, std::move(volume));
    }
    return snapshot;
}

void UsbVolumes::Observe(const Event& event)
{
    if (event.kind == DBT_DEVICEARRIVAL || event.kind == DBT_DEVICEQUERYREMOVEFAILED)
        ejecting_ &= ~event.unitmask;
    else if (event.kind == DBT_DEVICEREMOVEPENDING)
        ejecting_ |= event.unitmask;
}

std::vector<RemovedVolume> UsbVolumes::Sync(const UsbVolumeSnapshot& snapshot)
{
    std::vector<RemovedVolume> removed;
    for (auto it = watched_.begin(); it != watched_.end();) {
        const wchar_t letter = it->first;
        const unsigned bit = 1u << (letter - L'A');
        const auto next = snapshot.volumes.find(letter);
        const bool replaced =
            next != snapshot.volumes.end() && !it->second.identity.empty() && !next->second.identity.empty() && it->second.identity != next->second.identity;
        if (!(snapshot.present & bit) || replaced) {
            removed.push_back({letter, it->second.label, !replaced && (ejecting_ & bit) != 0});
            ejecting_ &= ~bit;
            it = watched_.erase(it);
        } else {
            ++it; // keep the cached identity if a still-present disk could not be queried
        }
    }
    for (const auto& [letter, volume] : snapshot.volumes) {
        auto& cached = watched_[letter];
        if (!volume.identity.empty())
            cached.identity = volume.identity;
        if (volume.labelRead || !volume.label.empty())
            cached.label = volume.label;
    }
    return removed;
}

UsbVolumeMonitor::UsbVolumeMonitor(std::function<void()> ready, Reader reader)
    : ready_(std::move(ready)),
      reader_(std::move(reader)),
      worker_([this] { Worker(); })
{
}

UsbVolumeMonitor::~UsbVolumeMonitor()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    CancelSynchronousIo(worker_.native_handle());
    worker_.join();
}

void UsbVolumeMonitor::Changed(const UsbVolumes::Event& event)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        events_.push_back(event);
    }
    changed_.notify_one();
}

std::vector<RemovedVolume> UsbVolumeMonitor::TakeRemoved()
{
    std::lock_guard lock(mutex_);
    std::vector<RemovedVolume> removed;
    removed.swap(removed_);
    return removed;
}

void UsbVolumeMonitor::Worker()
{
    // Empty readers and failing disks must never raise an "insert disk" dialog.
    DWORD before = 0;
    const bool restoreErrors = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &before) != FALSE;
    UsbVolumes volumes;
    bool initial = true;
    for (;;) {
        std::vector<UsbVolumes::Event> events;
        {
            std::unique_lock lock(mutex_);
            if (!initial)
                changed_.wait(lock, [this] { return stopping_ || !events_.empty(); });
            if (stopping_)
                break;
            events.swap(events_);
        }
        initial = false;
        for (const auto& event : events)
            volumes.Observe(event);
        if (!reader_) {
            Log(L"usb: volume reader is null");
            break;
        }
        const auto snapshot = reader_();
        if (!snapshot) {
            Log(L"usb: reading device information failed");
            continue;
        }
        auto removed = volumes.Sync(*snapshot);
        bool notify = false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                break;
            notify = !removed.empty();
            removed_.insert(removed_.end(), std::make_move_iterator(removed.begin()), std::make_move_iterator(removed.end()));
        }
        if (notify && ready_)
            ready_();
    }
    if (restoreErrors)
        SetThreadErrorMode(before, nullptr);
}

} // namespace atrium
