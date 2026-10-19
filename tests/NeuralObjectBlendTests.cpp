#include "FrameGen/NeuralObjectBlend.h"
#include "FrameGen/NeuralObjectControl.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
#include <cmath>
using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline::NeuralRendering;
static void Require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static void Check(HRESULT hr,const char* why){if(FAILED(hr)){std::fprintf(stderr,"FAIL: %s hr=%08lX\n",why,(unsigned long)hr);std::exit(1);}}
namespace { constexpr UINT W=32,H=24;
    struct GPU
    {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        UINT64 value{};
        std::vector<ComPtr<ID3D12Resource>> keep;

        void Submit()
        {
            Check(list->Close(), "close list");
            ID3D12CommandList* lists[]{ list.Get() };
            queue->ExecuteCommandLists(1, lists);
            Check(queue->Signal(fence.Get(), ++value), "signal");
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Check(fence->SetEventOnCompletion(value, event), "fence event");
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
            Check(device->GetDeviceRemovedReason(), "device not removed");
            Check(allocator->Reset(), "reset allocator");
            Check(list->Reset(allocator.Get(), nullptr), "reset list");
            keep.clear();
        }

        ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, bool output=false, UINT width=W, UINT height=H)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width; desc.Height = height;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
            desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> texture;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(&texture)), "texture");
            return texture;
        }

        static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
        {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
            list->ResourceBarrier(1, &barrier);
        }

        ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type)
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
            ComPtr<ID3D12Resource> buffer;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&buffer)), "buffer");
            return buffer;
        }

        void Upload(ID3D12Resource* texture, const std::vector<float>& pixels, UINT channels)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
            const auto desc = texture->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            auto upload = Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
            std::uint8_t* mapped{};
            Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
            for (UINT y = 0; y < desc.Height; ++y) { std::memcpy(mapped + y * footprint.Footprint.RowPitch, &pixels[y * desc.Width * channels], desc.Width * channels * 4); }
            upload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            src.PlacedFootprint = footprint;
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            keep.push_back(upload);
        }

        // Returns rows of raw texels (4 or 8 bytes each).
        std::vector<std::uint8_t> Read(ID3D12Resource* texture, UINT bytesPerTexel)
        {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 total{};
            const auto desc = texture->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
            auto readback = Buffer(total, D3D12_HEAP_TYPE_READBACK);
            D3D12_TEXTURE_COPY_LOCATION src{ texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {} };
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {} };
            dst.PlacedFootprint = footprint;
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Barrier(list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            keep.push_back(readback);
            Submit();
            std::vector<std::uint8_t> out(desc.Width * desc.Height * bytesPerTexel);
            std::uint8_t* mapped{};
            D3D12_RANGE range{ 0, static_cast<SIZE_T>(total) };
            Check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "map readback");
            for (UINT y = 0; y < desc.Height; ++y) { std::memcpy(&out[y * desc.Width * bytesPerTexel], mapped + y * footprint.Footprint.RowPitch, desc.Width * bytesPerTexel); }
            readback->Unmap(0, nullptr);
            return out;
        }
    };

}
int main(){
    GPU gpu; ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&gpu.device)),"device");
    D3D12_COMMAND_QUEUE_DESC q{};Check(gpu.device->CreateCommandQueue(&q,IID_PPV_ARGS(&gpu.queue)),"queue");
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&gpu.allocator)),"allocator");
    Check(gpu.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,gpu.allocator.Get(),nullptr,IID_PPV_ARGS(&gpu.list)),"list");
    Check(gpu.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gpu.fence)),"fence");
    ObjectMaskSettings settings;settings.enabled=true;settings.strength={0,0.25f,0.5f,1};
    auto sanitized=settings;sanitized.strength={-1,2,std::numeric_limits<float>::quiet_NaN(),0.75f};
    Require(SanitizeObjectMask(sanitized).strength==std::array<float,4>{0,1,1,0.75f},"sanitize invalid tuning");
    Require(ObjectStrength(settings,ObjectGroup::World)==1,"world unchanged");
    ObjectBlend blend;
    for(unsigned iteration=0;iteration<36;++iteration){
        settings.preview=static_cast<ObjectPreview>(iteration/12);settings.previewGroup=iteration%2?2:0;
        const UINT gw=iteration%2 ? W/2:W, gh=iteration%2 ? H/2:H;
        const std::array<float,2> offset{iteration%3==0?0.0f:iteration%3==1?0.75f:-0.75f,iteration%2?0.375f:-0.375f};
        std::vector<float> original(W*H*4),neural(W*H*4),objects(gw*gh),depth(gw*gh,0.5f);
        for(UINT i=0;i<W*H;++i){ original[i*4]=0.25f;original[i*4+1]=-0.5f;original[i*4+2]=4;original[i*4+3]=0.75f;
            neural[i*4]=1.25f;neural[i*4+1]=0.5f;neural[i*4+2]=8;neural[i*4+3]=0.25f; }
        // Packed groups 0..7: 0 is unclassified and 5..7 are out of range.
        for(UINT y=0;y<gh;++y)for(UINT x=0;x<gw;++x){unsigned i=y*gw+x; auto group=static_cast<ObjectGroup>(x%8);float objectDepth=0.5f;
            if(y%4==1)depth[i]=0.25f; // a later opaque occluder invalidates the group
            if(y%4==2)objectDepth=std::numeric_limits<float>::quiet_NaN();
            if(y%4==3)objectDepth=0.5f+0.001f; // a different surface at this pixel
            objects[i]=PackObjectPixel(group,objectDepth); }
        auto a=gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT),b=gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT);
        auto m=gpu.Texture(DXGI_FORMAT_R32_FLOAT,false,gw,gh),d=gpu.Texture(DXGI_FORMAT_R32_FLOAT,false,gw,gh);
        auto output=gpu.Texture(DXGI_FORMAT_R32G32B32A32_FLOAT,true);
        gpu.Upload(a.Get(),original,4);gpu.Upload(b.Get(),neural,4);gpu.Upload(m.Get(),objects,1);gpu.Upload(d.Get(),depth,1);gpu.Submit();
        Check(blend.Record(gpu.device.Get(),gpu.list.Get(),iteration%TheosRenderPipeline::SourceDLSSG::kCommandSlots,
            settings,a.Get(),b.Get(),m.Get(),d.Get(),output.Get(),offset),"production object blend");
        auto bytes=gpu.Read(output.Get(),16);std::vector<float> pixels(W*H*4);std::memcpy(pixels.data(),bytes.data(),bytes.size());
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){
            unsigned qx=unsigned(std::clamp(int(std::floor((x+0.5f)*gw/W+offset[0])),0,int(gw)-1)),
                qy=unsigned(std::clamp(int(std::floor((y+0.5f)*gh/H+offset[1])),0,int(gh)-1)),i=y*W+x;
            unsigned group=qx%8;if(qy%4!=0||group>4)group=0;
            float amount=group?settings.strength[group-1]:1;
            for(unsigned c=0;c<4;++c){float expected=amount==0?original[i*4+c]:amount==1?neural[i*4+c]:c==3?original[i*4+c]:original[i*4+c]+(neural[i*4+c]-original[i*4+c])*amount;
                const bool selected=group&&(settings.previewGroup==0||settings.previewGroup==group);
                const std::array<std::array<float,3>,4> colors{{{.2f,.9f,.4f},{.25f,.55f,1},{1,.65f,.15f},{.8f,.3f,.95f}}};
                if(c<3&&settings.preview==ObjectPreview::Overlay&&selected)expected+=(colors[group-1][c]-expected)*.6f;
                if(c<3&&settings.preview==ObjectPreview::Classes)expected=selected?colors[group-1][c]:0;
                Require(std::abs(pixels[i*4+c]-expected)<.000001f,"all channels match independent group/occlusion/preview oracle");}
        }
        Require(blend.Record(gpu.device.Get(),gpu.list.Get(),0,settings,a.Get(),b.Get(),m.Get(),d.Get(),a.Get())==E_INVALIDARG,"alias refused before recording");
    }
    // Read the actual shader-generated vendor controls, including independent
    // pass resources and the nonlinear peripheral grid. No vendor model double
    // is used for this pixel oracle.
    for(bool peripheral:{false,true})for(unsigned shift=0;shift<3;++shift){
        constexpr UINT gw=16,gh=12;
        const std::array<float,2> offset{shift==0?0.f:shift==1?.75f:-.75f,-.375f};
        std::vector<float> objects(gw*gh),depth(gw*gh,.5f);
        for(unsigned y=0;y<gh;++y)for(unsigned x=0;x<gw;++x){
            const auto i=y*gw+x;objects[i]=PackObjectPixel(static_cast<ObjectGroup>(x%6),.5f);
            if(y%3==1)depth[i]=.25f;if(y%3==2)objects[i]=std::numeric_limits<float>::quiet_NaN();
        }
        auto m=gpu.Texture(DXGI_FORMAT_R32_FLOAT,false,gw,gh),d=gpu.Texture(DXGI_FORMAT_R32_FLOAT,false,gw,gh);
        gpu.Upload(m.Get(),objects,1);gpu.Upload(d.Get(),depth,1);gpu.Submit();
        ObjectControl control;Tuning tuning;tuning.intensity=.8f;tuning.localStructureStrength=.6f;tuning.localToneStrength=.4f;
        for(unsigned pass=0;pass<2;++pass){
            ObjectPassTuning rules;
            for(unsigned group=0;group<4;++group)rules.groups[group]={group!=2,float(group+pass)/5,float(4-group)/5};
            const UINT w=pass?W/2:W,h=pass?H/2:H;
            Check(control.Record(gpu.device.Get(),gpu.list.Get(),shift%TheosRenderPipeline::SourceDLSSG::kCommandSlots,pass,
                rules,tuning,m.Get(),d.Get(),w,h,peripheral,offset),"per-pass object controls");
            auto bytes=gpu.Read(control.Output(pass),16);std::vector<float> pixels(w*h*4);std::memcpy(pixels.data(),bytes.data(),bytes.size());
            auto coordinate=[&](unsigned p,unsigned size,unsigned guide,float jitter){
                float uv=(p+.5f)/size;
                if(peripheral){float r=(2*uv-1)*.9f,a=std::abs(r),unpacked=a;
                    if(a>.8f){float y=(a-.8f)/.1f;unpacked=.8f+.2f*(.5f*y/(1-.5f*y));}
                    uv=(std::copysign(unpacked,r)+1)*.5f;}
                return unsigned(std::clamp(int(std::floor(uv*guide+jitter)),0,int(guide)-1));
            };
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
                const auto qx=coordinate(x,w,gw,offset[0]),qy=coordinate(y,h,gh,offset[1]);
                const unsigned group=qx%6;std::array<float,4> expected{.8f,.4f,.6f,1};
                if(qy%3==0&&group>=1&&group<=4&&rules.groups[group-1].enabled)
                    expected={.8f,rules.groups[group-1].tone,rules.groups[group-1].structure,1};
                for(unsigned c=0;c<4;++c)Require(pixels[(y*w+x)*4+c]==expected[c],"RGBA control contract, defaults and mapped groups");
            }
        }
        Require(control.Output(0)!=control.Output(1),"passes retain distinct control resources");
    }
    ComPtr<ID3D12InfoQueue> info;if(SUCCEEDED(gpu.device.As(&info))){
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size{};info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
            Require(m->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&m->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"no D3D12 debug errors");}
    }
    std::puts("PASS production D3D12 object masks: all groups, exact endpoints, HDR, alpha, depth rejection, guide scaling and retired-slot reuse");
}
