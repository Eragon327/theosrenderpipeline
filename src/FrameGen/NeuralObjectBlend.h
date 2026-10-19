#pragma once
#include "SourceDLSSGInterop.h"
#include "NeuralObjectMask.h"
#include "NeuralObjectGuideShader.h"
#include <string>
#include <array>

namespace TheosRenderPipeline::NeuralRendering
{
    struct ObjectBlendConstants
    {
        UINT width{}, height{}, guideWidth{}, guideHeight{};
        std::array<float,4> strength{};
        std::array<float,2> guideOffset{}, padding{};
        UINT preview{},previewGroup{},reserved[2]{};
    };
    static_assert(sizeof(ObjectBlendConstants) == 64);
    inline const std::string kObjectBlendShader = std::string(R"(
cbuffer Params:register(b0) { uint Width,Height,GuideWidth,GuideHeight; float4 Strength; float2 GuideOffset,Padding; uint Preview,PreviewGroup;uint2 Reserved; };
Texture2D<float4> Original:register(t0);
Texture2D<float4> Neural:register(t1);
Texture2D<float> Objects:register(t2);
Texture2D<float> Depth:register(t3);
RWTexture2D<float4> Result:register(u0);
)")+kObjectGuideFunctions+R"(
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=Width || p.y>=Height) return;
    // Capture and depth share the active render grid. NR's final resolve has
    // already undone model downscaling/peripheral compression at this point.
    // Before SR the source is jittered too: offset is zero. After SR add the
    // existing SDK pixel jitter to sample the corresponding raw guide pixel.
    uint group=ClassifiedObject(p.xy+0.5,uint2(Width,Height),uint2(GuideWidth,GuideHeight),GuideOffset,0);
    float amount=group!=0 ? Strength[clamp(group,1,4)-1] : 1;
    float4 n=Neural.Load(int3(p.xy,0)), original=Original.Load(int3(p.xy,0));
    // Exact endpoints preserve all channels of the selected source.
    float4 result=amount<=0 ? original : amount>=1 ? n : float4(lerp(original.rgb,n.rgb,amount),original.a);
    bool selected=group!=0&&(PreviewGroup==0||PreviewGroup==group);
    float3 tint=group==1?float3(0.2,0.9,0.4):group==2?float3(0.25,0.55,1):group==3?float3(1,0.65,0.15):float3(0.8,0.3,0.95);
    if(Preview==1&&selected)result.rgb=lerp(result.rgb,tint,0.6);
    if(Preview==2)result.rgb=selected?tint:float3(0,0,0);
    Result[p.xy]=result;
})";
    class ObjectBlend
    {
    public:
        // One dispatch per source frame/retired slot. Inputs/output enter and
        // leave COMMON; no queue submission or wait is introduced here.
        HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
            const ObjectMaskSettings& settings, ID3D12Resource* original, ID3D12Resource* neural,
            ID3D12Resource* objects, ID3D12Resource* depth, ID3D12Resource* output,
            std::array<float,2> guideOffset = {});
    private:
        HRESULT Initialize(ID3D12Device* device);
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
        std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, SourceDLSSG::kCommandSlots> heaps_;
    };
}
