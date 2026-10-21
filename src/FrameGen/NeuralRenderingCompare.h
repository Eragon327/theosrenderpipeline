#pragma once
#include <algorithm>
#include <cmath>

namespace TheosRenderPipeline::NeuralRendering
{
	// Session-only comparison aids. Never saved and never a reason to rebuild
	// the NR feature or reset its history.
	struct CompareSettings
	{
		// Left of splitPosition (0..1 of the width) shows the frame without NR.
		bool split{};
		float splitPosition{ 0.5f };
		// NR re-evaluates one held frame, with zero motion, while the game runs
		// on. After upscaling only: before it, DLSS needs a moving frame.
		bool freeze{};
		bool operator==(const CompareSettings&) const = default;
	};

	inline CompareSettings SanitizeCompare(CompareSettings value)
	{
		value.splitPosition = std::isfinite(value.splitPosition) ? std::clamp(value.splitPosition, 0.0f, 1.0f) : 0.5f;
		return value;
	}

	// The first column that shows NR's result.
	inline unsigned SplitColumn(const CompareSettings& value, unsigned width)
	{
		return static_cast<unsigned>(std::lround(SanitizeCompare(value).splitPosition * static_cast<float>(width)));
	}
}
