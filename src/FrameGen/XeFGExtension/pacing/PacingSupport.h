#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include "runtime_layout.h"
#include "FrameGen/XeFGPacingAPI.h"
namespace trp_pacing_bridge {
inline constexpr unsigned kGeneratedCapacity=5;
HMODULE Self() noexcept;
void WriteLog(const std::string&, const std::string& = {});
}
// The SDK pointers are accepted only after their vtables prove native System32
// ownership. This candidate deliberately does not import OptiScaler wrapper code.
namespace single_overlay::native {
inline bool PinInterface(IUnknown* object, size_t slot) noexcept {
    uintptr_t table{},function{};SIZE_T got{};
    if(!object || !ReadProcessMemory(GetCurrentProcess(),object,&table,sizeof(table),&got) || got!=sizeof(table) || !table) return false;
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(table+slot*sizeof(void*)),&function,sizeof(function),&got) || got!=sizeof(function) || !function) return false;
    HMODULE module{};wchar_t path[32768]{},system[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(function),&module) ||
       !GetModuleFileNameW(module,path,32768)) return false;
    const auto n=GetSystemDirectoryW(system,32768);
    return n && n<32768 && _wcsnicmp(path,system,n)==0 && path[n]==L'\\';
}
template<class T> bool Unwrap(T* input, Microsoft::WRL::ComPtr<T>& output) noexcept {
    return PinInterface(input,2) && SUCCEEDED(input->QueryInterface(IID_PPV_ARGS(output.ReleaseAndGetAddressOf())));
}
}
namespace single_overlay::dx12 {
inline bool DeviceIdentity(ID3D12Device* device, Microsoft::WRL::ComPtr<IUnknown>& output) noexcept {
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(output.ReleaseAndGetAddressOf())));
}
}
