#include "XeFGUnlock.h"

// Standard ships Intel's runtimes and uses them as Intel ships them: XeSS and
// official XeFG x2. The x3-x6 unlock, which patches libxess_fg.dll in memory, is
// built into the Universal renderer only, like the NVIDIA MFG unlocks.
namespace TheosRenderPipeline::XeFGUnlock {
bool Available() { return false; }
PatchResult EnsureInstalled(HMODULE, std::string& reason)
{
    reason = "x3-x6 need the Universal renderer";
    return PatchResult::Refused;
}
void NewContextEpoch() {}
Stats Snapshot() { return {}; }
}
