#pragma once

#include <d3d11.h>

namespace TheosRenderPipeline::ScreenshotDiagnostics
{
    // Opt-in ([Debug] ScreenshotDiagnostics=true). Records how Skyrim fills its
    // screenshot target and when, relative to TRP's Present, so saved photos can
    // later be given TRP's finished frame. Logging only: no image changes.
    // Call at Present on the game's render thread, outside any observer.
    void OnPresent(ID3D11DeviceContext* context, bool enabled);
}
