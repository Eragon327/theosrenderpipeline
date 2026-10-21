#pragma once
#include "NeuralRenderingReconstruction.h"
#include "NeuralRenderingTuning.h"
#include <algorithm>
#include <array>
#include <string>

namespace TheosRenderPipeline::NeuralRendering
{
	// Pass 1 plus up to four extra passes, each running the model again on the
	// previous pass's result with its own feature and history.
	inline constexpr int kMaxNeuralPasses = 5;
	inline constexpr std::size_t kExtraPasses = kMaxNeuralPasses - 1;

	// The settings of one extra pass (Pass 2 to 5). Each follows Pass 1, never the
	// pass before it, until one of its own settings changes.
	struct ExtraPassSettings
	{
		bool linked{ true };
		float inputScale{ 1 };
		int preset{};
		Tuning tuning{};
		bool operator==(const ExtraPassSettings&) const = default;
	};
	using ExtraPasses = std::array<ExtraPassSettings, kExtraPasses>;
	inline ExtraPassSettings SanitizeExtraPass(ExtraPassSettings value)
	{
		value.inputScale = NormalizeInputScale(value.inputScale);
		if (value.preset < 0 || value.preset > 1) { value.preset = 0; }
		value.tuning = SanitizeBuild14Tuning(value.tuning);
		return value;
	}
	inline ExtraPasses SanitizeExtraPasses(ExtraPasses value)
	{
		for (auto& pass : value) { pass = SanitizeExtraPass(pass); }
		return value;
	}
	inline ExtraPassSettings EffectivePass(const ExtraPassSettings& value, const Reconstruction& first, const Tuning& tuning)
	{
		return SanitizeExtraPass(value.linked ? ExtraPassSettings{ true, first.inputScale, first.preset, tuning } : value);
	}
	inline ExtraPasses EffectiveExtraPasses(const ExtraPasses& value, const Reconstruction& first, const Tuning& tuning)
	{
		ExtraPasses result;
		for (std::size_t i = 0; i < value.size(); ++i) { result[i] = EffectivePass(value[i], first, tuning); }
		return result;
	}
	// "NRPass2" to "NRPass5": Pass 2's released keys keep their names.
	inline std::string ExtraPassKey(std::size_t index, const char* name)
	{
		return "NRPass" + std::to_string(index + 2) + name;
	}
	template<class Ini> ExtraPassSettings LoadExtraPass(const Ini& ini, const char* section, std::size_t index,
		const Reconstruction& first, const Tuning& tuning)
	{
		// Missing overrides inherit the old shared settings. Relinking never erases
		// saved overrides, so experimenting with the checkbox is reversible.
		const auto key = [&](const char* name) { return ExtraPassKey(index, name); };
		ExtraPassSettings value;
		value.linked = ini.GetBoolValue(section, key("UseSameSettings").c_str(), true);
		value.inputScale = static_cast<float>(ini.GetDoubleValue(section, key("InputScale").c_str(), first.inputScale));
		value.preset = static_cast<int>(ini.GetLongValue(section, key("Preset").c_str(), first.preset));
		auto& t = value.tuning;
		t.style = std::clamp(static_cast<int>(ini.GetLongValue(section, key("Style").c_str(), tuning.style)), 0, 255);
		t.intensity = static_cast<float>(ini.GetDoubleValue(section, key("Intensity").c_str(), tuning.intensity));
		t.localToneStrength = static_cast<float>(ini.GetDoubleValue(section, key("LocalTone").c_str(), tuning.localToneStrength));
		t.localStructureStrength = static_cast<float>(ini.GetDoubleValue(section, key("LocalStructure").c_str(), tuning.localStructureStrength));
		t.skinStructureStrength = static_cast<float>(ini.GetDoubleValue(section, key("SkinStructure").c_str(), tuning.skinStructureStrength));
		t.useAutoSkinMask = ini.GetBoolValue(section, key("AutoSkinMask").c_str(), tuning.useAutoSkinMask);
		t.uiCorrection = ini.GetBoolValue(section, key("UICorrection").c_str(), tuning.uiCorrection);
		return SanitizeExtraPass(value);
	}
	template<class Ini> ExtraPasses LoadExtraPasses(const Ini& ini, const char* section, const Reconstruction& first, const Tuning& tuning)
	{
		ExtraPasses value;
		for (std::size_t i = 0; i < value.size(); ++i) { value[i] = LoadExtraPass(ini, section, i, first, tuning); }
		return value;
	}
	template<class Ini> void StoreExtraPasses(Ini& ini, const char* section, ExtraPasses passes)
	{
		for (std::size_t i = 0; i < passes.size(); ++i) {
			const auto value = SanitizeExtraPass(passes[i]);
			const auto key = [&](const char* name) { return ExtraPassKey(i, name); };
			ini.SetBoolValue(section, key("UseSameSettings").c_str(), value.linked);
			ini.SetDoubleValue(section, key("InputScale").c_str(), value.inputScale);
			ini.SetLongValue(section, key("Preset").c_str(), value.preset);
			const auto& t = value.tuning;
			ini.SetLongValue(section, key("Style").c_str(), t.style);
			ini.SetDoubleValue(section, key("Intensity").c_str(), t.intensity);
			ini.SetDoubleValue(section, key("LocalTone").c_str(), t.localToneStrength);
			ini.SetDoubleValue(section, key("LocalStructure").c_str(), t.localStructureStrength);
			ini.SetDoubleValue(section, key("SkinStructure").c_str(), t.skinStructureStrength);
			ini.SetBoolValue(section, key("AutoSkinMask").c_str(), t.useAutoSkinMask);
			ini.SetBoolValue(section, key("UICorrection").c_str(), t.uiCorrection);
		}
	}

	// How many NGX features exist and how many passes may evaluate this frame.
	struct FeatureRamp
	{
		int created{};
		int ready{};
		bool operator==(const FeatureRamp&) const = default;
	};
	// The first frame creates Pass 1 and 2 and runs them, as before. Each later
	// pass's feature is created alone, at most one per frame, and first evaluated
	// on a later frame: creating and evaluating several new features in one
	// command list has hung the GPU in other NR tools. Until the rest are ready,
	// only the ready prefix of passes runs.
	inline FeatureRamp NextFeatureRamp(int created, int passes)
	{
		passes = std::clamp(passes, 1, kMaxNeuralPasses);
		if (created <= 0) {
			const int first = (std::min)(passes, 2);
			return { first, first };
		}
		created = (std::min)(created, passes);
		return { (std::min)(created + 1, passes), created };
	}
}
