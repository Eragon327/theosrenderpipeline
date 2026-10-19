#include "NeuralObjectCapture.h"
#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstring>

namespace TheosRenderPipeline::NeuralRendering
{
    using Microsoft::WRL::ComPtr;
    void ObjectCapture::Reset()
    {
        mask_.Reset(); target_.Reset(); shader_.Reset(); constants_.Reset(); blend_.Reset();
        depths_.clear(); device_.Reset(); frame_ = (std::numeric_limits<std::uint64_t>::max)(); draws_ = 0;
    }
    HRESULT ObjectCapture::Initialize(ID3D11Device* device, UINT width, UINT height)
    {
        if (device_.Get() != device) { Reset(); device_ = device; }
        if (mask_ && target_) {
            D3D11_TEXTURE2D_DESC d{}; mask_->GetDesc(&d);
            if (d.Width == width && d.Height == height) { return S_OK; }
            // These resources have only immediate-context users; shared copies
            // have separate ownership in the host.
            mask_.Reset(); target_.Reset();
        }
        HRESULT hr{};
        if (!shader_) {
            constexpr char source[] = "cbuffer Params:register(b0){float Group;float3 Pad;}"
                "float2 main(float4 p:SV_Position):SV_Target{return float2(Group,p.z);}";
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(hr = D3DCompile(source, sizeof(source)-1, "NR-object-capture", nullptr, nullptr,
                    "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
                FAILED(hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_))) { return hr; }
        }
        if (!constants_) {
            D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 16; buffer.Usage = D3D11_USAGE_DYNAMIC;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER; buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(hr = device->CreateBuffer(&buffer, nullptr, &constants_))) { return hr; }
        }
        if (!blend_) {
            D3D11_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(hr = device->CreateBlendState(&blend, &blend_))) { return hr; }
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1; desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32_FLOAT; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(hr = device->CreateTexture2D(&desc, nullptr, &texture)) ||
            FAILED(hr = device->CreateRenderTargetView(texture.Get(), nullptr, &target))) { return hr; }
        mask_ = std::move(texture); target_ = std::move(target);
        frame_ = (std::numeric_limits<std::uint64_t>::max)(); draws_ = 0;
        return S_OK;
    }
    HRESULT ObjectCapture::Record(ID3D11DeviceContext* context, ObjectGroup group, std::uint64_t frame,
        FrameExtent active, ID3D11Resource* mainDepth, ID3D11Resource* copiedDepth, const std::function<void()>& draw)
    {
        if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            group == ObjectGroup::World || static_cast<unsigned>(group) > 4 || !active.width || !active.height) { lastSkip_ = Skip::Input; return S_FALSE; }
        // Replaying must not append stream-output vertices or disturb a pixel
        // UAV binding. These uncommon draws keep the ordinary NR path.
        ID3D11Buffer* so[4]{}; context->SOGetTargets(4,so);
        bool sideEffects{};
        for (auto* buffer : so) { if (buffer) { sideEffects = true; buffer->Release(); } }
        ID3D11UnorderedAccessView* uavs[8]{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
        for (auto* view : uavs) { if (view) { sideEffects = true; view->Release(); } }
        if (sideEffects) { lastSkip_ = Skip::SideEffects; return S_FALSE; }
        ComPtr<ID3D11DepthStencilView> dsv;
        std::array<ComPtr<ID3D11RenderTargetView>, 8> targets;
        ID3D11RenderTargetView* rawTargets[8]{};
        context->OMGetRenderTargets(8, rawTargets, &dsv);
        unsigned targetCount{};
        for (unsigned i=0; i<8; ++i) { targets[i].Attach(rawTargets[i]); if (rawTargets[i]) { targetCount=i+1; } }
        if (!dsv) { lastSkip_ = Skip::NoDepth; return S_FALSE; }
        ComPtr<ID3D11Resource> depthResource; dsv->GetResource(&depthResource);
        if (depthResource.Get() != mainDepth && depthResource.Get() != copiedDepth) { lastSkip_ = Skip::DepthTarget; return S_FALSE; }
        ComPtr<ID3D11Texture2D> depthTexture;
        if (FAILED(depthResource.As(&depthTexture))) { lastSkip_ = Skip::DepthShape; return S_FALSE; }
        D3D11_TEXTURE2D_DESC depthDesc{}; depthTexture->GetDesc(&depthDesc);
        if (depthDesc.SampleDesc.Count != 1 || depthDesc.ArraySize != 1 ||
            depthDesc.Width < active.width || depthDesc.Height < active.height) { lastSkip_ = Skip::DepthShape; return S_FALSE; }
        D3D11_VIEWPORT viewport[16]{}; UINT viewportCount=16;
        context->RSGetViewports(&viewportCount, viewport);
        // Skyrim scales its viewport by the render ratio, so it can be fractional
        // (5120 x 2/3 = 3413.33) where the render extent is rounded.
        auto within = [](float value, UINT extent) { return std::abs(value - float(extent)) < 1.0f; };
        if (viewportCount != 1 || viewport[0].TopLeftX != 0 || viewport[0].TopLeftY != 0 ||
            !within(viewport[0].Width, active.width) || !within(viewport[0].Height, active.height)) {
            lastSkip_ = Skip::Viewport; lastViewport_ = viewport[0]; lastViewportCount_ = viewportCount; return S_FALSE;
        }
        ComPtr<ID3D11DepthStencilState> depthState; UINT stencil{};
        context->OMGetDepthStencilState(&depthState, &stencil);
        D3D11_DEPTH_STENCIL_DESC state{};
        if (depthState) { depthState->GetDesc(&state); }
        else { state.DepthEnable = TRUE; state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; state.DepthFunc = D3D11_COMPARISON_LESS; }
        // Replaying a constant PS cannot reproduce alpha discard, pixel-depth
        // output or transparency. The game adapter excludes alpha-test passes;
        // here require a depth-tested draw. Skyrim lays opaque depth down before
        // its lighting draws, which then test against it without writing. The
        // read-only EQUAL replay marks the same visible pixels either way, and a
        // draw whose depth was never written marks none.
        if (!state.DepthEnable) { lastSkip_ = Skip::DepthState; return S_FALSE; }
        ComPtr<ID3D11BlendState> blend; float factors[4]{}; UINT sampleMask{};
        context->OMGetBlendState(&blend, factors, &sampleMask);
        if (blend) {
            D3D11_BLEND_DESC blendDesc{}; blend->GetDesc(&blendDesc);
            if (blendDesc.AlphaToCoverageEnable || blendDesc.RenderTarget[0].BlendEnable) { lastSkip_ = Skip::Blend; return S_FALSE; }
        }
        ComPtr<ID3D11Device> device; context->GetDevice(&device);
        auto hr = Initialize(device.Get(), depthDesc.Width, depthDesc.Height);
        if (FAILED(hr)) { return hr; }
        if (frame_ != frame) {
            const float clear[4]{}; context->ClearRenderTargetView(target_.Get(), clear);
            frame_ = frame; draws_ = 0;
        }
        auto found = depths_.find(depthState.Get());
        if (found == depths_.end()) {
            if (depths_.size() >= 64) { lastSkip_ = Skip::StateCache; return S_FALSE; }
            state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; state.DepthFunc = D3D11_COMPARISON_EQUAL;
            state.StencilWriteMask = 0;
            if (!state.StencilEnable) {
                state.FrontFace.StencilFunc = state.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
            }
            state.FrontFace.StencilFailOp = state.FrontFace.StencilDepthFailOp = state.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            state.BackFace.StencilFailOp = state.BackFace.StencilDepthFailOp = state.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            DepthState replacement; replacement.original = depthState;
            if (FAILED(hr = device->CreateDepthStencilState(&state, &replacement.readOnly))) { return hr; }
            found = depths_.emplace(depthState.Get(), std::move(replacement)).first;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(hr = context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) { return hr; }
        const float values[4]{float(static_cast<unsigned>(group)),0,0,0};
        std::memcpy(mapped.pData, values, sizeof(values)); context->Unmap(constants_.Get(), 0);
        ComPtr<ID3D11PixelShader> shader; ID3D11ClassInstance* classes[256]{}; UINT count=256;
        context->PSGetShader(&shader, classes, &count);
        ComPtr<ID3D11Buffer> buffer; context->PSGetConstantBuffers(0, 1, &buffer);
        auto* target = target_.Get(); context->OMSetRenderTargets(1, &target, dsv.Get());
        context->OMSetDepthStencilState(found->second.readOnly.Get(), stencil);
        context->OMSetBlendState(blend_.Get(), nullptr, sampleMask);
        context->PSSetShader(shader_.Get(), nullptr, 0);
        auto* constants = constants_.Get(); context->PSSetConstantBuffers(0, 1, &constants);
        draw();
        context->PSSetShader(shader.Get(), classes, count);
        for (UINT i=0; i<count; ++i) { if (classes[i]) { classes[i]->Release(); } }
        auto* previousBuffer = buffer.Get(); context->PSSetConstantBuffers(0, 1, &previousBuffer);
        context->OMSetRenderTargets(targetCount, rawTargets, dsv.Get());
        context->OMSetDepthStencilState(depthState.Get(), stencil);
        context->OMSetBlendState(blend.Get(), factors, sampleMask);
        ++draws_; lastSkip_ = Skip::None; return S_OK;
    }
}
