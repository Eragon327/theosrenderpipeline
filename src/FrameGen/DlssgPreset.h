#pragma once

#include <cstdint>
#include <string>

namespace TheosRenderPipeline::SourceDLSSG
{
	// NVIDIA's Frame Generation model presets, as DLSS-G 310.9.16 resolves them:
	// A interpolates the UI with the scene, B generates the HUD-less scene and UI
	// separately, C uses Lite candidate blending. B and C are exclusive in DLSS-G.
	// XeFG has no Lite blending; it reads only whether the UI is recomposed.
	enum class DlssgPreset : int { A = 1, B = 2, C = 3 };

	constexpr bool ValidPreset(int value) { return value >= 1 && value <= 3; }
	constexpr bool UsesUIRecomposition(DlssgPreset preset) { return preset == DlssgPreset::B; }
	constexpr bool UsesLiteBlending(DlssgPreset preset) { return preset == DlssgPreset::C; }

	constexpr const char* PresetLabel(DlssgPreset preset)
	{
		switch (preset) {
		case DlssgPreset::A: return "Preset A: Standard";
		case DlssgPreset::C: return "Preset C: Lite blending (beta)";
		default: return "Preset B: UI recomposition";
		}
	}

	// NVIDIA driver profile value 0x10E41DF1: 1-15 name a preset letter, 0xFFFFFF is
	// "latest" (DLSS-G 310.9.16 treats it as B), 0 is no override.
	inline std::string DriverPresetName(std::uint32_t value)
	{
		if (value == 0xFFFFFF) { return "Latest"; }
		return value >= 1 && value <= 15 ? std::string(1, static_cast<char>('A' + value - 1)) : std::string{};
	}
}
