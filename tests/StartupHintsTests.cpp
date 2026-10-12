#include "StartupHints.h"
#include <cstdio>
#include <cstdlib>

using namespace TheosRenderPipeline::StartupHints;

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
static bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

int main()
{
    // Status lines as reported by users and produced by the renderer.
    Require(Classify("another frame-generation mod is already loaded; disable the competing mod failed HRESULT=0x8000FFFF") ==
        Failure::CompetingFrameGeneration, "competing frame generation");
    Require(Classify("sl.interposer.dll already loaded by another owner failed HRESULT=0x8000FFFF") ==
        Failure::StreamlineOwnedElsewhere, "Streamline owned elsewhere");
    Require(Classify("slSetFeatureLoaded feature=1000 Streamline result=34 failed HRESULT=0x80004005") ==
        Failure::FrameGenerationUnavailable, "result 34");
    Require(!Classify("slSetFeatureLoaded feature=3 Streamline result=34 failed HRESULT=0x80004005"), "Reflex load is not the DLSS-G hint");
    Require(Classify("nvngx_dlssg.dll was not loaded from the configured Streamline directory failed HRESULT=0x80004005") ==
        Failure::StreamlineModuleMissing, "module missing");
    Require(Classify("NVIDIA swapchain ownership was not acquired failed (0x80004005); rendering stopped, restart required.") ==
        Failure::SwapchainNotAcquired, "swapchain not acquired");
    Require(Classify("configured DLSS-G module or required export missing; no patches applied") ==
        Failure::CompatibilityRuntimeMissing, "Ada runtime missing");
    Require(Classify("Ampere resolver publication changed; restart required") == Failure::CompatibilityRuntimeChanged, "import rewrite");
    Require(Classify("Ampere publication verification failed") == Failure::CompatibilityRuntimeChanged, "publication verification");
    Require(Classify("NVAPI does not match the physical compatibility adapter") == Failure::CompatibilityAdapterUnidentified, "NVAPI");
    Require(Classify("Ampere provider ABI is unsupported") == Failure::CompatibilityRuntimeUnsupported, "unsupported runtime");
    Require(!Classify("Deferred source DLSS startup failed") && HintFor("Deferred source DLSS startup failed", {}).empty(),
        "unrecognised failures keep the original dialog");

    Facts standard{};
    const auto unsupported = Hint(Failure::FrameGenerationUnavailable, Facts{false, false, true, false, true, {}});
    Require(Has(unsupported, "Likely causes and what to try:\n") && Has(unsupported, "reported DLSS frame generation as unsupported") &&
        Has(unsupported, "Universal edition") && Has(unsupported, "Smooth Motion") && Has(unsupported, "scheduling is off"),
        "result 34 on Standard: runtime reason, Universal, overrides and HAGS off");
    const auto universal = Hint(Failure::FrameGenerationUnavailable, Facts{true, false, false, true, false, {}});
    Require(!Has(universal, "Universal edition") && !Has(universal, "as unsupported") && Has(universal, "NVIDIA reported a frame-generation override") &&
        Has(universal, "Check that hardware-accelerated GPU scheduling is on"), "only observed facts are stated as facts");
    Require(Has(Hint(Failure::StreamlineOwnedElsewhere, Facts{.otherOwner = "D:\\Mods\\ENB AA\\sl.interposer.dll"}), "ENB AA\\sl.interposer.dll") &&
        Has(Hint(Failure::StreamlineOwnedElsewhere, standard), "ENB Anti-Aliasing"), "names the other owner when known");
    Require(Has(Hint(Failure::SwapchainNotAcquired, Facts{.communityShaders = true}), "Community Shaders' frame generation") &&
        Has(Hint(Failure::SwapchainNotAcquired, standard), "ENB frame generation"), "CS route vs other swapchain owners");
    Require(Has(Hint(Failure::CompatibilityRuntimeChanged, standard), "Apocrypha Menu Framework"), "import rewriting mods");
    for (auto failure : {Failure::CompetingFrameGeneration, Failure::StreamlineOwnedElsewhere, Failure::FrameGenerationUnavailable,
             Failure::StreamlineModuleMissing, Failure::SwapchainNotAcquired, Failure::CompatibilityRuntimeMissing,
             Failure::CompatibilityRuntimeChanged, Failure::CompatibilityRuntimeUnsupported, Failure::CompatibilityAdapterUnidentified}) {
        const auto hint = Hint(failure, Facts{false, false, true, true, true, "C:\\x\\sl.common.dll"});
        Require(!hint.empty() && hint.size() < 700, "every failure has a compact hint");
    }
    std::puts("PASS startup failure classification and hints");
}
