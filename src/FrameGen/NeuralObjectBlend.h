#pragma once
#include "SourceDLSSGInterop.h"
#include "NeuralObjectMask.h"
#include <array>

namespace TheosRenderPipeline::NeuralRendering
{
    struct ObjectBlendConstants
    {
        UINT width{}, height{}, guideWidth{}, guideHeight{};
        std::array<float,4> strength{};
        std::array<float,2> guideOffset{}, padding{};
    };
    static_assert(sizeof(ObjectBlendConstants) == 48);
    inline constexpr char kObjectBlendShader[] = R"(
cbuffer Params:register(b0) { uint Width,Height,GuideWidth,GuideHeight; float4 Strength; float2 GuideOffset,Padding; };
Texture2D<float4> Original:register(t0);
Texture2D<float4> Neural:register(t1);
Texture2D<float2> Objects:register(t2);
Texture2D<float> Depth:register(t3);
RWTexture2D<float4> Result:register(u0);
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
    if(p.x>=Width || p.y>=Height) return;
    // Capture and depth share the active render grid. NR's final resolve has
    // already undone model downscaling/peripheral compression at this point.
    // Before SR the source is jittered too: offset is zero. After SR add the
    // existing SDK pixel jitter to sample the corresponding raw guide pixel.
    int2 coordinate=int2(floor((float2(p.xy)+0.5)*float2(GuideWidth,GuideHeight)/float2(Width,Height)+GuideOffset));
    uint2 q=uint2(clamp(coordinate,int2(0,0),int2(GuideWidth,GuideHeight)-1));
    float2 object=Objects.Load(int3(q,0)); float depth=Depth.Load(int3(q,0));
    uint group=(uint)round(object.x);
    bool valid=isfinite(object.x) && isfinite(object.y) && isfinite(depth) && group>=1 && group<=4 && object.x==float(group) &&
        abs(object.y-depth)<=max(0.00000024,abs(depth)*0.000002);
    float amount=valid ? Strength[clamp(group,1,4)-1] : 1;
    float4 n=Neural.Load(int3(p.xy,0)), original=Original.Load(int3(p.xy,0));
    // Exact endpoints preserve all channels of the selected source.
    Result[p.xy]=amount<=0 ? original : amount>=1 ? n : float4(lerp(original.rgb,n.rgb,amount),original.a);
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
