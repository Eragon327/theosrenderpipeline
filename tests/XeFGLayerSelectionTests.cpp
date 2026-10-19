#include "FrameGen/XeFGLayerSelection.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline::SourceDLSSG;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

int main()
{
    unsigned cases = 0;
    auto expect = [&](XeFGLayers got, XeFGLayer hudless, XeFGLayer ui, const char* why) {
        Require(got.hudless == hudless && got.ui == ui, why); ++cases;
    };
    constexpr auto sdr = DXGI_FORMAT_R8G8B8A8_UNORM, retained = DXGI_FORMAT_R10G10B10A2_UNORM;
    // The same retained format applies with HDR on, Windows HDR off, and a
    // live HDR-disable. None may select incompatible 8-bit world layers.
    expect(SelectXeFGLayers(retained, sdr, true, true, true), XeFGLayer::Converted, XeFGLayer::Converted, "fresh retained 10-bit layers");
    expect(SelectXeFGLayers(retained, sdr, true, false, true), XeFGLayer::None, XeFGLayer::None, "stale converted world never offered");
    expect(SelectXeFGLayers(retained, sdr, true, false, false), XeFGLayer::None, XeFGLayer::None, "stale converted world without UI refused");
    expect(SelectXeFGLayers(retained, sdr, false, true, true), XeFGLayer::None, XeFGLayer::None, "missing conversion refused");
    expect(SelectXeFGLayers(sdr, sdr, true, false, true), XeFGLayer::Standard, XeFGLayer::Standard, "matching SDR ignores stale conversion");
    expect(SelectXeFGLayers(sdr, sdr, false, false, false), XeFGLayer::Standard, XeFGLayer::None, "matching SDR without UI");
    expect(SelectXeFGLayers(retained, retained, false, false, true), XeFGLayer::Standard, XeFGLayer::Standard, "matching native 10-bit producer");
    std::printf("PASS: %u XeFG layer selections: retained 10-bit SDR/HDR, stale conversion refused, matching native formats\n", cases);
}
