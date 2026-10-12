#include "FrameGen/XeFGUnlock.h"
#include <cstdio>
#include <cstdlib>
#include <string>

static void Require(bool value, const char* reason)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}

// Standard ships Intel's runtimes unmodified; only Universal links the x3-x6 unlock.
int main(int argc, char** argv)
{
    using namespace TheosRenderPipeline::XeFGUnlock;
    Require(argc == 2, "edition argument");
    const bool universal = std::string(argv[1]) == "universal";
    Require(Available() == universal, "the unlock is available in Universal only");
    std::string reason;
    Require(EnsureInstalled(nullptr, reason) == PatchResult::Refused, "no runtime is ever patched without admission");
    Require(universal ? reason == "null runtime" : reason == "x3-x6 need the Universal renderer",
        "Standard refuses with the edition reason before any admission check");
    NewContextEpoch();
    Require(Snapshot().presents == 0, "no pacing activity without a published unlock");
    std::printf("PASS XeFG unlock edition=%s available=%d\n", universal ? "Universal" : "Standard", Available());
}
