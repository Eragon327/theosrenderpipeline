#pragma once

#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace TheosRenderPipeline::SourceDLSSG
{
	// A runtime whose kernels do not suit this GPU can return an all-black (or
	// NaN) image without any API error: the community int8 v2 build on RTX 40,
	// for one. The guard compares a patch of NR's input and output a few times a
	// second; if the output stays black while the input is lit, the backend falls
	// back to NVIDIA's runtime, or turns NR off.
	struct PatchStats
	{
		bool supported{};
		std::uint32_t texels{}, lit{}, dark{};
	};

	namespace NeuralGuardDetail
	{
		inline float Half(std::uint16_t h)
		{
			const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
			if (exponent == 0x1F) { return mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity(); }
			if (exponent == 0) { return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24); }
			return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
		}
		// Unsigned small floats with a 5-bit exponent (R11G11B10).
		inline float Small(std::uint32_t bits, std::uint32_t mantissaBits)
		{
			const std::uint32_t exponent = bits >> mantissaBits, mantissa = bits & ((1u << mantissaBits) - 1);
			if (exponent == 0x1F) { return mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity(); }
			const float fraction = static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits);
			return exponent == 0 ? std::ldexp(fraction, -14) : std::ldexp(1.0f + fraction, static_cast<int>(exponent) - 15);
		}
		inline std::uint32_t Bytes(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
			case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 4;
			default: return 0;
			}
		}
		inline void Rgb(DXGI_FORMAT format, const std::uint8_t* texel, float (&rgb)[3])
		{
			std::uint32_t packed{};
			switch (format) {
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				for (int c = 0; c < 3; ++c) { std::uint16_t h; std::memcpy(&h, texel + c * 2, 2); rgb[c] = Half(h); }
				return;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(rgb, texel, 12); return;
			case DXGI_FORMAT_R11G11B10_FLOAT:
				std::memcpy(&packed, texel, 4);
				rgb[0] = Small(packed & 0x7FF, 6); rgb[1] = Small((packed >> 11) & 0x7FF, 6); rgb[2] = Small(packed >> 22, 5);
				return;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				std::memcpy(&packed, texel, 4);
				for (int c = 0; c < 3; ++c) { rgb[c] = static_cast<float>((packed >> (c * 10)) & 0x3FF) / 1023.0f; }
				return;
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				for (int c = 0; c < 3; ++c) { rgb[c] = texel[2 - c] / 255.0f; }
				return;
			default:
				for (int c = 0; c < 3; ++c) { rgb[c] = texel[c] / 255.0f; }
				return;
			}
		}
	}

	// Dark: no finite colour channel above 0.001 (zero, tiny, negative, NaN or
	// infinite all count). Lit: the brightest channel exceeds 2% of white.
	inline PatchStats ClassifyPatch(DXGI_FORMAT format, const std::uint8_t* data, std::uint32_t width, std::uint32_t height,
		std::uint32_t rowPitch)
	{
		PatchStats stats{};
		const auto bytes = NeuralGuardDetail::Bytes(format);
		if (!bytes || !data || !width || !height || rowPitch < width * bytes) { return stats; }
		stats.supported = true;
		for (std::uint32_t y = 0; y < height; ++y) {
			for (std::uint32_t x = 0; x < width; ++x) {
				float rgb[3];
				NeuralGuardDetail::Rgb(format, data + y * rowPitch + x * bytes, rgb);
				float brightest = 0;
				for (const float value : rgb) {
					if (std::isfinite(value)) { brightest = (std::max)(brightest, value); }
				}
				++stats.texels;
				stats.dark += brightest <= 0.001f;
				stats.lit += brightest > 0.02f;
			}
		}
		return stats;
	}

	// Trips after kStrikes consecutive samples with a lit input and an entirely
	// dark output. Dark input says nothing about the runtime and is skipped.
	class NeuralBlackDetector
	{
	public:
		static constexpr int kStrikes = 4;
		bool Add(const PatchStats& input, const PatchStats& output)
		{
			if (!input.supported || !output.supported || !input.texels || !output.texels) { return false; }
			if (input.lit * 4 < input.texels) { return false; }
			if (output.dark != output.texels) { strikes_ = 0; return false; }
			return ++strikes_ == kStrikes;
		}
		void Reset() { strikes_ = 0; }
	private:
		int strikes_{};
	};

	// Copies a centred 16x16 patch of NR's input and output every kInterval
	// recorded frames, read back after the queue passes a fence; never waits.
	// Resources enter and leave COMMON, as everywhere in the NR pass.
	class NeuralOutputGuard
	{
	public:
		static constexpr std::uint32_t kBox = 16;
		static constexpr std::uint64_t kInterval = 15;

		// Why the last due sample was not recorded, or nullptr.
		const char* Skipped() const { return skipped_; }
		DXGI_FORMAT InputFormat() const { return patches_[0].format; }
		DXGI_FORMAT OutputFormat() const { return patches_[1].format; }

		bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output)
		{
			if (recorded_ || submitted_ || ++frames_ % kInterval) { return false; }
			skipped_ = nullptr;
			if (!device || !list || !input || !output) { skipped_ = "missing resource"; return false; }
			if (!fence_ && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { skipped_ = "fence"; return false; }
			for (int i = 0; i < 2; ++i) {
				auto& patch = patches_[i];
				auto* resource = i ? output : input;
				const auto desc = resource->GetDesc();
				if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
					desc.Width < kBox || desc.Height < kBox) { skipped_ = "unsupported resource shape"; return false; }
				if (!NeuralGuardDetail::Bytes(desc.Format)) { patch.format = desc.Format; skipped_ = "unsupported format"; return false; }
				if (!patch.buffer || patch.format != desc.Format) {
					auto box = desc; box.Width = kBox; box.Height = kBox; box.MipLevels = 1; box.DepthOrArraySize = 1;
					box.Flags = D3D12_RESOURCE_FLAG_NONE;
					UINT64 total{};
					device->GetCopyableFootprints(&box, 0, 1, 0, &patch.footprint, nullptr, nullptr, &total);
					D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
					D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					buffer.Width = total; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
					buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
					patch.buffer.Reset();
					if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
						D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&patch.buffer)))) { skipped_ = "readback allocation"; return false; }
					patch.format = desc.Format;
				}
				patch.left = static_cast<UINT>((desc.Width - kBox) / 2);
				patch.top = (desc.Height - kBox) / 2;
			}
			D3D12_RESOURCE_BARRIER barriers[2]{};
			for (int i = 0; i < 2; ++i) {
				barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[i].Transition = { i ? output : input, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
					D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE };
			}
			list->ResourceBarrier(input == output ? 1 : 2, barriers);
			for (int i = 0; i < 2; ++i) {
				auto& patch = patches_[i];
				D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = patch.buffer.Get();
				destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = patch.footprint;
				D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = i ? output : input;
				source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				const D3D12_BOX box{ patch.left, patch.top, 0, patch.left + kBox, patch.top + kBox, 1 };
				list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
			}
			for (auto& barrier : barriers) { std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); }
			list->ResourceBarrier(input == output ? 1 : 2, barriers);
			recorded_ = true;
			return true;
		}

		// Only after the list containing Record has been submitted to `queue`.
		void Submitted(ID3D12CommandQueue* queue)
		{
			if (!recorded_) { return; }
			recorded_ = false;
			submitted_ = queue && SUCCEEDED(queue->Signal(fence_.Get(), ++value_));
		}

		// True once per completed sample.
		bool Poll(PatchStats& input, PatchStats& output)
		{
			if (!submitted_ || fence_->GetCompletedValue() < value_) { return false; }
			submitted_ = false;
			PatchStats* out[2]{ &input, &output };
			for (int i = 0; i < 2; ++i) {
				auto& patch = patches_[i];
				void* mapped{};
				const D3D12_RANGE range{ 0, static_cast<SIZE_T>(patch.footprint.Footprint.RowPitch) * kBox };
				*out[i] = {};
				if (FAILED(patch.buffer->Map(0, &range, &mapped))) { continue; }
				*out[i] = ClassifyPatch(patch.format, static_cast<const std::uint8_t*>(mapped), kBox, kBox,
					patch.footprint.Footprint.RowPitch);
				const D3D12_RANGE none{};
				patch.buffer->Unmap(0, &none);
			}
			return true;
		}

	private:
		struct Patch
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
			UINT left{}, top{};
		} patches_[2];
		Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
		std::uint64_t value_{}, frames_{};
		bool recorded_{}, submitted_{};
		const char* skipped_{};
	};
}
