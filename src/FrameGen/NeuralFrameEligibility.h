#pragma once

namespace TheosRenderPipeline::SourceDLSSG
{
    inline bool NeuralFrameEligible(bool dedicatedUI, bool worldOnly, bool uiKnown,
        bool blockedMenu, bool hudless, bool ui)
    {
        return dedicatedUI && uiKnown && !blockedMenu && (worldOnly || (hudless && ui));
    }

    inline bool NeuralPreparedEligible(bool worldOnly, bool evaluatedEarly, bool hudless, bool ui)
    {
        return worldOnly ? evaluatedEarly : (hudless && ui);
    }
}
