// Pure backdrop source selection, retry backoff, and black-capture fallback policy.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace atrium {

class CapturePolicy {
public:
    enum class InitialSource { Wallpaper, Duplication, MonitorCapture };
    enum class BlackAction { RestartCapture, SwitchToMonitorCapture, FallbackToWallpaper };
    enum class DuplicationFailure { Transient, Elsewhere };

    static constexpr uint64_t kRetryBackoffMs = 30000;
    static constexpr uint64_t kMaxRetryBackoffMs = 300000;
    static constexpr uint64_t kBlackRetryInitialMs = 2000;
    static constexpr uint64_t kBlackRetryMaxMs = 120000;
    static constexpr uint64_t kBlackDetectMs = 1500;
    static constexpr uint64_t kBlackWindowMs = 30000;
    static constexpr float kBlackLumaThreshold = 0.01f;
    static constexpr int kMaxBlackRestarts = 2;

    void BeginSession(uintptr_t monitorId = 0, unsigned captureGeneration = 0)
    {
        blackSince_.reset();
        healthySince_.reset();
        if (lastBlackGeneration_ && captureGeneration != *lastBlackGeneration_) {
            lastBlackGeneration_.reset();
            blackRetryBackoffMs_ = kBlackRetryInitialMs;
            if (blackEscalated_ && !elsewhere_) {
                duplicationFailed_ = false;
                blackEscalated_ = false;
                retryBackoffMs_ = kRetryBackoffMs;
            }
        }
        if (elsewhere_ && (monitorId != elsewhereMonitor_ || captureGeneration != elsewhereGeneration_)) {
            elsewhere_ = false;
            if (!blackEscalated_) {
                duplicationFailed_ = false;
                retryBackoffMs_ = kRetryBackoffMs;
            }
        }
    }

    void ResetBlackTimer()
    {
        blackSince_.reset();
        healthySince_.reset();
    }

    InitialSource ChooseInitialSource(bool remote, uint64_t nowMs) const
    {
        if (remote || nowMs < wallpaperUntil_)
            return InitialSource::Wallpaper;
        return duplicationFailed_ ? InitialSource::MonitorCapture : InitialSource::Duplication;
    }

    void OnDuplicationUnavailable(uint64_t nowMs, DuplicationFailure reason = DuplicationFailure::Transient, uintptr_t monitorId = 0,
                                  unsigned captureGeneration = 0)
    {
        duplicationFailed_ = true;
        if (reason == DuplicationFailure::Elsewhere) {
            elsewhere_ = true;
            elsewhereMonitor_ = monitorId;
            elsewhereGeneration_ = captureGeneration;
            return;
        }
        elsewhere_ = false;
        blackEscalated_ = false;
        duplicationRetryAt_ = nowMs + retryBackoffMs_;
        retryBackoffMs_ = std::min<uint64_t>(retryBackoffMs_ * 2, kMaxRetryBackoffMs);
    }

    bool ShouldProbeDuplication(bool needScreen, bool remote, bool captureKeepsOwnOut, uintptr_t monitorId, unsigned captureGeneration, uint64_t nowMs)
    {
        if (!needScreen || remote || !duplicationFailed_ || captureKeepsOwnOut)
            return false;
        if (elsewhere_ && monitorId == elsewhereMonitor_ && captureGeneration == elsewhereGeneration_)
            return false;
        if (nowMs < duplicationRetryAt_)
            return false;
        const uint64_t cap = blackEscalated_ ? kBlackRetryMaxMs : kMaxRetryBackoffMs;
        duplicationRetryAt_ = nowMs + retryBackoffMs_;
        retryBackoffMs_ = std::min<uint64_t>(retryBackoffMs_ * 2, cap);
        return true;
    }

    bool ShouldProbeDuplication(bool needScreen, bool remote, bool captureKeepsOwnOut, unsigned captureGeneration, uint64_t nowMs)
    {
        return ShouldProbeDuplication(needScreen, remote, captureKeepsOwnOut, 0, captureGeneration, nowMs);
    }

    void OnDuplicationRecovered()
    {
        duplicationFailed_ = false;
        elsewhere_ = false;
        blackEscalated_ = false;
        retryBackoffMs_ = kRetryBackoffMs;
        healthySince_.reset();
    }

    bool ShouldExitBlackoutWallpaper(bool blackOut, uint64_t nowMs) const { return blackOut && nowMs >= wallpaperUntil_; }

    // Keep the GDI ScreenLitAbove() sample gated behind the 1500 ms dark-backdrop timer.
    bool PollBlackBackdrop(bool eligible, float brightestLuma, uint64_t nowMs)
    {
        if (!eligible || brightestLuma > kBlackLumaThreshold) {
            blackSince_.reset();
            if (blackRestartMs_ && nowMs >= *blackRestartMs_ && nowMs - *blackRestartMs_ > kBlackWindowMs) {
                blackRestarts_ = 0;
                blackRestartMs_.reset();
            }
            if (eligible && !duplicationFailed_ && blackRetryBackoffMs_ > kBlackRetryInitialMs) {
                if (!healthySince_)
                    healthySince_ = nowMs;
                else if (nowMs >= *healthySince_ && nowMs - *healthySince_ > kBlackWindowMs) {
                    blackRetryBackoffMs_ = kBlackRetryInitialMs;
                    healthySince_.reset();
                }
            } else {
                healthySince_.reset();
            }
            return false;
        }
        healthySince_.reset();
        if (!blackSince_) {
            blackSince_ = nowMs;
            return false;
        }
        if (nowMs < *blackSince_ || nowMs - *blackSince_ <= kBlackDetectMs)
            return false;
        blackSince_.reset();
        return true;
    }

    BlackAction OnBlackOverLitScreen(unsigned captureGeneration, uint64_t nowMs)
    {
        blackRestartMs_ = nowMs;
        healthySince_.reset();
        if (++blackRestarts_ < kMaxBlackRestarts)
            return BlackAction::RestartCapture;
        blackRestarts_ = 0;
        blackRestartMs_.reset();
        if (duplicationFailed_) {
            wallpaperUntil_ = nowMs + kRetryBackoffMs;
            return BlackAction::FallbackToWallpaper;
        }
        if (lastBlackGeneration_ && *lastBlackGeneration_ != captureGeneration)
            blackRetryBackoffMs_ = kBlackRetryInitialMs;
        lastBlackGeneration_ = captureGeneration;
        duplicationFailed_ = true;
        blackEscalated_ = true;
        duplicationRetryAt_ = nowMs + blackRetryBackoffMs_;
        retryBackoffMs_ = std::min<uint64_t>(blackRetryBackoffMs_ * 2, kBlackRetryMaxMs);
        blackRetryBackoffMs_ = retryBackoffMs_;
        return BlackAction::SwitchToMonitorCapture;
    }

private:
    bool duplicationFailed_ = false;
    bool elsewhere_ = false;
    bool blackEscalated_ = false;
    uintptr_t elsewhereMonitor_ = 0;
    unsigned elsewhereGeneration_ = 0;
    uint64_t duplicationRetryAt_ = 0;
    uint64_t retryBackoffMs_ = kRetryBackoffMs;
    uint64_t blackRetryBackoffMs_ = kBlackRetryInitialMs;
    std::optional<unsigned> lastBlackGeneration_;
    uint64_t wallpaperUntil_ = 0;
    int blackRestarts_ = 0;
    std::optional<uint64_t> blackRestartMs_;
    std::optional<uint64_t> blackSince_;
    std::optional<uint64_t> healthySince_;
};

} // namespace atrium
