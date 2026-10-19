#pragma once
// Private reconstruction prototype. All hooks and payloads are process scoped.
// StopAfterGpuIdle must be called after retiring every attached SDK context.
#include "KernelMetadata.h"
#include <nvapi.h>
#include <MinHook.h>
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

struct TensorEngine {
    using List=ID3D12GraphicsCommandList;
    using Pso=ID3D12PipelineState;
    struct State {
        int program=-1;
        ID3D12DescriptorHeap* heap{};
        UINT64 table[2]{};
        unsigned bindings{}, valid{};
        std::array<unsigned,12> constants{};
    };
    struct Arguments {
        NvU64 input{}, residual{}, weights{}, bias{}, activation{}, output{};
        unsigned width{},height{};
        std::array<unsigned,12> constants{};
    };
    static_assert(sizeof(Arguments)==104);
    static inline TensorEngine* active{};
    ComPtr<ID3D12Device> device;
    std::array<NVDX_ObjectHandle,16> kernels{};
    std::vector<void*> hooks;
    std::map<List*,State> states;
    std::map<Pso*,unsigned> programs;
    std::vector<ComPtr<Pso>> heldPsos;
    std::recursive_mutex mutex;
    unsigned increment{};
    bool forceFallback{};
    std::atomic_bool enabled{};
    bool hooksReady{}, unsafe{};
    uintptr_t sdkBegin{},sdkEnd{};
    std::atomic_uint64_t launches{}, fallback{}, matched{};
    std::atomic_uint64_t cached{};
    std::array<std::atomic_uint64_t,17> perProgram{};
    std::array<std::atomic_uint64_t,7> refusals{};
    decltype(&NvAPI_D3D12_CreateCubinComputeShaderWithName) create{};
    decltype(&NvAPI_D3D12_DestroyCubinComputeShader) destroy{};
    decltype(&NvAPI_D3D12_LaunchCubinShader) launch{};
    decltype(&NvAPI_D3D12_GetCudaIndependentDescriptorObject) descriptor{};
    static inline HRESULT(WINAPI* compute)(ID3D12Device*,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,REFIID,void**){};
    static inline HRESULT(WINAPI* stream)(ID3D12Device2*,const D3D12_PIPELINE_STATE_STREAM_DESC*,REFIID,void**){};
    static inline HRESULT(WINAPI* loadCompute)(ID3D12PipelineLibrary*,LPCWSTR,const D3D12_COMPUTE_PIPELINE_STATE_DESC*,REFIID,void**){};
    static inline HRESULT(WINAPI* loadStream)(ID3D12PipelineLibrary1*,LPCWSTR,const D3D12_PIPELINE_STATE_STREAM_DESC*,REFIID,void**){};
    static inline HRESULT(WINAPI* reset)(List*,ID3D12CommandAllocator*,Pso*){};
    static inline void(WINAPI* dispatch)(List*,UINT,UINT,UINT){};
    static inline void(WINAPI* setPso)(List*,Pso*){};
    static inline void(WINAPI* heaps)(List*,UINT,ID3D12DescriptorHeap*const*){};
    static inline void(WINAPI* signature)(List*,ID3D12RootSignature*){};
    static inline void(WINAPI* table)(List*,UINT,D3D12_GPU_DESCRIPTOR_HANDLE){};
    static inline void(WINAPI* constant)(List*,UINT,UINT,UINT){};
    static inline void(WINAPI* constants)(List*,UINT,UINT,const void*,UINT){};

    static std::string Sha(const void* data,size_t bytes) {
        BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE h{};
        if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
        std::array<unsigned char,32> hash{};
        auto r=BCryptCreateHash(alg,&h,nullptr,0,nullptr,0,0);
        if(r>=0)r=BCryptHashData(h,(PUCHAR)data,static_cast<ULONG>(bytes),0);
        if(r>=0)r=BCryptFinishHash(h,hash.data(),32,0);
        if(h)BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg,0);
        if(r<0)return {};
        std::string s;const char* hex="0123456789abcdef";
        for(auto v:hash){s+=hex[v>>4];s+=hex[v&15];}return s;
    }
    static bool Track(void* caller) {
        auto s=active;const auto pc=reinterpret_cast<uintptr_t>(caller);
        return s && s->enabled.load(std::memory_order_acquire) && pc>=s->sdkBegin && pc<s->sdkEnd;
    }
    static bool NativeMethod(void* entry) {
        HMODULE module{};wchar_t name[32768]{},system[32768]{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(entry),&module) || !GetModuleFileNameW(module,name,32768) || !GetSystemDirectoryW(system,32768))return false;
        auto path=std::filesystem::path(name);auto filename=path.filename().wstring();
        return !_wcsicmp(path.parent_path().c_str(),system) &&
            (!_wcsicmp(filename.c_str(),L"D3D12Core.dll") || !_wcsicmp(filename.c_str(),L"d3d12.dll"));
    }
    template<class Fn> void Hook(void* address,void* replacement,Fn& original) {
        if(std::find(hooks.begin(),hooks.end(),address)!=hooks.end())return;
        Require(NativeMethod(address),"tensor method has a wrapper or another owner");
        Require(MH_CreateHook(address,replacement,reinterpret_cast<void**>(&original))==MH_OK,"rebuilt hook planned");
        hooks.push_back(address);
    }
    void Install(ID3D12Device* d,HMODULE fg,const std::filesystem::path& assets) {
        std::lock_guard lock(mutex);
        Require(!unsafe,"tensor state retained after uncertain retirement");
        Require(!device || device.Get()==d,"tensor device changed; restart required");
        device=d;active=this;enabled=false;states.clear();programs.clear();heldPsos.clear();
        sdkBegin=reinterpret_cast<uintptr_t>(fg);
        auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(sdkBegin+reinterpret_cast<IMAGE_DOS_HEADER*>(fg)->e_lfanew);
        sdkEnd=sdkBegin+nt->OptionalHeader.SizeOfImage;
        std::ifstream input(assets/"tensor.fatbin",std::ios::binary);
        std::vector<char> blob((std::istreambuf_iterator<char>(input)),{});
        Require(Sha(blob.data(),blob.size())=="2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7","rebuilt tensor payload identity");
        auto nv=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        Require(nv!=nullptr,"rebuilt native NVAPI");
        auto query=reinterpret_cast<void*(__cdecl*)(unsigned)>(GetProcAddress(nv,"nvapi_QueryInterface"));
        auto init=reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
        Require(init && init()==NVAPI_OK,"rebuilt NVAPI initialize");
        create=reinterpret_cast<decltype(create)>(query(0x1dc7261f));
        destroy=reinterpret_cast<decltype(destroy)>(query(0x7fb785ba));
        launch=reinterpret_cast<decltype(launch)>(query(0x5c52bb86));
        descriptor=reinterpret_cast<decltype(descriptor)>(query(0x0ddac234));
        Require(create && destroy && launch && descriptor,"rebuilt experimental NVAPI functions");
        for(const auto& m:kernelMetadata)if(!kernels[m.kernel]) {
#if defined(TRP_XEFG_EXTENSION_TESTING)
            Require(testFault!=2 || m.kernel!=3,"injected failure after creating three CUDA kernels");
#endif
            Require(create(d,blob.data(),static_cast<NvU32>(blob.size()),128,1,1,m.name,&kernels[m.kernel])==NVAPI_OK && kernels[m.kernel],"rebuilt CUDA kernel");
        }
        increment=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto mh=MH_Initialize();Require(mh==MH_OK || mh==MH_ERROR_ALREADY_INITIALIZED,"rebuilt detour engine");
        if(!hooksReady) {
        auto vt=*reinterpret_cast<void***>(d);
        Hook(vt[11],reinterpret_cast<void*>(&Compute),compute);
        ComPtr<ID3D12Device2> d2;Check(d->QueryInterface(IID_PPV_ARGS(&d2)),"rebuilt device2");
        Hook((*reinterpret_cast<void***>(d2.Get()))[47],reinterpret_cast<void*>(&Stream),stream);
        ComPtr<ID3D12PipelineLibrary> library;
        if(SUCCEEDED(d2->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(&library)))) {
            Hook((*reinterpret_cast<void***>(library.Get()))[10],reinterpret_cast<void*>(&LoadCompute),loadCompute);
            ComPtr<ID3D12PipelineLibrary1> library1;
            if(SUCCEEDED(library.As(&library1)))
                Hook((*reinterpret_cast<void***>(library1.Get()))[13],reinterpret_cast<void*>(&LoadStream),loadStream);
        }
        // Native methods are shared by lists on this device. Only hash-matched
        // PSOs are eligible; all other dispatches pass through unchanged.
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<List> list;
        Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)),"rebuilt hook allocator");
        Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list)),"rebuilt hook list");
        Check(list->Close(),"rebuilt hook list closed");
        vt=*reinterpret_cast<void***>(list.Get());
        Hook(vt[10],reinterpret_cast<void*>(&Reset),reset);
        Hook(vt[14],reinterpret_cast<void*>(&Dispatch),dispatch);
        Hook(vt[25],reinterpret_cast<void*>(&SetPso),setPso);
        Hook(vt[28],reinterpret_cast<void*>(&Heaps),heaps);
        Hook(vt[29],reinterpret_cast<void*>(&Signature),signature);
        Hook(vt[31],reinterpret_cast<void*>(&Table),table);
        Hook(vt[33],reinterpret_cast<void*>(&Constant),constant);
        Hook(vt[35],reinterpret_cast<void*>(&Constants),constants);
        for(auto p:hooks){auto r=MH_EnableHook(p);Require(r==MH_OK || r==MH_ERROR_ENABLED,"rebuilt hook published");}
        hooksReady=true;
        }
        enabled.store(true,std::memory_order_release);
        std::printf("rebuilt_tensor initialized kernels=16 payload=%zu\n",blob.size());
    }
    bool Register(Pso* p,D3D12_SHADER_BYTECODE cs) {
        std::lock_guard guard(mutex);if(!enabled.load())return false;
        if(!cs.pShaderBytecode || !cs.BytecodeLength)return false;
        auto hash=Sha(cs.pShaderBytecode,cs.BytecodeLength);
        for(unsigned i=0;i<std::size(kernelMetadata);++i){const auto& m=kernelMetadata[i];
            if(hash==m.sha || (m.alternate && hash==m.alternate)) {
                std::lock_guard lock(mutex);if(programs.emplace(p,i).second){heldPsos.emplace_back(p);++matched;}
                return true;
            }
        }
        return false;
    }
    static HRESULT WINAPI Compute(ID3D12Device* d,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
        auto r=compute(d,desc,iid,out);
        if(SUCCEEDED(r) && out && *out && desc && Track(_ReturnAddress()) && d==active->device.Get())active->Register(static_cast<Pso*>(*out),desc->CS);
        return r;
    }
    static HRESULT WINAPI Stream(ID3D12Device2* d,const D3D12_PIPELINE_STATE_STREAM_DESC* desc,REFIID iid,void** out) {
        auto r=stream(d,desc,iid,out);
        if(FAILED(r) || !desc || !out || !*out || !Track(_ReturnAddress()))return r;
        ComPtr<ID3D12Device> base;if(FAILED(d->QueryInterface(IID_PPV_ARGS(&base))) || base.Get()!=active->device.Get())return r;
        active->RegisterStream(static_cast<Pso*>(*out),desc);return r;
    }
    static HRESULT WINAPI LoadCompute(ID3D12PipelineLibrary* lib,LPCWSTR name,const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,REFIID iid,void** out) {
        auto r=loadCompute(lib,name,desc,iid,out);
        if(SUCCEEDED(r)&&out&&*out&&desc&&Track(_ReturnAddress())) {
            ComPtr<ID3D12Device> d;
            if(SUCCEEDED(lib->GetDevice(IID_PPV_ARGS(&d)))&&d.Get()==active->device.Get()&&active->Register(static_cast<Pso*>(*out),desc->CS))++active->cached;
        }return r;
    }
    static HRESULT WINAPI LoadStream(ID3D12PipelineLibrary1* lib,LPCWSTR name,const D3D12_PIPELINE_STATE_STREAM_DESC* desc,REFIID iid,void** out) {
        auto r=loadStream(lib,name,desc,iid,out);
        if(SUCCEEDED(r)&&out&&*out&&desc&&Track(_ReturnAddress())) {
            ComPtr<ID3D12Device> d;
            if(SUCCEEDED(lib->GetDevice(IID_PPV_ARGS(&d)))&&d.Get()==active->device.Get()&&active->RegisterStream(static_cast<Pso*>(*out),desc))++active->cached;
        }return r;
    }
    bool RegisterStream(Pso* pso,const D3D12_PIPELINE_STATE_STREAM_DESC* desc) {
        if(!desc||!desc->pPipelineStateSubobjectStream)return false;
        auto p=static_cast<const unsigned char*>(desc->pPipelineStateSubobjectStream);
        size_t offset=0;D3D12_SHADER_BYTECODE cs{};
        while(offset+8<=desc->SizeInBytes) {
            auto type=*reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p+offset);
            size_t size=0;
            switch(type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:size=16;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:size=24;break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:size=8;break;
            default:return false;
            }
            if(size>desc->SizeInBytes-offset)return false;
            if(type==D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS)cs=*reinterpret_cast<const D3D12_SHADER_BYTECODE*>(p+offset+8);
            offset+=size;
        }
        return offset==desc->SizeInBytes && Register(pso,cs);
    }
    int Program(Pso* p){auto it=programs.find(p);return it==programs.end()?-1:static_cast<int>(it->second);}
    static HRESULT WINAPI Reset(List* l,ID3D12CommandAllocator* a,Pso* p) {
        auto r=reset(l,a,p);if(!Track(_ReturnAddress()))return r;std::lock_guard lock(active->mutex);
        auto& s=active->states[l];s={};if(SUCCEEDED(r))s.program=active->Program(p);return r;
    }
    static void WINAPI SetPso(List* l,Pso* p) {
        if(Track(_ReturnAddress())){std::lock_guard lock(active->mutex);active->states[l].program=active->Program(p);}setPso(l,p);
    }
    static void WINAPI Heaps(List* l,UINT count,ID3D12DescriptorHeap*const* h) {
        if(Track(_ReturnAddress())){std::lock_guard lock(active->mutex);auto& s=active->states[l];auto previous=s.heap;s.heap=nullptr;
        for(UINT i=0;i<count;++i)if(h[i] && h[i]->GetDesc().Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)s.heap=h[i];if(previous!=s.heap)s.bindings=0;}
        heaps(l,count,h);
    }
    static void WINAPI Signature(List* l,ID3D12RootSignature* root) {
        if(Track(_ReturnAddress())){std::lock_guard lock(active->mutex);auto& s=active->states[l];s.bindings=s.valid=0;s.constants={};}signature(l,root);
    }
    static void WINAPI Table(List* l,UINT root,D3D12_GPU_DESCRIPTOR_HANDLE h) {
        if(Track(_ReturnAddress()) && root<2){std::lock_guard lock(active->mutex);auto& s=active->states[l];s.table[root]=h.ptr;s.bindings|=1u<<root;}table(l,root,h);
    }
    static void WINAPI Constant(List* l,UINT root,UINT value,UINT offset) {
        if(Track(_ReturnAddress()) && root==2 && offset<12){std::lock_guard lock(active->mutex);auto& s=active->states[l];s.constants[offset]=value;s.valid|=1u<<offset;}constant(l,root,value,offset);
    }
    static void WINAPI Constants(List* l,UINT root,UINT count,const void* values,UINT offset) {
        if(Track(_ReturnAddress()) && root==2 && values && count && offset<12 && count<=12-offset){std::lock_guard lock(active->mutex);auto& s=active->states[l];
            std::memcpy(s.constants.data()+offset,values,count*4);s.valid|=((1u<<count)-1)<<offset;}constants(l,root,count,values,offset);
    }
    bool Launch(List* l,const State& s,UINT x,UINT y,UINT z) {
        const auto& m=kernelMetadata[s.program];
        auto refuse=[&](unsigned why){++fallback;++refusals[why];return false;};
        if(forceFallback)return refuse(6);
        if(!x || !y || x>65535 || y>65535 || z!=m.dispatchZ)return refuse(0);
        if((s.bindings&3)!=3 || !s.heap)return refuse(1);
        if((s.valid&m.constantMask)!=m.constantMask)return refuse(2);
        const auto hd=s.heap->GetDesc();
        if(hd.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(hd.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))return refuse(3);
        auto gpu=s.heap->GetGPUDescriptorHandleForHeapStart().ptr;
        auto cpu=s.heap->GetCPUDescriptorHandleForHeapStart().ptr;
        auto object=[&](UINT64 address,unsigned add,bool surface,NvU64& handle){
            if(address<gpu || (address-gpu)%increment)return false;
            UINT64 index=(address-gpu)/increment+add;if(index>=hd.NumDescriptors)return false;
            NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS p{};p.structSizeIn=sizeof(p);p.pDevice=device.Get();
            p.type=static_cast<NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_TYPE>(surface?0:1);p.desc.ptr=cpu+index*increment;
            auto r=descriptor(&p);handle=p.handle;return r==NVAPI_OK && handle!=0;
        };
        Arguments a{};
        if(!object(s.table[0],0,false,a.input) || (m.mode==1 && !object(s.table[0],1,false,a.residual)) ||
            !object(s.table[0],m.mode==2?1:2,true,a.output) || !object(s.table[1],0,false,a.weights) ||
            !object(s.table[1],1,false,a.bias) || !object(s.table[1],2,false,a.activation))return refuse(4);
        a.width=x*m.scaleX;a.height=y*m.scaleY;a.constants=s.constants;
        if(launch(l,kernels[m.kernel],(a.width+m.tileX-1)/m.tileX,(a.height+m.tileY-1)/m.tileY,m.gridZ,&a,sizeof(a))!=NVAPI_OK)return refuse(5);
        ++launches;++perProgram[s.program];return true;
    }
    static void WINAPI Dispatch(List* l,UINT x,UINT y,UINT z) {
        if(Track(_ReturnAddress())){std::lock_guard lock(active->mutex);auto it=active->states.find(l);
        if(active->enabled.load() && it!=active->states.end() && it->second.program>=0 && active->Launch(l,it->second,x,y,z))return;}
        dispatch(l,x,y,z);
    }
    bool StopAfterGpuIdle() {
        std::lock_guard lock(mutex);enabled.store(false,std::memory_order_release);
        // Native entry hooks and this owner stay process resident and forward.
        // Never detach a hook beneath another component's chain or callback.
        for(auto& h:kernels)if(h){
            if(destroy && destroy(device.Get(),h)==NVAPI_OK)h=nullptr;
            else unsafe=true;
        }
        if(unsafe)return false;
        heldPsos.clear();states.clear();programs.clear();return true;
    }
};
