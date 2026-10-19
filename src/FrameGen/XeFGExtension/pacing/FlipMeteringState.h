#pragma once
#include <cstdint>

namespace sdk_pacing::detail
{
// Driver configuration survives until an explicit off call succeeds.
struct FlipMeteringState
{
    static constexpr std::uint32_t FailureLimit = 8;
    static constexpr std::uint32_t NoIndependentFlip = 1;
    bool probed{}, admitted{}, configured{}, metered{}, refused{};
    std::uint32_t outputs{}, feedback{}, consecutiveFailures{};
    std::int64_t spacing{}, batchNs{};
    int status{};

    // Called once, with the result of an off-only capability probe before any
    // enable attempt. Refusing that probe cannot leave our metering configured.
    bool Probe(int result, std::uint32_t driverFeedback) noexcept
    {
        if (probed) return admitted;
        probed = true;
        status = result;
        feedback = driverFeedback;
        admitted = result == 0;
        refused = !admitted;
        return admitted;
    }
    bool CanEnable() const noexcept { return admitted && !refused; }

    // False means an outstanding configuration could not be disabled. The
    // caller must retain the queue/owner and prohibit provider replacement.
    bool Record(std::uint32_t requestedOutputs, std::int64_t requestedBatchNs, int result,
        std::uint32_t driverFeedback) noexcept
    {
        status = result;
        feedback = driverFeedback;
        if (!requestedOutputs && status != 0 && configured) return false;
        // Off acknowledges cleanup, not support for enabling metering. Keep
        // failed enables across passthrough/unknown-interval off calls.
        if (status == 0 && requestedOutputs) consecutiveFailures = 0;
        else if (requestedOutputs && ++consecutiveFailures >= FailureLimit) refused = true;
        if (status != 0) {
            // A failed update does not acknowledge that the preceding driver
            // configuration is gone. Even an initial enable refusal needs an
            // explicit off acknowledgement before its queue can be released.
            configured = configured || requestedOutputs != 0;
            metered = false;
            return true;
        }
        configured = requestedOutputs != 0;
        metered = configured && !(feedback & NoIndependentFlip);
        outputs = configured ? requestedOutputs : 0;
        batchNs = configured ? requestedBatchNs : 0;
        spacing = configured ? requestedBatchNs / requestedOutputs : 0;
        return true;
    }
};
}
