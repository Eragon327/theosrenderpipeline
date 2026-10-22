#include "SourceDLSSGMFG.h"
#include "Localization.h"
#include <spdlog/spdlog.h>

namespace TheosRenderPipeline::SourceDLSSG
{
    void MFGUnlock::SelectAdapter(ID3D12Device*)
    {
        state_.route = MFGRoute::Native;
        spdlog::info("[SourceDLSSG MFG] selected route=Native edition=Standard compatibilityAvailable=false; "
            "RTX 30-series requires the Universal renderer with SourceDLSSGMFGUnlock=true, even with interpolation off");
    }
    void MFGUnlock::BeforeStreamline(ID3D12Device*, const std::filesystem::path&) {}
    void MFGUnlock::EnterStartupScope() noexcept {}
    void MFGUnlock::LeaveStartupScope() noexcept {}
    // Both variants read the setting; only the full renderer applies the unlock.
    void MFGUnlock::Configure(bool)
    {
        state_.requested = false;
    }

    void MFGUnlock::Prepare(ID3D12Device*, const std::filesystem::path&)
    {
        if (started_) { return; }
        started_ = true;
        state_.route = MFGRoute::Native;
        state_.status = TR("Standard renderer; native NVIDIA capabilities; Ada/Ampere compatibility not included");
        spdlog::info("[SourceDLSSG MFG] startup path: {}; runtime capabilities remain authoritative", state_.status);
    }

    void MFGUnlock::BindWrapper(const void*) {}
    void MFGUnlock::Tick() {}
}
