#pragma once
#include "NeuralObjectMask.h"
#include "D3D11FrameCopy.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>

namespace TheosRenderPipeline::NeuralRendering
{
    // Immediate-context-only, D3D11-owned capture. Only a copied shared guide is
    // read by D3D12, with the same retirement dependency as motion/depth guides.
    class ObjectCapture
    {
    public:
        // Why the latest Record call kept the ordinary path (S_FALSE).
        enum class Skip : std::uint8_t { None, Input, SideEffects, NoDepth, DepthTarget, DepthShape, Viewport, DepthState, Blend, StateCache, Count };
        static constexpr const char* kSkipNames[]{ "none", "input", "side-effects", "no-depth", "depth-target", "depth-shape",
            "viewport", "depth-state", "blend", "state-cache" };
        Skip LastSkip() const { return lastSkip_; }
        // The values the latest viewport or depth-target check rejected, for diagnostics.
        D3D11_VIEWPORT LastViewport() const { return lastViewport_; }
        UINT LastViewportCount() const { return lastViewportCount_; }
        HRESULT Record(ID3D11DeviceContext* context, ObjectGroup group, std::uint64_t frame,
            FrameExtent active, ID3D11Resource* mainDepth, ID3D11Resource* copiedDepth,
            const std::function<void()>& draw);
        ID3D11Texture2D* Current(std::uint64_t frame) const { return frame == frame_ && draws_ ? mask_.Get() : nullptr; }
        void Reset();
        std::uint64_t Draws() const { return draws_; }
    private:
        HRESULT Initialize(ID3D11Device* device, UINT width, UINT height);
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> mask_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> shader_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
        // Keys retain their COM owner, preventing address reuse within the cache.
        struct DepthState { Microsoft::WRL::ComPtr<ID3D11DepthStencilState> original, readOnly; };
        std::unordered_map<ID3D11DepthStencilState*, DepthState> depths_;
        std::uint64_t frame_{(std::numeric_limits<std::uint64_t>::max)()}, draws_{};
        Skip lastSkip_{Skip::None};
        D3D11_VIEWPORT lastViewport_{};
        UINT lastViewportCount_{};
    };
}
