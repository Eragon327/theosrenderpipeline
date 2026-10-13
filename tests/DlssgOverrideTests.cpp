#include "FrameGen/DlssgOverride.h"
#include "FrameGen/DlssgLiteBlending.h"
#include <string>
#include <cstdio>
#include <cstdlib>

using TheosRenderPipeline::SourceDLSSG::IsNvidiaModelStorePath;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

int main()
{
    Require(IsNvidiaModelStorePath(LR"(C:\ProgramData\NVIDIA\NGX\models\dlssg\versions\20318480\files\160_E658700.bin)"),
        "NVIDIA's downloaded DLSS-G is an override");
    Require(IsNvidiaModelStorePath(L"c:/programdata/nvidia/ngx/models/dlssg/versions/1/files/x.bin"), "case and separators do not matter");
    Require(!IsNvidiaModelStorePath(LR"(D:\Lorerim\mods\Theos Render Pipeline Standard\SKSE\Plugins\TheosRenderPipeline\NVIDIA\Streamline\nvngx_dlssg.dll)"),
        "TRP's own Streamline folder is not an override");
    Require(!IsNvidiaModelStorePath(LR"(C:\Program Files\NVIDIA Corporation\NVIDIA app\models\x.dll)"), "other NVIDIA folders are not the model store");
    Require(!IsNvidiaModelStorePath(L""), "empty path");
    using namespace TheosRenderPipeline::SourceDLSSG::LiteBlending;
    // NVSDK_NGX_D3D12_CreateFeature in DLSS-G 310.9.1 and 310.9.16; the next instruction is RIP-relative.
    const std::uint8_t createFeature[]{ 0x40, 0x53, 0x55, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xA8, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x05 };
    Require(MatchesCreateFeaturePrologue(createFeature), "shipped DLSS-G prologue accepted");
    Require(!MatchesCreateFeaturePrologue(std::span(createFeature, 12)), "a short read is refused");
    auto changed = std::to_array(createFeature); changed[2] = 0x57;
    Require(!MatchesCreateFeaturePrologue(changed), "a different prologue is refused");
    const std::string nul(1, '\0');
    const std::string image = "xx" + nul + "DLSSG.UserInterfaceRecompositionEnabled" + nul + std::string(kParameter) + nul + "yy";
    Require(NamesParameter({ reinterpret_cast<const std::uint8_t*>(image.data()), image.size() }), "a build naming the parameter supports it");
    const std::string old = "DLSSG.UserInterfaceRecompositionEnabled";
    Require(!NamesParameter({ reinterpret_cast<const std::uint8_t*>(old.data()), old.size() }), "older builds do not");
    std::puts("PASS DLSS-G Lite blending: prologue and support detection");
    std::puts("PASS DLSS-G override path: NVIDIA model store recognised, TRP and other folders not");
}
