#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "FrameGen/XeFGTensorAPI.h"
using Microsoft::WRL::ComPtr;
static void Require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
static void Check(HRESULT hr,const char* reason){Require(SUCCEEDED(hr),reason);}
static std::string escape(const std::string& text){std::string out;for(auto c:text){if(c=='"'||c=='\\')out+='\\';out+=c;}return out;}
static void logEvent(const std::string& name,const std::string& fields={}){
    const auto text="[TRP XeFG tensor] "+name+" "+fields+"\n";OutputDebugStringA(text.c_str());
}
#if defined(TRP_XEFG_EXTENSION_TESTING)
static unsigned testFault{};
#endif
#include "TensorEngine.h"
#include "portable/runtime_layout.h"
#include "portable/port/model_port.h"
#include "portable/port/shader_port.h"
#include "portable/host_unlock.h"
#include "PacingBridge.h"

namespace {
using namespace TheosRenderPipeline::XeFGTensor;
HMODULE self{};
struct Owner {
    std::mutex mutex;
    TensorEngine tensor;
    std::unique_ptr<ShaderPort> shaders;
    std::unique_ptr<ModelPort> model;
    std::unique_ptr<xess_host_unlock::Unlock> unlock;
    ComPtr<ID3D12Device> device;
    HMODULE fg{},ll{};
    std::uint32_t flags{};
    bool running{};
    std::string status;
    bool Uncertain()const {return (flags&Unsafe) || TRPPacingIsUnsafe() || tensor.unsafe || (model&&model->unsafe) ||
        (shaders&&shaders->unsafe) || (unlock&&unlock->unsafe);}
    bool Restore() {
        if(TRPPacingHasOwner()){status="XeFG pacing context must retire before helper cleanup";flags|=Unsafe;return false;}
        if(Uncertain()){flags|=Unsafe;return false;}
        if(!tensor.StopAfterGpuIdle()){flags|=Unsafe;return false;}
        try {
            if(unlock){unlock->Restore();unlock.reset();}
            if(model){model->restore();model.reset();}
            if(shaders){shaders->Restore();shaders.reset();}
            running=false;flags=0;return true;
        } catch(const std::exception& e){status=std::string("XeFG backend retirement refused; resources retained: ")+e.what();flags|=Unsafe;return false;}
    }
};
// Hooks may be in another component's call chain. This owner, callback code and
// native trampolines remain pinned; idle hooks forward without tracking work.
Owner& State(){static auto* value=new Owner;return *value;}
void Reason(char* out,std::uint32_t bytes,const std::string& text){if(out&&bytes){const auto n=(std::min)(text.size(),size_t(bytes-1));std::memcpy(out,text.data(),n);out[n]=0;}}
bool ModuleHash(HMODULE module,const char* expected){
    wchar_t path[32768]{};if(!module||!GetModuleFileNameW(module,path,32768))return false;
    std::ifstream file(std::filesystem::path(path),std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    return TensorEngine::Sha(bytes.data(),bytes.size())==expected;
}
HRESULT __cdecl Start(ID3D12Device* device,HMODULE fg,HMODULE ll,const wchar_t* directory,char* reason,std::uint32_t bytes) {
    auto& s=State();std::lock_guard lock(s.mutex);
    auto result=[&](HRESULT hr){Reason(reason,bytes,s.status);return hr;};
    if(s.Uncertain()){s.status="Unsafe XeFG backend state retained; restart required";return result(E_UNEXPECTED);}
    if(s.running){if(s.device.Get()!=device || s.fg!=fg || s.ll!=ll){s.status="Another device/runtime owns the XeFG tensor backend";return result(E_UNEXPECTED);}return result((s.flags&TensorReady)?S_OK:S_FALSE);}
    try {
        Require(device&&fg&&ll&&directory&&std::filesystem::path(directory).is_absolute(),"XeFG backend arguments");
        Require(ModuleHash(fg,"752b1491350a29fb2850fa4d0d14f62d79980e0b3cd067d147bb2469c1ec3182"),"XeFG 1.3.4 identity mismatch");
        Require(ModuleHash(ll,"d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7"),"XeLL identity mismatch");
        for(auto module:{self,fg,ll}){HMODULE pinned{};Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module),&pinned)&&pinned==module,"XeFG backend module pin");}
        Require(!s.device || s.device.Get()==device,"XeFG tensor device changed; restart required");
        s.device=device;s.fg=fg;s.ll=ll;
        // Cache the old model witnesses before pacing publishes its forwarding hooks.
        (void)xess_layout::Get(fg);
        TRPPreparePacing(device,fg);
#if defined(TRP_XEFG_EXTENSION_TESTING)
        wchar_t fault[16]{};GetEnvironmentVariableW(L"TRP_XEFG_TEST_FAULT",fault,16);
        testFault=static_cast<unsigned>(std::wcstoul(fault,nullptr,10));
#endif
        s.shaders=std::make_unique<ShaderPort>();s.shaders->Initialize(fg,self);
#if defined(TRP_XEFG_EXTENSION_TESTING)
        Require(testFault!=3,"injected failure after shader publication");
#endif
        s.model=std::make_unique<ModelPort>();s.model->Initialize(fg,true,true);
#if defined(TRP_XEFG_EXTENSION_TESTING)
        Require(testFault!=4,"injected failure after model publication");
#endif
        s.unlock=std::make_unique<xess_host_unlock::Unlock>();s.unlock->Initialize(fg,ll,5,false);
        s.flags=ModelReady;s.running=true;
        try {
#if defined(TRP_XEFG_EXTENSION_TESTING)
            Require(testFault!=1,"injected tensor admission refusal");
#endif
            s.tensor.Install(device,fg,std::filesystem::path(directory));
            s.flags|=TensorReady;s.status="Ported XeFG model on NVIDIA tensor cores";return result(S_OK);
        }catch(const std::exception& e){
            const auto detail=std::string(e.what());
            if(!s.tensor.StopAfterGpuIdle()){s.flags|=Unsafe;s.status="Tensor initialization rollback failed; resources retained";return result(E_UNEXPECTED);}
            s.status="Ported XeFG model on portable HLSL; tensor fallback: "+detail;return result(S_FALSE);
        }
    }catch(const std::exception& e){
        const auto detail=std::string(e.what());
        if(!s.Restore()){if(s.status.empty())s.status="XeFG model rollback uncertain; resources retained";return result(E_UNEXPECTED);}
        s.status="XeFG model safely refused; official 1.3.4 x2 retained: "+detail;return result(E_FAIL);
    }
}
HRESULT __cdecl Retire(char* reason,std::uint32_t bytes){
    auto& s=State();std::lock_guard lock(s.mutex);
    const auto ok=s.Restore();if(ok)s.status="XeFG tensor context retired; native hooks forwarding";
    Reason(reason,bytes,s.status);return ok?S_OK:E_UNEXPECTED;
}
void __cdecl Snapshot(Stats* stats){if(!stats||stats->size!=sizeof(Stats))return;auto& s=State();std::lock_guard lock(s.mutex);stats->flags=s.flags;stats->launches=s.tensor.launches.load();stats->fallbacks=s.tensor.fallback.load();stats->matchedPSOs=s.tensor.matched.load();stats->cachedPSOs=s.tensor.cached.load();}
}
extern "C" __declspec(dllexport) HRESULT __cdecl TRPXeFGTensorGetAPI(TheosRenderPipeline::XeFGTensor::API* api){
    using namespace TheosRenderPipeline::XeFGTensor;
    if(!api||api->size!=sizeof(API)||api->version!=Version)return E_NOINTERFACE;
    api->start=&Start;api->retire=&Retire;api->snapshot=&Snapshot;return S_OK;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void*){if(reason==DLL_PROCESS_ATTACH)self=module;return TRUE;}
