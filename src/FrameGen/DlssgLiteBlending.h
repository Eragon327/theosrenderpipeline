#pragma once

#include <Windows.h>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace TheosRenderPipeline::SourceDLSSG::LiteBlending
{
	// DLSS-G 310.9.16 reads this at EndpointCore::Create; NVIDIA's Frame
	// Generation Preset C sets the same state from the driver profile.
	inline constexpr std::string_view kParameter = "DLSSG.LiteCandidateBlendingEnabled";
	inline constexpr std::size_t kPrologueBytes = 13;

	// push rbx; push rbp; push rsi; push r14; sub rsp, imm32. None of these is
	// RIP-relative, so the stub can replay them verbatim. Anything else is refused.
	inline bool MatchesCreateFeaturePrologue(std::span<const std::uint8_t> bytes)
	{
		constexpr std::array<std::uint8_t, 9> fixed{ 0x40, 0x53, 0x55, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC };
		return bytes.size() >= kPrologueBytes && std::equal(fixed.begin(), fixed.end(), bytes.begin());
	}

	// The build implements Lite blending when it names the parameter.
	inline bool NamesParameter(std::span<const std::uint8_t> image)
	{
		return std::string_view(reinterpret_cast<const char*>(image.data()), image.size()).find(kParameter) != std::string_view::npos;
	}

	enum class HookState { NotInstalled, Installed, UnsupportedPrologue, Failed };
	struct Snapshot
	{
		HookState hook{ HookState::NotInstalled };
		bool runtimeSupports{};
		std::uint32_t requests{};
	};

	// Install once on the retained DLSS-G module, before frame generation is created.
	// The hook only adds the parameter while Configure(true) is in effect.
	bool Install(HMODULE provider);
	void Configure(bool enabled);
	Snapshot State();
	const char* Describe(const Snapshot& state, bool enabled);
}
