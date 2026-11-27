// Hardware D3D11 + DirectComposition Genie mesh renderer owned exclusively by Bar's render thread.
#pragma once

#include "../icon_image.h"
#include "genie_mesh.h"
#include "gpu.h"

#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace atrium {

struct GenieFlightDraw {
    uint64_t flightId = 0;
    uint64_t pictureId = 0;
    std::shared_ptr<const IconImage> image;
    int picWidth = 0;
    int picHeight = 0;
    int padPx = kGenieDefaultPicturePadPx;
    RECT fromRect{};
    RECT toRect{};
    GenieFlightPhase phase = GenieFlightPhase::Flying;
    bool isRestore = false;
    double phaseStartMs = 0.0;
    double flightMs = kGenieFlightMs;
    double dissolveMs = kGenieDissolveMs;
    GenieDirection direction = GenieDirection::Down;
    bool inferDirection = true;
};

struct GenieSceneSnapshot {
    uint64_t seq = 0;
    uint64_t epoch = 0;
    HWND overlayHwnd = nullptr;
    RECT overlayBounds{};
    std::vector<GenieFlightDraw> flights;
};

constexpr UINT WM_GENIE_FRAME_PRESENTED = WM_APP + 0x44;
constexpr UINT WM_GENIE_DEVICE_LOST = WM_APP + 0x45;

// Host interface implemented by the primary Bar to receive Genie scenes on the UI thread.
class GenieHost {
public:
    GenieHost() = default;
    virtual ~GenieHost() = default;
    GenieHost(const GenieHost&) = delete;
    GenieHost& operator=(const GenieHost&) = delete;
    GenieHost(GenieHost&&) = delete;
    GenieHost& operator=(GenieHost&&) = delete;
    virtual void SetGenieOverlay(HWND hwnd, uint64_t epoch) = 0;
    virtual void SubmitGenieFlights(GenieSceneSnapshot scene) = 0;
    virtual void ClearGenieFlights() = 0;
};

struct GenieFlightEval {
    float meshT = 0.0f;
    float opacity = 0.0f;
    bool animating = false;
    bool pourFinished = false;
};

inline GenieFlightEval EvaluateGenieFlightAt(const GenieFlightDraw& f, double nowMs)
{
    const double flightMs = std::max(1.0, f.flightMs);
    const double dissolveMs = std::max(1.0, f.dissolveMs);
    switch (f.phase) {
    case GenieFlightPhase::CoverWait: return {0.0f, 1.0f, false, false};
    case GenieFlightPhase::Flying: {
        const double elapsed = std::max(0.0, nowMs - f.phaseStartMs);
        const float t = static_cast<float>(std::clamp(elapsed / flightMs, 0.0, 1.0));
        return {t, 1.0f - GenieSmoothStep(0.86f, 1.0f, t), elapsed < flightMs, false};
    }
    case GenieFlightPhase::PouringOut: {
        const double elapsed = std::max(0.0, nowMs - f.phaseStartMs);
        const float t = 1.0f - static_cast<float>(std::clamp(elapsed / flightMs, 0.0, 1.0));
        return {t, 1.0f - GenieSmoothStep(0.86f, 1.0f, t), elapsed < flightMs, elapsed >= flightMs};
    }
    case GenieFlightPhase::RestoreHolding: return {0.0f, 1.0f, false, false};
    case GenieFlightPhase::Dissolving: {
        const double elapsed = std::max(0.0, nowMs - f.phaseStartMs);
        const float alpha = 1.0f - GenieSmoothStep(0.0f, static_cast<float>(dissolveMs), static_cast<float>(elapsed));
        return {0.0f, alpha, elapsed < dissolveMs, false};
    }
    case GenieFlightPhase::Completed: return {0.0f, 0.0f, false, false};
    }
    return {};
}

class GenieRenderer {
public:
    static constexpr double kIdleReleaseMs = 4000.0;

    enum class RenderStatus { Ok, DeviceLost };
    struct RenderResult {
        RenderStatus status = RenderStatus::Ok;
        bool active = false;
    };
    using FrameNotifyFn = std::function<void(uint64_t flightId, double nowMs, bool pourFinished)>;

    explicit GenieRenderer(Gpu gpu);
    ~GenieRenderer();
    GenieRenderer(const GenieRenderer&) = delete;
    GenieRenderer& operator=(const GenieRenderer&) = delete;

    RenderResult Render(const GenieSceneSnapshot& scene, double nowMs, const FrameNotifyFn& onFramePresented);
    void ReleaseSurfaceResources();

private:
    struct CachedTexture {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> srv;
    };

    bool EnsurePipeline();
    bool EnsureIndexBuffer(int cols, int rows);
    bool EnsureVertexBuffer(size_t vertexCount);
    bool EnsureSurface(HWND overlayHwnd, int width, int height, bool& surfaceLost);
    ID3D11ShaderResourceView* LookupOrUploadTexture(uint64_t pictureId, const IconImage& image);
    RenderResult RenderIdle(const GenieSceneSnapshot& scene, double nowMs);
    void BindFramePipeline(int width, int height);
    bool DrawFlight(const GenieFlightDraw& f, const GenieFlightEval& ev, const RECT& overlayBounds, bool& deviceLost);
    void PruneUnusedTextures(const GenieSceneSnapshot& scene);
    void PruneFlightTracking(const GenieSceneSnapshot& scene);
    bool IsDeviceRemoved(HRESULT hr) const;

    Gpu gpu_;
    bool pipelineReady_ = false;
    bool canAutoGenMips_ = false;

    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11InputLayout> inputLayout_;
    winrt::com_ptr<ID3D11Buffer> cb_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    winrt::com_ptr<ID3D11RasterizerState> rasterizer_;
    winrt::com_ptr<ID3D11Buffer> vb_;
    winrt::com_ptr<ID3D11Buffer> ib_;
    size_t vbCapacity_ = 0;
    int ibCols_ = 0;
    int ibRows_ = 0;
    UINT ibIndexCount_ = 0;

    HWND overlayHwnd_ = nullptr;
    int swapW_ = 0;
    int swapH_ = 0;
    winrt::com_ptr<IDCompositionDevice> dcomp_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<IDXGISwapChain1> swap_;
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;

    std::unordered_map<uint64_t, CachedTexture> textures_;
    std::unordered_set<uint64_t> reportedFirstFrames_;
    std::unordered_set<uint64_t> reportedPourDone_;

    uint64_t presentedSeq_ = 0;
    bool hasPresentedNonEmpty_ = false;
    double idleSinceMs_ = 0.0;
};

} // namespace atrium
