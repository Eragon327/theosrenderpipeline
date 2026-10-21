#include "NeuralObjectControl.h"
#include "NeuralObjectGuideShader.h"
#include <d3dcompiler.h>
#include <string>

namespace TheosRenderPipeline::NeuralRendering
{
    using Microsoft::WRL::ComPtr;
    namespace {
        struct Constants {
            UINT width,height,guideWidth,guideHeight;
            std::array<float,2> offset;
            UINT peripheral,enabled;
            std::array<float,4> defaults;
            std::array<std::array<float,4>,4> rules;
        };
        static_assert(sizeof(Constants)==112);
    }
    HRESULT ObjectControl::Initialize(ID3D12Device* device)
    {
        if(root_)return S_OK;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,sizeof(Constants)/4};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=params;
        ComPtr<ID3DBlob> blob,code,error;
        auto hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(FAILED(hr))return hr;
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
        if(FAILED(hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))))return hr;
        const std::string shader=std::string(R"(
cbuffer Params:register(b0){uint Width,Height,GuideWidth,GuideHeight;float2 Offset;uint Peripheral,Enabled;float4 Defaults;float4 Rules[4];};
Texture2D<float> Objects:register(t0);Texture2D<float> Depth:register(t1);RWTexture2D<float4> Result:register(u0);
)")+kObjectGuideFunctions+R"(
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
    if(p.x>=Width||p.y>=Height)return;
    uint group=ClassifiedObject(p.xy+0.5,uint2(Width,Height),uint2(GuideWidth,GuideHeight),Offset,Peripheral);
    // Vendor RGB contract: intensity, tone, structure. Keep A at 1.
    float4 value=Defaults;
    if(group!=0&&(Enabled&(1u<<(group-1)))!=0)value=Rules[group-1];
    Result[p.xy]=value;
})";
        if(FAILED(hr=D3DCompile(shader.data(),shader.size(),"NR-object-control",nullptr,nullptr,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error)))return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if(FAILED(hr=device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline))))return hr;
        root_=std::move(root);pipeline_=std::move(pipeline);return S_OK;
    }
    HRESULT ObjectControl::Record(ID3D12Device* device,ID3D12GraphicsCommandList* list,std::size_t slot,unsigned pass,
        const ObjectPassTuning& settings,const Tuning& tuning,ID3D12Resource* objects,ID3D12Resource* depth,
        UINT width,UINT height,bool peripheral,std::array<float,2> offset)
    {
        if(!device||!list||slot>=heaps_.size()||pass>=outputs_.size()||!width||!height||!objects||!depth||objects==depth||
            !std::isfinite(offset[0])||!std::isfinite(offset[1]))return E_INVALIDARG;
        const auto m=objects->GetDesc(),d=depth->GetDesc();
        for(const auto& v:{m,d})if(v.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||!v.Width||!v.Height||v.Width>UINT_MAX||
            v.DepthOrArraySize!=1||v.MipLevels!=1||v.SampleDesc.Count!=1||v.Format!=DXGI_FORMAT_R32_FLOAT)return E_INVALIDARG;
        if(m.Width!=d.Width||m.Height!=d.Height)return E_INVALIDARG;
        auto hr=Initialize(device);if(FAILED(hr))return hr;
        auto& output=outputs_[pass];
        if(output&&(output->GetDesc().Width!=width||output->GetDesc().Height!=height))return E_INVALIDARG; // retire NeuralPass before resizing
        if(!output){
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
            desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            if(FAILED(hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&output))))return hr;
        }
        Constants c{width,height,static_cast<UINT>(d.Width),d.Height,offset,static_cast<UINT>(peripheral),0,
            {tuning.intensity,tuning.localToneStrength,tuning.localStructureStrength,1},{}};
        for(unsigned i=0;i<4;++i){const auto& g=settings.groups[i];if(g.enabled)c.enabled|=1u<<i;c.rules[i]={tuning.intensity,g.tone,g.structure,1};}
        if(!heaps_[slot][pass]){
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,3,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
            if(FAILED(hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heaps_[slot][pass]))))return hr;
        }
        auto* heap=heaps_[slot][pass].Get();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(auto* r:{objects,depth}){
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels=1;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            device->CreateShaderResourceView(r,&srv,cpu);cpu.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        D3D12_RESOURCE_BARRIER barriers[3]{};ID3D12Resource* resources[]{objects,depth,output.Get()};
        for(unsigned i=0;i<3;++i){auto& b=barriers[i];b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resources[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,i==2?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};}
        list->ResourceBarrier(3,barriers);list->SetDescriptorHeaps(1,&heap);list->SetComputeRootSignature(root_.Get());list->SetPipelineState(pipeline_.Get());
        list->SetComputeRoot32BitConstants(0,sizeof(c)/4,&c,0);list->SetComputeRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((width+7)/8,(height+7)/8,1);
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
        list->ResourceBarrier(3,barriers);return S_OK;
    }
}
