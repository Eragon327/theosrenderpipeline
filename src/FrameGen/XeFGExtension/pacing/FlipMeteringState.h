#pragma once
#include <cstdint>

namespace sdk_pacing::detail
{
// Driver configuration survives until an explicit off call succeeds.
struct FlipMeteringState
{
    static constexpr std::uint32_t FailureLimit = 8;
    static constexpr std::uint32_t NoIndependentFlip = 1;
    bool configured{}, metered{}, refused{};
    std::uint32_t outputs{}, feedback{}, consecutiveFailures{};
    std::int64_t spacing{}, batchNs{};
    int status{};

    // False means an outstanding configuration could not be disabled. The
    // caller must retain the queue/owner and prohibit provider replacement.
    bool Record(std::uint32_t requestedOutputs, std::int64_t requestedBatchNs, int result,
        std::uint32_t driverFeedback) noexcept
    {
        status = result;
        feedback = driverFeedback;
        if (!requestedOutputs && status != 0 && configured) return false;
        if (status == 0) consecutiveFailures = 0;
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
