#pragma once
#include <cstdint>

namespace TheosRenderPipeline
{
    // Intel's XeFG pacing remembers long frames it generated across and then
    // holds later Presents for up to about 250 ms, fading over dozens of frames;
    // passthrough frames neither feed nor clear that memory. In the presenter
    // fixture six 500 ms frames while generating cost about 25 slow Presents;
    // the same frames in passthrough cost none. After a new presenter or a
    // transition, generation therefore waits for settled frames. Once settled it
    // stays allowed until the next reset: toggling generation mid-play would not
    // clear Intel's memory and would only add hitches.
    class XeFGSettleGate
    {
    public:
        static constexpr double kSlowMs = 100.0;
        static constexpr std::uint32_t kRequired = 10;
        void Reset() { settled_ = 0; held_ = 0; }
        // intervalMs: time since the previous host frame; 0 when unknown.
        bool Observe(double intervalMs)
        {
            if (settled_ >= kRequired) { return true; }
            ++held_;
            settled_ = intervalMs > 0 && intervalMs < kSlowMs ? settled_ + 1 : 0;
            return settled_ >= kRequired;
        }
        bool Settled() const { return settled_ >= kRequired; }
        // Frames observed before settling since the last reset.
        std::uint32_t Held() const { return held_; }
    private:
        std::uint32_t settled_{}, held_{};
    };
}
