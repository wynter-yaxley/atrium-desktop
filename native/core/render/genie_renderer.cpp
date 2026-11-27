#include "genie_renderer.h"

#include "../log.h"
#include "genie_ps.h"
#include "genie_vs.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

namespace atrium {

namespace {

struct GenieParamsCb {
    float opacity = 1.0f;
    float pad[3]{};
};
static_assert(sizeof(GenieParamsCb) == 16);

bool IsDeviceRemovedHr(HRESULT hr) { return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG; }

} // namespace

GenieRenderer::GenieRenderer(Gpu gpu) : gpu_(std::move(gpu)) {}

GenieRenderer::~GenieRenderer() { ReleaseSurfaceResources(); }

bool GenieRenderer::IsDeviceRemoved(HRESULT hr) const
{
    if (IsDeviceRemovedHr(hr))
        return true;
    return !gpu_.device || FAILED(gpu_.device->GetDeviceRemovedReason());
}

void GenieRenderer::ReleaseSurfaceResources()
{
    rtv_ = nullptr;
    if (visual_)
        visual_->SetContent(nullptr);
    if (target_)
        target_->SetRoot(nullptr);
    if (dcomp_)
        dcomp_->Commit();
    swap_ = nullptr;
    visual_ = nullptr;
    target_ = nullptr;
    dcomp_ = nullptr;
    overlayHwnd_ = nullptr;
    swapW_ = 0;
    swapH_ = 0;
    textures_.clear();
    hasPresentedNonEmpty_ = false;
}

bool GenieRenderer::EnsurePipeline()
{
    if (pipelineReady_)
        return true;
    auto* dev = gpu_.device.get();
    if (!dev)
        return false;

    if (FAILED(dev->CreateVertexShader(g_GenieVS, sizeof(g_GenieVS), nullptr, vs_.put())) ||
        FAILED(dev->CreatePixelShader(g_GeniePS, sizeof(g_GeniePS), nullptr, ps_.put())))
        return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(GenieVertex, x)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(GenieVertex, u)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(GenieVertex, edgeU)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, static_cast<UINT>(offsetof(GenieVertex, clipSignedPx)), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(layout, ARRAYSIZE(layout), g_GenieVS, sizeof(g_GenieVS), inputLayout_.put())))
        return false;

    const D3D11_BUFFER_DESC cbd{sizeof(GenieParamsCb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(dev->CreateBuffer(&cbd, nullptr, cb_.put())))
        return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 8;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, sampler_.put())))
        return false;

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, blend_.put())))
        return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    if (FAILED(dev->CreateRasterizerState(&rd, rasterizer_.put())))
        return false;

    UINT sup = 0;
    canAutoGenMips_ = SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &sup)) && (sup & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0 &&
                      (sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0 && (sup & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0 &&
                      (sup & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0;

    if (!EnsureIndexBuffer(kGenieGridCols, kGenieGridRows) ||
        !EnsureVertexBuffer(static_cast<size_t>(kGenieGridCols + 1) * static_cast<size_t>(kGenieGridRows + 1)))
        return false;

    pipelineReady_ = true;
    return true;
}

bool GenieRenderer::EnsureIndexBuffer(int cols, int rows)
{
    if (ib_ && ibCols_ == cols && ibRows_ == rows)
        return true;
    std::vector<uint16_t> indices;
    const size_t stride = static_cast<size_t>(cols) + 1;
    indices.reserve(static_cast<size_t>(rows) * static_cast<size_t>(cols) * 6);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const auto i00 = static_cast<uint16_t>(static_cast<size_t>(r) * stride + static_cast<size_t>(c));
            const auto i10 = static_cast<uint16_t>(i00 + 1);
            const auto i01 = static_cast<uint16_t>(i00 + stride);
            const auto i11 = static_cast<uint16_t>(i01 + 1);
            indices.push_back(i00);
            indices.push_back(i10);
            indices.push_back(i11);
            indices.push_back(i00);
            indices.push_back(i11);
            indices.push_back(i01);
        }
    }
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint16_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{indices.data(), 0, 0};
    winrt::com_ptr<ID3D11Buffer> nextIb;
    if (FAILED(gpu_.device->CreateBuffer(&ibd, &init, nextIb.put())))
        return false;
    ib_ = std::move(nextIb);
    ibCols_ = cols;
    ibRows_ = rows;
    ibIndexCount_ = static_cast<UINT>(indices.size());
    return true;
}

bool GenieRenderer::EnsureVertexBuffer(size_t vertexCount)
{
    if (vb_ && vertexCount <= vbCapacity_)
        return true;
    const size_t cap = std::max(vertexCount, static_cast<size_t>(kGenieGridCols + 1) * static_cast<size_t>(kGenieGridRows + 1));
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = static_cast<UINT>(cap * sizeof(GenieVertex));
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    winrt::com_ptr<ID3D11Buffer> nextVb;
    if (FAILED(gpu_.device->CreateBuffer(&vbd, nullptr, nextVb.put())))
        return false;
    vb_ = std::move(nextVb);
    vbCapacity_ = cap;
    return true;
}

bool GenieRenderer::EnsureSurface(HWND overlayHwnd, int width, int height, bool& surfaceLost)
{
    surfaceLost = false;
    width = std::max(1, width);
    height = std::max(1, height);
    if (overlayHwnd_ == overlayHwnd && swap_ && rtv_ && dcomp_ && target_ && visual_) {
        if (swapW_ == width && swapH_ == height)
            return true;
        rtv_ = nullptr;
        HRESULT hr = swap_->ResizeBuffers(2, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) {
            surfaceLost = IsDeviceRemoved(hr);
            ReleaseSurfaceResources();
            return false;
        }
        winrt::com_ptr<ID3D11Texture2D> back;
        hr = swap_->GetBuffer(0, IID_PPV_ARGS(back.put()));
        if (SUCCEEDED(hr))
            hr = gpu_.device->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
        if (FAILED(hr)) {
            surfaceLost = IsDeviceRemoved(hr);
            ReleaseSurfaceResources();
            return false;
        }
        swapW_ = width;
        swapH_ = height;
        hr = dcomp_->Commit();
        if (FAILED(hr)) {
            surfaceLost = IsDeviceRemoved(hr);
            ReleaseSurfaceResources();
            return false;
        }
        return true;
    }

    ReleaseSurfaceResources();
    if (!overlayHwnd || !IsWindow(overlayHwnd))
        return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = gpu_.factory->CreateSwapChainForComposition(gpu_.device.get(), &desc, nullptr, swap_.put());
    if (FAILED(hr)) {
        surfaceLost = IsDeviceRemoved(hr);
        ReleaseSurfaceResources();
        return false;
    }
    auto dxgi = gpu_.device.as<IDXGIDevice>();
    hr = DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(dcomp_.put()));
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateTargetForHwnd(overlayHwnd, TRUE, target_.put());
    if (SUCCEEDED(hr))
        hr = dcomp_->CreateVisual(visual_.put());
    if (SUCCEEDED(hr))
        hr = visual_->SetContent(swap_.get());
    if (SUCCEEDED(hr))
        hr = target_->SetRoot(visual_.get());
    if (SUCCEEDED(hr))
        hr = dcomp_->Commit();
    winrt::com_ptr<ID3D11Texture2D> back;
    if (SUCCEEDED(hr))
        hr = swap_->GetBuffer(0, IID_PPV_ARGS(back.put()));
    if (SUCCEEDED(hr))
        hr = gpu_.device->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
    if (FAILED(hr)) {
        surfaceLost = IsDeviceRemoved(hr);
        ReleaseSurfaceResources();
        return false;
    }
    overlayHwnd_ = overlayHwnd;
    swapW_ = width;
    swapH_ = height;
    return true;
}

ID3D11ShaderResourceView* GenieRenderer::LookupOrUploadTexture(uint64_t pictureId, const IconImage& image)
{
    if (pictureId != 0) {
        if (const auto it = textures_.find(pictureId); it != textures_.end())
            return it->second.srv.get();
    }
    if (image.Empty() || image.width <= 0 || image.height <= 0)
        return nullptr;

    auto* dev = gpu_.device.get();
    auto* ctx = gpu_.context.get();
    CachedTexture entry;
    const UINT rowPitch = static_cast<UINT>(image.width * 4);
    if (canAutoGenMips_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(image.width);
        td.Height = static_cast<UINT>(image.height);
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, entry.texture.put())) ||
            FAILED(dev->CreateShaderResourceView(entry.texture.get(), nullptr, entry.srv.put())))
            return nullptr;
        ctx->UpdateSubresource(entry.texture.get(), 0, nullptr, image.Data(), rowPitch, 0);
        ctx->GenerateMips(entry.srv.get());
    } else {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(image.width);
        td.Height = static_cast<UINT>(image.height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{image.Data(), rowPitch, 0};
        if (FAILED(dev->CreateTexture2D(&td, &init, entry.texture.put())) ||
            FAILED(dev->CreateShaderResourceView(entry.texture.get(), nullptr, entry.srv.put())))
            return nullptr;
    }
    auto* srv = entry.srv.get();
    if (pictureId != 0)
        textures_.emplace(pictureId, std::move(entry));
    return srv;
}

void GenieRenderer::PruneUnusedTextures(const GenieSceneSnapshot& scene)
{
    std::erase_if(textures_,
                  [&](const auto& kv) { return std::ranges::none_of(scene.flights, [&](const GenieFlightDraw& f) { return f.pictureId == kv.first; }); });
}

void GenieRenderer::PruneFlightTracking(const GenieSceneSnapshot& scene)
{
    std::erase_if(reportedFirstFrames_,
                  [&](uint64_t id) { return std::ranges::none_of(scene.flights, [id](const GenieFlightDraw& f) { return f.flightId == id; }); });
    std::erase_if(reportedPourDone_,
                  [&](uint64_t id) { return std::ranges::none_of(scene.flights, [id](const GenieFlightDraw& f) { return f.flightId == id; }); });
}

GenieRenderer::RenderResult GenieRenderer::RenderIdle(const GenieSceneSnapshot& scene, double nowMs)
{
    reportedFirstFrames_.clear();
    reportedPourDone_.clear();
    if (hasPresentedNonEmpty_ && swap_ && rtv_ && dcomp_ && overlayHwnd_ && IsWindow(overlayHwnd_)) {
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        gpu_.context->ClearRenderTargetView(rtv_.get(), clear);
        HRESULT hr = swap_->Present(0, 0);
        if (SUCCEEDED(hr))
            hr = dcomp_->Commit();
        if (FAILED(hr) && IsDeviceRemoved(hr))
            return {RenderStatus::DeviceLost, false};
        hasPresentedNonEmpty_ = false;
        presentedSeq_ = scene.seq;
        idleSinceMs_ = nowMs;
    }
    if ((swap_ || !textures_.empty()) &&
        (!scene.overlayHwnd || !IsWindow(scene.overlayHwnd) || (idleSinceMs_ > 0.0 && (nowMs - idleSinceMs_) >= kIdleReleaseMs))) {
        ReleaseSurfaceResources();
    }
    return {RenderStatus::Ok, false};
}

void GenieRenderer::BindFramePipeline(int width, int height)
{
    auto* ctx = gpu_.context.get();
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->ClearRenderTargetView(rtv_.get(), clear);
    ID3D11RenderTargetView* rtv = rtv_.get();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(blend_.get(), nullptr, 0xFFFFFFFF);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rasterizer_.get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(inputLayout_.get());
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11SamplerState* samp = sampler_.get();
    ctx->PSSetSamplers(0, 1, &samp);
    ID3D11Buffer* cb = cb_.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
}

bool GenieRenderer::DrawFlight(const GenieFlightDraw& f, const GenieFlightEval& ev, const RECT& overlayBounds, bool& drewFlight, bool& deviceLost)
{
    drewFlight = false;
    deviceLost = false;
    if (!f.image || f.image->Empty() || ev.opacity <= 0.002f)
        return true;

    GenieMeshInput in;
    in.fromRect = f.fromRect;
    in.toRect = f.toRect;
    in.surfaceBounds = overlayBounds;
    in.texWidth = f.image->width;
    in.texHeight = f.image->height;
    in.picWidth = f.picWidth;
    in.picHeight = f.picHeight;
    in.padPx = f.padPx;
    in.t = ev.meshT;
    in.cols = kGenieGridCols;
    in.rows = kGenieGridRows;
    in.direction = f.direction;
    in.inferDirection = f.inferDirection;

    const GenieMeshOutput mesh = BuildGenieMesh(in);
    if (mesh.degenerate || mesh.vertices.empty() || mesh.indices.empty())
        return true;

    ID3D11ShaderResourceView* srv = LookupOrUploadTexture(f.pictureId, *f.image);
    if (!srv) {
        deviceLost = IsDeviceRemoved(S_OK);
        return !deviceLost;
    }
    if (!EnsureIndexBuffer(in.cols, in.rows) || !EnsureVertexBuffer(mesh.vertices.size())) {
        deviceLost = IsDeviceRemoved(S_OK);
        return false;
    }

    auto* ctx = gpu_.context.get();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = ctx->Map(vb_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(mapHr)) {
        deviceLost = IsDeviceRemoved(mapHr);
        return !deviceLost;
    }
    std::memcpy(mapped.pData, mesh.vertices.data(), mesh.vertices.size() * sizeof(GenieVertex));
    ctx->Unmap(vb_.get(), 0);

    const GenieParamsCb params{ev.opacity, {}};
    ctx->UpdateSubresource(cb_.get(), 0, nullptr, &params, 0, 0);

    const UINT stride = sizeof(GenieVertex);
    const UINT offset = 0;
    ID3D11Buffer* vb = vb_.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(ib_.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->DrawIndexed(static_cast<UINT>(mesh.indices.size()), 0, 0);
    drewFlight = true;
    return true;
}

GenieRenderer::RenderResult GenieRenderer::Render(const GenieSceneSnapshot& scene, double nowMs, const FrameNotifyFn& onFramePresented)
{
    if (scene.flights.empty())
        return RenderIdle(scene, nowMs);

    idleSinceMs_ = 0.0;
    PruneFlightTracking(scene);
    PruneUnusedTextures(scene);

    const int width = static_cast<int>(scene.overlayBounds.right - scene.overlayBounds.left);
    const int height = static_cast<int>(scene.overlayBounds.bottom - scene.overlayBounds.top);
    if (!scene.overlayHwnd || !IsWindow(scene.overlayHwnd) || width <= 0 || height <= 0)
        return {RenderStatus::Ok, false};

    std::vector<GenieFlightEval> evals;
    evals.reserve(scene.flights.size());
    bool anyAnimating = false;
    bool anyPendingNotify = false;
    for (const GenieFlightDraw& f : scene.flights) {
        const GenieFlightEval ev = EvaluateGenieFlightAt(f, nowMs);
        anyAnimating |= ev.animating;
        anyPendingNotify |= !reportedFirstFrames_.contains(f.flightId) || (ev.pourFinished && !reportedPourDone_.contains(f.flightId));
        evals.push_back(ev);
    }

    if (scene.seq == presentedSeq_ && !anyAnimating && !anyPendingNotify)
        return {RenderStatus::Ok, false};

    if (!EnsurePipeline())
        return {IsDeviceRemoved(S_OK) ? RenderStatus::DeviceLost : RenderStatus::Ok, false};

    bool surfaceLost = false;
    if (!EnsureSurface(scene.overlayHwnd, width, height, surfaceLost))
        return {surfaceLost ? RenderStatus::DeviceLost : RenderStatus::Ok, false};

    std::vector<bool> drewFlights(scene.flights.size(), false);
    BindFramePipeline(width, height);
    for (size_t i = 0; i < scene.flights.size(); ++i) {
        bool drew = false;
        bool deviceLost = false;
        if (!DrawFlight(scene.flights[i], evals[i], scene.overlayBounds, drew, deviceLost))
            return {deviceLost ? RenderStatus::DeviceLost : RenderStatus::Ok, false};
        drewFlights[i] = drew;
    }

    auto* ctx = gpu_.context.get();
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    HRESULT hr = swap_->Present(0, 0);
    if (SUCCEEDED(hr))
        hr = dcomp_->Commit();
    if (FAILED(hr)) {
        if (IsDeviceRemoved(hr)) {
            Log(L"genie: hardware render device lost (0x%08X)", static_cast<unsigned>(hr));
            return {RenderStatus::DeviceLost, false};
        }
        Log(L"genie: hardware present/commit failed (0x%08X); releasing overlay surface", static_cast<unsigned>(hr));
        ReleaseSurfaceResources();
        return {RenderStatus::Ok, false};
    }

    presentedSeq_ = scene.seq;
    hasPresentedNonEmpty_ = true;

    for (size_t i = 0; i < scene.flights.size(); ++i) {
        const GenieFlightDraw& f = scene.flights[i];
        const GenieFlightEval& ev = evals[i];
        const GeniePresentedFlags flags = EvaluateGenieFlightNotify(f.flightId, drewFlights[i], ev.pourFinished, reportedFirstFrames_, reportedPourDone_);
        if (flags.notify && onFramePresented)
            onFramePresented(f.flightId, nowMs, flags.pourDone);
    }

    return {RenderStatus::Ok, anyAnimating};
}

} // namespace atrium
