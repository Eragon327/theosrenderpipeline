#pragma once
#include <dxgiformat.h>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Select by resource format, independently of the display's HDR signal.
    // Converted targets are valid only after composition of this frame.
    enum class XeFGLayer { None, Standard, Converted };
    struct XeFGLayers
    {
        XeFGLayer hudless{ XeFGLayer::None };
        XeFGLayer ui{ XeFGLayer::None };
    };

    constexpr XeFGLayers SelectXeFGLayers(DXGI_FORMAT outputFormat, DXGI_FORMAT sourceFormat,
        bool conversionReady, bool composedThisFrame, bool uiSource)
    {
        if (outputFormat != sourceFormat) {
            // This includes a retained 10-bit swapchain while Windows HDR is off.
            return conversionReady && composedThisFrame ?
                XeFGLayers{ XeFGLayer::Converted, XeFGLayer::Converted } : XeFGLayers{};
        }
        return { XeFGLayer::Standard, uiSource ? XeFGLayer::Standard : XeFGLayer::None };
    }
}
