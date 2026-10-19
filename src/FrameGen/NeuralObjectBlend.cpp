#include "NeuralObjectBlend.h"
#include <d3dcompiler.h>

namespace TheosRenderPipeline::NeuralRendering
{
    using Microsoft::WRL::ComPtr;
    HRESULT ObjectBlend::Initialize(ID3D12Device* device)
    {
        if (root_) { return S_OK; }
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,4}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants = {0,0,sizeof(ObjectBlendConstants)/4};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2,ranges};
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=2; desc.pParameters=params;
        ComPtr<ID3DBlob> signature, code, errors;
        auto hr = D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors);
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(hr) || FAILED(hr=device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root))) ||
            FAILED(hr=D3DCompile(kObjectBlendShader.data(),kObjectBlendShader.size(),"NR-object-blend",nullptr,nullptr,
                "main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))) { return hr; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root.Get();
        pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if (FAILED(hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&pipeline)))) { return hr; }
        for (auto& heap:heaps_) {
            D3D12_DESCRIPTOR_HEAP_DESC h{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,5,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if (FAILED(hr=device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&heap)))) { return hr; }
        }
        root_=std::move(root); pipeline_=std::move(pipeline); return S_OK;
    }
    HRESULT ObjectBlend::Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot,
        const ObjectMaskSettings& settings, ID3D12Resource* original, ID3D12Resource* neural,
        ID3D12Resource* objects, ID3D12Resource* depth, ID3D12Resource* output, std::array<float,2> guideOffset)
    {
        if (!std::isfinite(guideOffset[0]) || !std::isfinite(guideOffset[1]) || !device || !list || slot>=heaps_.size() || !settings.enabled || !original || !neural || !objects || !depth || !output ||
            output==original || output==neural || output==objects || output==depth) { return E_INVALIDARG; }
        const auto d=output->GetDesc(), g=depth->GetDesc(), m=objects->GetDesc();
        for(auto* r:{original,neural,objects,depth,output}) {
            const auto t=r->GetDesc();
            if(t.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || t.DepthOrArraySize!=1 || t.MipLevels!=1 ||
                t.SampleDesc.Count!=1 || !t.Width || !t.Height || t.Width>UINT_MAX) { return E_INVALIDARG; }
        }
        if(d.Width!=original->GetDesc().Width || d.Height!=original->GetDesc().Height ||
            d.Format!=original->GetDesc().Format || d.Width!=neural->GetDesc().Width || d.Height!=neural->GetDesc().Height ||
            d.Format!=neural->GetDesc().Format || m.Width!=g.Width || m.Height!=g.Height ||
            m.Format!=DXGI_FORMAT_R32_FLOAT || g.Format!=DXGI_FORMAT_R32_FLOAT) { return E_INVALIDARG; }
        auto hr=Initialize(device); if(FAILED(hr)) { return hr; }
        auto transition=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
        };
        const std::array<ID3D12Resource*,4> inputs{original,neural,objects,depth};
        auto* heap=heaps_[slot].Get(); auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(unsigned i=0;i<inputs.size();++i) {
            if(std::find(inputs.begin(),inputs.begin()+i,inputs[i])==inputs.begin()+i) {
                transition(inputs[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=inputs[i]->GetDesc().Format;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(inputs[i],&view,handle); handle.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=d.Format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output,nullptr,&uav,handle);
        transition(output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ObjectBlendConstants constants{static_cast<UINT>(d.Width),d.Height,static_cast<UINT>(g.Width),g.Height,
            SanitizeObjectMask(settings).strength,guideOffset,{},static_cast<UINT>(settings.preview),settings.previewGroup,{}};
        list->SetDescriptorHeaps(1,&heap); list->SetComputeRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
        list->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);
        list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((constants.width+7)/8,(constants.height+7)/8,1);
        transition(output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        for(unsigned i=0;i<inputs.size();++i) {
            if(std::find(inputs.begin(),inputs.begin()+i,inputs[i])==inputs.begin()+i) {
                transition(inputs[i],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
            }
        }
        return S_OK;
    }
}
