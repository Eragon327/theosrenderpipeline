#include "NeuralRuntimeAdaKernels.h"

namespace TheosRenderPipeline::NeuralRendering
{
	bool AdaKernelsSupported() { return false; }

	AdaKernelResult ApplyAdaKernels(HMODULE, const std::filesystem::path&, bool)
	{
		return { false, false, "the new NR model on RTX 40 needs the Universal edition" };
	}
}
