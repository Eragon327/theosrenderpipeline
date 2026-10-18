#pragma once
#include <nvsdk_ngx.h>

namespace TheosRenderPipeline::DLSS
{
    // Failed SDK release retains the feature and every dependent resource.
    template<class Release, class Cleanup>
    NVSDK_NGX_Result ReleaseOwnedFeature(NVSDK_NGX_Handle*& handle, Release release, Cleanup cleanup)
    {
        const auto result = handle ? release(handle) : NVSDK_NGX_Result_Success;
        if (!NVSDK_NGX_SUCCEED(result)) { return result; }
        handle = nullptr;
        cleanup();
        return result;
    }
}
