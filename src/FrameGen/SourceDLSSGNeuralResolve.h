#pragma once
#include "SourceDLSSGInterop.h"
#include <array>
#include <span>

namespace TheosRenderPipeline::SourceDLSSG
{
	enum class ResolveKernel : unsigned { Downsample, Residual, Ratio, PackDepth, PackMotion, PrepareColor, PackGuides, ResizeColor, RestoreSecond, Split, MeasureExposure };
	struct ResolveConstants
	{
		UINT sourceWidth{}, sourceHeight{}, targetWidth{}, targetHeight{};
		UINT sourceIsBGRA{}, mode{}, passthrough{ 1 }, producerColor{};
		UINT workWidth{}, workHeight{};
		float transferStrength{ 1 }, colourStrength{ 1 }, maxRatio{ 2 }, whitePoint{ 1 };
		UINT peripheral{}, encodedFormat{}; // 0 float32, 1 float16, 2 UNORM8 intermediate
		UINT guideWidth{}, guideHeight{};
		float motionScaleX{ 1 }, motionScaleY{ 1 };
		UINT splitX{};
		float exposureBias{}, exposureRate{ 1 };
		UINT adaptiveExposure{}, exposureReset{};
		float jitterX{}, jitterY{};
	};
	static_assert(sizeof(ResolveConstants) == 108);
	// All resources enter and leave COMMON. Slot retirement is the caller's
	// responsibility. Each dispatch in a frame needs a distinct stage index.
	class NeuralResolveKernels
	{
	public:
		static constexpr unsigned kStages = 13;
		HRESULT Initialize(ID3D12Device* device);
		// The measured white point bound at t3 for this frame's dispatches, or null.
		void SetExposure(ID3D12Resource* exposure) { exposure_ = exposure; }
		HRESULT Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned stage,
			ResolveKernel kernel, const ResolveConstants& constants, ID3D12Resource* a, ID3D12Resource* b,
			ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* secondOutput = nullptr);
		// Test observation: descriptor tables written since initialization.
		std::uint64_t DescriptorWrites() const { return descriptorWrites_; }
	private:
		struct DescriptorTable
		{
			// Retain identities so a released resource's address cannot be reused
			// and mistaken for an unchanged table. Rewrite only a retired slot.
			std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 6> resources;
			UINT width{}, height{};
			bool valid{};
		};
		std::array<std::array<DescriptorTable, kStages>, kCommandSlots> tables_;
		std::uint64_t descriptorWrites_{};
		Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
		std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 11> pipelines_;
		ID3D12Resource* exposure_{}; // Owned by the pass.
		std::array<std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kStages>, kCommandSlots> heaps_;
	};
}
