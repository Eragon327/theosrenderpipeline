#include "FrameGen/DlssgOverride.h"
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
    std::puts("PASS DLSS-G override path: NVIDIA model store recognised, TRP and other folders not");
}
