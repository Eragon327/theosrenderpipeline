#pragma once

namespace TheosRenderPipeline::SourceDLSSG
{
    enum class RetirementPurpose { Resize, UpscalerReplacement };

    // A fixed-extent algorithm change still retires every reader. It retains
    // compatible shared inputs/private passes, then invalidates temporal history.
    // Resize/destruction must release them after exactly the same retirement.
    template<class Retire, class Harvest, class Release, class Reset>
    bool RetireSharedResources(RetirementPurpose purpose, Retire retire, Harvest harvest,
        Release release, Reset reset)
    {
        if (!retire()) { return false; }
        harvest();
        if (purpose == RetirementPurpose::Resize) { release(); }
        reset();
        return true;
    }
}
