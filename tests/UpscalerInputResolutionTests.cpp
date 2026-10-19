#include "FrameGen/UpscalerInputResolution.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr,"FAIL: %s\n",reason); std::exit(1); }
}
int main()
{
    using namespace TheosRenderPipeline;
    const UpscalerInputResolution range{{1280,720},{960,540},{1920,1080}};
    Require(range.Accepts({1280,720},{1920,1080}),"SDK optimal extent");
    Require(range.Accepts({960,540},{1920,1080}) && range.Accepts({1920,1080},{1920,1080}),"inclusive supported boundaries");
    Require(range.Accepts({1707,960},{2560,1440}),"rounded fixed input can differ from SDK preferred input");
    Require(!range.Accepts({959,540},{1920,1080}) && !range.Accepts({1921,1080},{2560,1440}),"SDK range is authoritative");
    Require(!range.Accepts({1280,800},{1920,1080}),"in-range dimensions with wrong aspect are rejected");
    Require(!range.Accepts({0,720},{1920,1080}) && !range.Accepts({1280,0},{1920,1080}),"partial fixed extent is rejected");
    Require(!range.Accepts({1280,720},{0,1080}) && !range.Accepts({1280,720},{1920,0}),"invalid output is rejected");
    Require(!range.Accepts({1280,720},{640,360}),"input cannot exceed output");
    const UpscalerInputResolution empty{};
    // Pinned libxess on RTX 4080 SUPER: Ultra Performance at 2560x1440.
    const UpscalerInputResolution roundedMinimum{{854,480},{854,480},{2560,1440}};
    Require(!roundedMinimum.Accepts({853,480},{2560,1440}) && roundedMinimum.Accepts({854,480},{2560,1440}),
        "a one-pixel SDK minimum discrepancy requires refusal");
    Require(!empty.Valid() && !empty.Accepts({1280,720},{1920,1080}),"unknown SDK range cannot authorize allocation");
    Require(!UpscalerInputResolution{{1280,720},{1400,540},{1920,1080}}.Valid(),"preferred extent below minimum is invalid");
    Require(!UpscalerInputResolution{{1280,720},{960,540},{1100,1080}}.Valid(),"preferred extent above maximum is invalid");
    const auto limit=(std::numeric_limits<std::uint32_t>::max)();
    const UpscalerInputResolution huge{{limit,limit},{1,1},{limit,limit}};
    Require(huge.Accepts({limit,limit},{limit,limit}),"aspect arithmetic near UINT_MAX does not overflow");
    Require(!huge.Accepts({limit,1},{limit,limit}),"large mismatch is not mistaken for rounding");
    std::puts("PASS SDK input bounds, fixed extents, aspect rounding and overflow");
}
