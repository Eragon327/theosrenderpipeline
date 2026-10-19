#pragma once
#include "D3D11FrameCopy.h"
#include <algorithm>
#include <cstdint>

namespace TheosRenderPipeline
{
    struct UpscalerInputResolution
    {
        FrameExtent optimal{}, minimum{}, maximum{};
        bool Valid() const
        {
            return minimum.width && minimum.height &&
                minimum.width <= optimal.width && optimal.width <= maximum.width &&
                minimum.height <= optimal.height && optimal.height <= maximum.height;
        }
        bool Accepts(FrameExtent input, FrameExtent output) const
        {
            if (!Valid() || !output.width || !output.height ||
                input.width < minimum.width || input.width > maximum.width ||
                input.height < minimum.height || input.height > maximum.height ||
                input.width > output.width || input.height > output.height) { return false; }
            // Preserve output aspect, allowing one pixel for rounded scaled extents.
            // Use integer products so near-maximum UINT dimensions cannot overflow.
            const std::uint64_t x = std::uint64_t(input.width) * output.height;
            const std::uint64_t y = std::uint64_t(input.height) * output.width;
            const auto difference = x > y ? x - y : y - x;
            return difference <= (std::max)(output.width, output.height);
        }
    };
}
