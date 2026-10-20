#pragma once
#include "NeuralObjectMask.h"
#include "NeuralRenderingTuning.h"
#include "SourceDLSSGInterop.h"

namespace TheosRenderPipeline::NeuralRendering
{
    // A separate output and descriptor table for each pass; input readers retire
    // with the enclosing NeuralPass. Resources enter and leave COMMON.
    class ObjectControl
    {
    public:
        HRESULT Record(ID3D12Device*,ID3D12GraphicsCommandList*,std::size_t slot,unsigned pass,
            const ObjectPassTuning&,const Tuning&,ID3D12Resource* objects,ID3D12Resource* depth,
            UINT width,UINT height,bool peripheral,std::array<float,2> guideOffset);
        ID3D12Resource* Output(unsigned pass) const { return pass<2 ? outputs_[pass].Get() : nullptr; }
    private:
        HRESULT Initialize(ID3D12Device*);
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> outputs_;
        std::array<std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>,2>,SourceDLSSG::kCommandSlots> heaps_;
    };
}
