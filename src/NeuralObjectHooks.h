#pragma once
#include "FrameGen/NeuralObjectCapture.h"
#include <atomic>

namespace TheosRenderPipeline::NeuralObjects
{
    void Install();
    void Configure(bool enabled);
    bool Active();
    void ResetAfterRetirement();
    void RecordDraw(ID3D11DeviceContext* context, const std::function<void()>& draw);
    ID3D11Texture2D* Current();
    const char* Status();
    // Whether the capture hook is installed (Skyrim 1.6.1170, no conflicting patch).
    bool Available();
}
