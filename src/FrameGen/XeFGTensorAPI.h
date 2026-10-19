#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>

// Optional, private XeFG extension ABI. No SDK handles or C++ objects cross it.
// The caller owns every XeFG context and calls Retire only after their successful
// destruction and its own queue retirement. Modules stay process resident.
namespace TheosRenderPipeline::XeFGTensor
{
    inline constexpr std::uint32_t Version = 1;
    enum Flags : std::uint32_t { ModelReady = 1, TensorReady = 2, Unsafe = 4 };
    struct Stats {
        std::uint32_t size{sizeof(Stats)}, flags{};
        std::uint64_t launches{}, fallbacks{};
        std::uint64_t matchedPSOs{}, cachedPSOs{};
    };
    struct API {
        std::uint32_t size{sizeof(API)}, version{Version};
        HRESULT (__cdecl* start)(ID3D12Device*, HMODULE fg, HMODULE ll, const wchar_t* directory,
            char* reason, std::uint32_t reasonBytes){};
        HRESULT (__cdecl* retire)(char* reason, std::uint32_t reasonBytes){};
        void (__cdecl* snapshot)(Stats*){};
    };
    using GetAPI = HRESULT (__cdecl*)(API*);
}
