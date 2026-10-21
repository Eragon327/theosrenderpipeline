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
    // backs off 3% when more than 5% of frames exceed the cap by 2%, and after about five
    // seconds without misses tries a 1% faster cap. Long gaps (loading, menus,
    // alt-tab) are ignored, and a long pause starts over.
    class SteadyPacing
    {
    public:
        static constexpr std::size_t kWindow = 120;
        static constexpr std::size_t kEvaluateEvery = 60;
        static constexpr std::size_t kMissesAllowed = kWindow / 20;
        static constexpr double kHeadroom = 1.03, kMissTolerance = 1.02, kBackOff = 1.03, kProbe = 0.99;
        static constexpr double kMinimumUs = 1000.0, kMaximumUs = 250000.0;

        // One rendered frame's interval since the previous one. Returns true when
        // the target changed.
        bool Observe(double intervalUs)
        {
            if (!std::isfinite(intervalUs) || intervalUs <= 0) { return false; }
            if (intervalUs > kMaximumUs) { Reset(); return false; }
            intervals_[next_] = intervalUs;
            next_ = (next_ + 1) % kWindow;
            count_ = (std::min)(count_ + 1, kWindow);
            if (count_ < kWindow || ++sinceEvaluation_ < kEvaluateEvery) { return false; }
            sinceEvaluation_ = 0;
            const double before = targetUs_;
            if (targetUs_ <= 0) {
                targetUs_ = Clamp(Percentile(0.9) * kHeadroom);
            } else if (Misses() > kMissesAllowed) {
                targetUs_ = Clamp(targetUs_ * kBackOff);
                stableFrames_ = 0;
            } else if ((stableFrames_ += kEvaluateEvery) >= 300) {
                targetUs_ = Clamp(targetUs_ * kProbe);
                stableFrames_ = 0;
            }
            return targetUs_ != before;
        }
        void Reset() { count_ = next_ = sinceEvaluation_ = stableFrames_ = 0; targetUs_ = 0; }
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
        std::size_t Misses() const
        {
            return static_cast<std::size_t>(std::count_if(intervals_.begin(), intervals_.end(),
                [&](double interval) { return interval > targetUs_ * kMissTolerance; }));
        }
        std::array<double, kWindow> intervals_{};
        std::size_t next_{}, count_{}, sinceEvaluation_{}, stableFrames_{};
        double targetUs_{};
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
