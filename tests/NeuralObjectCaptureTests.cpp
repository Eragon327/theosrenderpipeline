#include "FrameGen/NeuralObjectCapture.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace TheosRenderPipeline;
using namespace TheosRenderPipeline::NeuralRendering;
static void Require(bool value,const char* why) { if(!value){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);} }
static void Check(HRESULT hr,const char* why) { if(FAILED(hr)){std::fprintf(stderr,"FAIL: %s %08lX\n",why,(unsigned long)hr);std::exit(1);} }
static std::vector<float> Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture,unsigned channels)
{
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;Check(device->CreateTexture2D(&d,nullptr,&staging),"staging");context->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE map{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"map");
    std::vector<float> result(d.Width*d.Height*channels);for(UINT y=0;y<d.Height;++y)std::memcpy(result.data()+y*d.Width*channels,(const char*)map.pData+y*map.RowPitch,d.Width*channels*4);
    context->Unmap(staging.Get(),0);return result;
}
int main()
{
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    Check(hr,"WARP");
    constexpr char source[]=R"(
float4 vs(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0.25,1);}
float4 ps():SV_Target {return float4(0.25,0.5,0.75,1);})";
    ComPtr<ID3DBlob> v,p,e;Check(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&v,&e),"VS");
    Check(D3DCompile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&p,&e),"PS");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    Check(device->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs),"create VS");
    Check(device->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps),"create PS");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color,depth;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    Check(device->CreateTexture2D(&desc,nullptr,&color),"color");Check(device->CreateRenderTargetView(color.Get(),nullptr,&rtv),"RTV");
    desc.Format=DXGI_FORMAT_D32_FLOAT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    Check(device->CreateTexture2D(&desc,nullptr,&depth),"depth");Check(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv),"DSV");
    D3D11_VIEWPORT viewport{0,0,16,16,0,1};context->RSSetViewports(1,&viewport);
    auto* target=rtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);
    const auto before=Read(device.Get(),context.Get(),color.Get(),4);
    ObjectCapture capture;
    Require(!capture.Current(9),"no uninitialized mask");
    for(unsigned group=1;group<=4;++group){
        const auto frame=10+group;
        Check(capture.Record(context.Get(),static_cast<ObjectGroup>(group),frame,{16,16},depth.Get(),nullptr,[&]{context->Draw(3,0);}),"record opaque group");
        auto* mask=capture.Current(frame);Require(mask&&capture.Draws()==1,"current frame has one captured draw");
        const auto pixels=Read(device.Get(),context.Get(),mask,2);
        for(unsigned i=0;i<256;++i){Require(pixels[i*2]==float(group)&&pixels[i*2+1]==0.25f,"every pixel contains exact group and depth");}
        Require(!capture.Current(frame+1),"stale capture rejected");
        ComPtr<ID3D11PixelShader> restored;context->PSGetShader(&restored,nullptr,nullptr);Require(restored.Get()==ps.Get(),"pixel shader restored");
        ComPtr<ID3D11RenderTargetView> restoredTarget;ComPtr<ID3D11DepthStencilView> restoredDepth;
        context->OMGetRenderTargets(1,&restoredTarget,&restoredDepth);Require(restoredTarget.Get()==rtv.Get()&&restoredDepth.Get()==dsv.Get(),"targets restored");
        ComPtr<ID3D11DepthStencilState> restoredState;UINT stencil{};context->OMGetDepthStencilState(&restoredState,&stencil);Require(!restoredState,"default depth state restored");
        Require(Read(device.Get(),context.Get(),color.Get(),4)==before,"capture never changes original color");
    }
    unsigned draws{};
    D3D11_TEXTURE2D_DESC uavDesc{};uavDesc.Width=uavDesc.Height=16;uavDesc.MipLevels=uavDesc.ArraySize=uavDesc.SampleDesc.Count=1;
    uavDesc.Format=DXGI_FORMAT_R32_FLOAT;uavDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> sideEffect;ComPtr<ID3D11UnorderedAccessView> uav;
    Check(device->CreateTexture2D(&uavDesc,nullptr,&sideEffect),"UAV texture");Check(device->CreateUnorderedAccessView(sideEffect.Get(),nullptr,&uav),"UAV");
    auto* rawUav=uav.Get();
    // A count of eight restored RTV slots would make a following UAV at slot 1
    // invalid, even when seven of those RTV pointers were null.
    context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,1,1,&rawUav,nullptr);
    Require(capture.Record(context.Get(),ObjectGroup::Player,20,{16,16},depth.Get(),nullptr,[&]{++draws;})==S_FALSE&&!draws,"pixel UAV draw is not replayed");
    ID3D11UnorderedAccessView* nullUav{};
    context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,1,1,&nullUav,nullptr);
    Require(capture.Record(context.Get(),ObjectGroup::World,20,{16,16},depth.Get(),nullptr,[&]{++draws;})==S_FALSE&&!draws,"unclassified objects never replay");
    Require(capture.Record(context.Get(),ObjectGroup::Player,20,{8,8},depth.Get(),nullptr,[&]{++draws;})==S_FALSE&&!draws,"other viewport rejected");
    Require(capture.Record(context.Get(),ObjectGroup::Player,20,{16,16},color.Get(),nullptr,[&]{++draws;})==S_FALSE&&!draws,"unrelated depth rejected");
    D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].BlendEnable=TRUE;blendDesc.RenderTarget[0].SrcBlend=blendDesc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend=blendDesc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;blendDesc.RenderTarget[0].BlendOp=blendDesc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;ComPtr<ID3D11BlendState> blend;Check(device->CreateBlendState(&blendDesc,&blend),"blend");
    context->OMSetBlendState(blend.Get(),nullptr,~0u);
    Require(capture.Record(context.Get(),ObjectGroup::Player,20,{16,16},depth.Get(),nullptr,[&]{++draws;})==S_FALSE&&!draws,"transparent draw rejected");
    context->ClearState();capture.Reset();Require(!capture.Current(14),"reset clears classification");
    ComPtr<ID3D11InfoQueue> info;if(SUCCEEDED(device.As(&info))){for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T length{};info->GetMessage(i,nullptr,&length);std::vector<char> bytes(length);auto* m=reinterpret_cast<D3D11_MESSAGE*>(bytes.data());info->GetMessage(i,m,&length);if(m->Severity==D3D11_MESSAGE_SEVERITY_ERROR||m->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION){std::fprintf(stderr,"%s\n",m->pDescription);Require(false,"D3D11 debug error");}}}
    std::puts("PASS production opaque-object capture: all groups, depth, frame freshness, state restoration and exclusion paths");
}
