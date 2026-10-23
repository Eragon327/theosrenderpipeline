#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace TheosRenderPipeline::SourceDLSSG
{
    // Fixed caps output at the saved FPS; Steady holds rendered frames to the
    // interval the slower frames sustain (SteadyPacing below). Live.
    enum class FrameLimitMode : int { Off, Fixed, Steady };
}

namespace TheosRenderPipeline
{
    // Steady frame limit: find the rendered-frame interval the slower frames can
    // sustain and hold every frame to it, so an alternating cost (a model that
    // runs every other frame, uneven scenes) becomes an even cadence instead of
    // stutter. It starts just above the 90th percentile of recent intervals,
    // backs off 3% when the average interval runs more than 1.5% over the cap,
    // and after about five steady seconds tries a 1% faster cap. A faster cap
    // that does not hold returns to the cap before it, and the next try waits
    // twice as long (up to about a minute): the 2026-10-22 game test showed a
    // probe, a 3% back-off below the cap that held, and another probe every
    // few seconds, a sawtooth felt as periodic stutter. The average,
    // not single late frames, decides: while the limiter holds frames to the
    // cap their intervals wobble around it but average to it, and counting each
    // late one as a miss walked the cap down without end. Each decision looks
    // only at the frames since the previous one. A long pause starts over.
    class SteadyPacing
    {
    public:
        static constexpr std::size_t kWindow = 120;
        static constexpr std::size_t kEvaluateEvery = 60;
        static constexpr double kHeadroom = 1.03, kSlowTolerance = 1.015, kBackOff = 1.03, kProbe = 0.99;
        static constexpr double kMinimumUs = 1000.0, kMaximumUs = 250000.0;
        // Steady frames before a faster cap is tried; doubled after each failed try.
        static constexpr std::size_t kProbeAfter = 300, kProbeAfterMaximum = 3840;

        // One rendered frame's interval since the previous one. Returns true when
        // the target changed.
        bool Observe(double intervalUs)
        {
            if (!std::isfinite(intervalUs) || intervalUs <= 0) { return false; }
            if (intervalUs > kMaximumUs) { Reset(); return false; }
            intervals_[next_] = intervalUs;
            next_ = (next_ + 1) % kWindow;
            count_ = (std::min)(count_ + 1, kWindow);
            ++fresh_;
            const double before = targetUs_;
            if (targetUs_ <= 0) {
                if (count_ < kWindow) { return false; }
                targetUs_ = Clamp(Percentile(0.9) * kHeadroom);
            } else {
                // Each decision uses only the frames since the last one.
                if (fresh_ < kEvaluateEvery) { return false; }
                const bool slow = Mean(kEvaluateEvery) > targetUs_ * kSlowTolerance;
                if (slow && probing_) {
                    // The faster cap did not hold: back to the one that did, and wait longer.
                    targetUs_ = heldUs_;
                    probeAfter_ = (std::min)(probeAfter_ * 2, kProbeAfterMaximum);
                    stableFrames_ = 0;
                } else if (slow) {
                    // The scene got heavier.
                    targetUs_ = Clamp(targetUs_ * kBackOff);
                    probeAfter_ = kProbeAfter;
                    stableFrames_ = 0;
                } else if (probing_) {
                    probeAfter_ = kProbeAfter;  // the faster cap holds; keep looking
                } else if ((stableFrames_ += kEvaluateEvery) >= probeAfter_) {
                    heldUs_ = targetUs_;
                    targetUs_ = Clamp(targetUs_ * kProbe);
                    stableFrames_ = 0;
                }
                probing_ = targetUs_ != before && !slow;
            }
            fresh_ = 0;
            return targetUs_ != before;
        }
        void Reset()
        {
            count_ = next_ = fresh_ = stableFrames_ = 0;
            targetUs_ = heldUs_ = 0;
            probing_ = false;
            probeAfter_ = kProbeAfter;
        }
        // 0 until the first full window has been measured.
        double TargetUs() const { return targetUs_; }
        double TargetFPS() const { return targetUs_ > 0 ? 1000000.0 / targetUs_ : 0; }

    private:
        static double Clamp(double us) { return std::clamp(us, kMinimumUs, kMaximumUs); }
        double Percentile(double fraction) const
        {
            auto sorted = intervals_;
            const auto index = static_cast<std::size_t>(fraction * (kWindow - 1));
            std::nth_element(sorted.begin(), sorted.begin() + index, sorted.end());
            return sorted[index];
        }
        // The average of the newest n intervals.
        double Mean(std::size_t n) const
        {
            double sum = 0;
            for (std::size_t i = 1; i <= n; ++i) { sum += intervals_[(next_ + kWindow - i) % kWindow]; }
            return sum / n;
        }
        std::array<double, kWindow> intervals_{};
        std::size_t next_{}, count_{}, fresh_{}, stableFrames_{}, probeAfter_{ kProbeAfter };
        double targetUs_{}, heldUs_{};
        bool probing_{};  // the last decision tried a faster cap
    };

    // The output cap that holds rendered frames to a steady interval: frame
    // generation's limiters (Reflex with DLSS-G, XeLL with XeFG) cap output.
    inline int SteadyOutputLimit(double renderedIntervalUs, unsigned outputsPerRenderedFrame)
    {
        if (renderedIntervalUs <= 0 || !outputsPerRenderedFrame) { return 0; }
        const auto fps = static_cast<int>(std::floor(1000000.0 / renderedIntervalUs * outputsPerRenderedFrame));
        return std::clamp(fps, 1, 1000);
    }
}
