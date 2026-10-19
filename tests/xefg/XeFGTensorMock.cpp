#include "FrameGen/XeFGTensorAPI.h"
#include <cstring>
using namespace TheosRenderPipeline::XeFGTensor;
namespace {
unsigned starts{}, retires{}, flags{};
bool Mode(const char* mode){char text[64]{};GetEnvironmentVariableA("TRP_XEFG_MOCK_MODE",text,sizeof(text));return !std::strcmp(text,mode);}
HRESULT __cdecl Start(ID3D12Device*,HMODULE,HMODULE,const wchar_t*,char*,std::uint32_t){
    ++starts;
    if(Mode("safe-start")){flags=0;return E_FAIL;}
    if(Mode("unsafe-start")){flags=Unsafe;return E_FAIL;}
    flags=ModelReady;return S_OK;
}
HRESULT __cdecl Retire(char*,std::uint32_t){++retires;return E_FAIL;} // Deliberately does not set Unsafe.
void __cdecl Snapshot(Stats* s){if(s&&s->size==sizeof(Stats)){s->flags=flags;s->matchedPSOs=starts;s->cachedPSOs=retires;}}
}
extern "C" __declspec(dllexport) HRESULT __cdecl TRPXeFGTensorGetAPI(API* api){
    if(!api||api->size!=sizeof(API)||api->version!=Version)return E_NOINTERFACE;
    api->start=&Start;api->retire=&Retire;api->snapshot=&Snapshot;return S_OK;
}
