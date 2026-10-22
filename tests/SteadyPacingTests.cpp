#include "FrameGen/SteadyPacing.h"
#include <cstdio>
#include <cstdlib>

using TheosRenderPipeline::SteadyPacing;
using TheosRenderPipeline::SteadyOutputLimit;

static void Require(bool ok, const char* why)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

// A frame that would take `cost` us, held to the controller's current cap.
static double Paced(const SteadyPacing& pacing, double cost)
{
    return pacing.TargetUs() > 0 ? (std::max)(cost, pacing.TargetUs()) : cost;
}

int main()
{
    // Alternating 10 ms and 20 ms frames (a model every other frame) settle on
    // a cap just above 20 ms, and stay there instead of drifting.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 119; ++frame) { pacing.Observe(frame % 2 ? 20000 : 10000); }
        Require(pacing.TargetUs() == 0, "nothing is capped before the first full window");
        for (int frame = 0; frame < 60; ++frame) { pacing.Observe(frame % 2 ? 20000 : 10000); }
        Require(pacing.TargetUs() >= 20000 && pacing.TargetUs() <= 21000, "the first cap sits just above the slow frames");
        double low = 1e9, high = 0;
        for (int frame = 0; frame < 6000; ++frame) {
            pacing.Observe(Paced(pacing, frame % 2 ? 20000 : 10000));
            if (frame > 600) { low = (std::min)(low, pacing.TargetUs()); high = (std::max)(high, pacing.TargetUs()); }
        }
        Require(low >= 19300 && high <= 21300, "the cap holds within a few percent of the slow frames' cost");
    }
    // Heavier scenes raise the cap within a couple of seconds.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 600; ++frame) { pacing.Observe(Paced(pacing, 16000)); }
        const double before = pacing.TargetUs();
        for (int frame = 0; frame < 900; ++frame) { pacing.Observe(Paced(pacing, 22000)); }
        Require(before < 17000 && pacing.TargetUs() * SteadyPacing::kSlowTolerance >= 22000, "a heavier scene backs the cap off");
    }
    // 2026-10-22 game test: while the limiter holds frames to the cap, single
    // intervals land up to 15% either side of it but average to it. The old
    // miss count read that wobble as overload and walked a 28.6 FPS cap down to
    // 21.3. Model every other frame before upscaling: 14 ms and 34 ms frames.
    {
        SteadyPacing pacing;
        constexpr double wobble[]{ 0.15, -0.10, -0.05, 0.08, -0.08 };
        double low = 1e9, high = 0;
        for (int frame = 0; frame < 9000; ++frame) {
            const double cost = frame % 2 ? 34000 : 14000;
            const double held = Paced(pacing, cost);
            pacing.Observe(pacing.TargetUs() > 0 && cost < pacing.TargetUs() ? held * (1 + wobble[frame % 5]) : held);
            if (frame > 1200) { low = (std::min)(low, pacing.TargetUs()); high = (std::max)(high, pacing.TargetUs()); }
        }
        // Probing may dip a few percent under the 34 ms frames before the average notices.
        Require(low >= 32500 && high <= 36500, "limiter wobble does not walk the cap away");
    }
    // Lighter scenes are found by probing, slowly.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 600; ++frame) { pacing.Observe(Paced(pacing, 22000)); }
        const double before = pacing.TargetUs();
        for (int frame = 0; frame < 6000; ++frame) { pacing.Observe(Paced(pacing, 14000)); }
        Require(pacing.TargetUs() < before * 0.9, "a lighter scene lets the cap come down");
    }
    // Loading screens and alt-tab start over instead of capping at their gap.
    {
        SteadyPacing pacing;
        for (int frame = 0; frame < 300; ++frame) { pacing.Observe(16000); }
        Require(pacing.TargetUs() > 0, "measured");
        Require(!pacing.Observe(900000) && pacing.TargetUs() == 0, "a long gap resets the measurement");
        Require(!pacing.Observe(-1) && !pacing.Observe(0), "invalid intervals are ignored");
    }
    // The cap converts to the output limit frame generation's limiters apply.
    Require(SteadyOutputLimit(20000, 1) == 50, "no generation: rendered rate");
    Require(SteadyOutputLimit(20000, 4) == 200, "x4: four outputs per rendered frame");
    Require(SteadyOutputLimit(0, 4) == 0 && SteadyOutputLimit(20000, 0) == 0, "unmeasured or invalid: no cap");
    Require(SteadyOutputLimit(500, 4) == 1000, "clamped to the limiters' range");
    std::puts("Steady pacing: alternating cost, heavier and lighter scenes, limiter wobble, gaps and output conversion passed.");
}
