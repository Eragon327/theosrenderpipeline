#include "FrameGen/XeFGExtension/pacing/FlipMeteringState.h"
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main()
{
    using sdk_pacing::detail::FlipMeteringState;
    FlipMeteringState unsupported;
    unsigned enableCalls{};
    if (unsupported.Probe(-104, 0) && unsupported.CanEnable()) {
        ++enableCalls;
        unsupported.Record(4, 28000000, -104, 0);
    }
    Require(enableCalls == 0 && unsupported.refused && !unsupported.configured && !unsupported.metered,
        "all-refused driver is declined by off probe without acquiring an obligation");
    Require(!unsupported.Probe(0, 0) && !unsupported.CanEnable(), "capability refusal is stable for this owner");
    Require(unsupported.Record(0, 0, -104, 0) && !unsupported.configured,
        "unsupported owner has no configuration to quarantine on retirement");
    FlipMeteringState active;
    Require(active.Probe(0, 0) && active.CanEnable(), "off probe admits a capable driver");
    Require(active.Record(4, 28000000, 0, 0) && active.metered, "initial driver configuration");
    Require(active.Record(4, 30000000, -7004, 0), "failed update alone permits recovery");
    Require(active.configured, "failed update must retain the obligation to disable the driver");
    Require(!active.metered && active.outputs == 4 && active.batchNs == 28000000,
        "failed update claims no metering and retains last acknowledged timing");
    Require(!active.Record(0, 0, -7003, 0) && active.configured,
        "failed off after failed update blocks retirement and retains ownership");
    Require(active.Record(0, 0, 0, 0) && !active.configured && !active.metered,
        "acknowledged off clears the outstanding configuration");

    FlipMeteringState refused;
    Require(refused.Probe(0, 0), "enable-refusing driver accepts the capability probe");
    for (unsigned i = 0; i < FlipMeteringState::FailureLimit; ++i)
        Require(refused.Record(4, 28000000, -7004, 0), "enable failure can be recovered");
    Require(refused.refused && !refused.CanEnable() && refused.configured && !refused.metered,
        "even failed initial enable attempts require explicit off before retirement");
    Require(refused.Record(0, 0, 0, 0) && !refused.configured,
        "software pacing fallback acknowledges driver off");

    FlipMeteringState noIflip;
    Require(noIflip.Probe(0, 0), "capability admission is independent of later flip feedback");
    Require(noIflip.Record(4, 28000000, 0, FlipMeteringState::NoIndependentFlip)
        && noIflip.configured && !noIflip.metered,
        "no independent flip feedback still leaves driver configuration to retire");
    Require(noIflip.Record(4, 24000000, 0, 0) && noIflip.metered && noIflip.spacing == 6000000,
        "later successful update applies its new timing");
    Require(noIflip.Record(0, 0, 0, 0) && !noIflip.configured && noIflip.spacing == 0,
        "normal successful off clears timing");
    std::puts("PASS: metering update failure retains checked driver-off obligation");
}
