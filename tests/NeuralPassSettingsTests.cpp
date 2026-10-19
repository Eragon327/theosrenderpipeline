#include "FrameGen/SourceDLSSGSettings.h"
#include "FrameGen/SourceDLSSGNeuralState.h"
#include <SimpleIni.h>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void Require(bool value, const char* why)
{
	if (!value) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main()
{
	using namespace TheosRenderPipeline;
	using namespace SourceDLSSG;
	CSimpleIniA ini;
    {
        CSimpleIniA saved;
        Preferences values;
        Require(!values.neuralObjectMasks.enabled, "object masks default off");
        values.neuralObjectMasks = {true,{0,0.25f,0.5f,1}};
        StorePreferences(saved,values);
        Require(LoadPreferences(saved).neuralObjectMasks == values.neuralObjectMasks, "all four object mask rules round trip");
        for(unsigned pass=0;pass<2;++pass)for(unsigned group=0;group<4;++group){
            values.neuralObjectMasks.passes[pass].groups[group]={group%2==0,float(pass+group)/5,float(group)/4,float(pass+group)/8};
        }
        StorePreferences(saved,values);
        Require(LoadPreferences(saved).neuralObjectMasks==values.neuralObjectMasks,"both passes' group controls round trip independently");
        CSimpleIniA old;
        old.SetBoolValue("SourceDLSSG", "NRObjectMasks", true);
        old.SetBoolValue("SourceDLSSG", "NRObjectPass1Group1Enabled", true);
        const auto migrated=LoadPreferences(old).neuralObjectMasks;
        Require(migrated.enabled && migrated.passes[0].groups[0].enabled && migrated.strength==std::array<float,4>{0,0,0,0} && migrated.passes[0].groups[0].strength==1,
            "old final protection survives and a missing per-pass strength keeps the full pass");
        auto malformed=migrated;
        malformed.passes[0].groups[0].strength=std::numeric_limits<float>::quiet_NaN();
        malformed.passes[1].groups[0]={true,1,1,-3};
        malformed.passes[1].groups[1]={false,1,1,3};
        malformed=NeuralRendering::SanitizeObjectMask(malformed);
        Require(malformed.passes[0].groups[0].strength==1 && malformed.passes[1].Strengths()==std::array<float,4>{0,1,1,1},
            "invalid strengths sanitize and disabled groups retain full NR");
        values.neuralObjectMasks.preview=NeuralRendering::ObjectPreview::Classes;
        values.neuralObjectMasks.previewGroup=3;
        StorePreferences(saved,values);
        Require(LoadPreferences(saved).neuralObjectMasks.preview==NeuralRendering::ObjectPreview::Off,"diagnostic preview never persists");
        NeuralOptions options; options.enabled=true; options.automaticRuntime=true;
        options.objectMasks=values.neuralObjectMasks;
        NeuralHistory history; Require(history.ResetFor(options,true,false),"first object policy resets");
        Require(!history.ResetFor(options,true,false),"unchanged object policy retains history");
        options.objectMasks.preview=NeuralRendering::ObjectPreview::Overlay;
        options.objectMasks.previewGroup=2;
        Require(!history.ResetFor(options,true,false),"preview edits do not invalidate inference history");
        options.objectMasks.passes[1].groups[1].tone=.125f;
        Require(history.ResetFor(options,true,false),"per-pass object controls invalidate history");
        options.objectMasks.passes[1].groups[1].strength=.875f;
        Require(history.ResetFor(options,true,false),"per-pass strength edits invalidate history");
        options.objectMasks.strength[2]=0.75f;
        Require(history.ResetFor(options,true,false),"object policy edit resets NR history");
    }

	auto empty = LoadPreferences(ini);
	Require(!empty.xefg.experimentalMFG && empty.xefg.generatedFrames == 1, "old INIs retain official XeFG x2");
	Require(empty.neuralRuntime == kAutomaticNeuralRuntime, "NR defaults to Automatic");
	{
		CSimpleIniA runtime;
		auto chosen = empty;
		chosen.neuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll";
		StorePreferences(runtime, chosen);
		Require(std::string(runtime.GetValue("Experimental", "NeuralRenderingRuntimePath", "")) == chosen.neuralRuntime &&
			LoadPreferences(runtime).neuralRuntime == chosen.neuralRuntime, "the chosen runtime round trips through the existing key");
		chosen.neuralRuntime.clear();
		Require(SanitizePreferences(chosen).neuralRuntime == kAutomaticNeuralRuntime, "an empty choice returns to Automatic");
	}
	empty.xefg = {true, 5};
	empty.generation.generatedFrames = 5;
	StorePreferences(ini, empty);
	Require(LoadPreferences(ini) == empty, "XeFG x6 opt-in round trips without replacing NVIDIA x6");
	Require(!ini.GetValue("FrameGeneration", "XeFGExperimentalMFG"), "the separate opt-in key is retired");
	Require(SanitizePreferences(Preferences{.xefg = {false, 3}}).xefg.experimentalMFG &&
		!SanitizePreferences(Preferences{.xefg = {true, 1}}).xefg.experimentalMFG, "x3-x6 alone request the unlock");
	ini.SetBoolValue("FrameGeneration", "XeFGExperimentalMFG", false);
	Require(LoadPreferences(ini).xefg == XeFGOptions{false, 1}, "a development INI with the opt-in off stays x2");
	ini.SetBoolValue("FrameGeneration", "XeFGExperimentalMFG", true);
	Require(LoadPreferences(ini).xefg == XeFGOptions{true, 5}, "a development INI with the opt-in on keeps x6");
	ini.Delete("FrameGeneration", "XeFGExperimentalMFG");
	Require(XeFGCount({false, 3}, 3) == 1 && XeFGCount({true, 3}, 1) == 1, "disabled or refused MFG retains x2");
	Require(XeFGCount({true, 3}, 3) == 3 && XeFGCount({true, 2}, 3) == 2, "separate XeFG x3/x4 counts");
	Require(XeFGCount({true, 5}, 5) == 5 && XeFGCount({true, 5}, 3) == 3, "x6 respects admitted capacity");
	Require(XeFGNeedsRecreation(false, {true, 3}), "x3-x6 on a presenter created without the unlock recreates it");
	Require(!XeFGNeedsRecreation(true, {false, 1}) && !XeFGNeedsRecreation(true, {true, 3}),
		"a presenter that tried the unlock serves x2-x6 live, including a refused unlock at x2");
	Require(!XeFGNeedsRecreation(false, {false, 1}), "official x2 never recreates");
	Require(XeFGCount({false, 5}, 5) == 1 && XeFGCount({true, 5}, 0) == 1,
		"resident unlock does not defeat disable or absent capacity");
	Require(SanitizeXeFG({true, 99}).generatedFrames == 5 && SanitizeXeFG({true, 0}).generatedFrames == 1,
		"bounded experimental counts");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 99);
	Require(LoadPreferences(ini).xefg.generatedFrames == 5, "excess INI multiplier clamps to x6");
	ini.SetLongValue("FrameGeneration", "XeFGMultiplier", 0);
	Require(LoadPreferences(ini).xefg.generatedFrames == 1, "invalid low INI multiplier clamps to x2");
	Require(XeFGOutputInterval(60) == 16667 && XeFGOutputInterval(0) == 0, "output cap does not scale with multiplier");
	ini.Reset();
	Require(ini.LoadData("[SourceDLSSG]\nNRPasses=2\nNRInputScale=0.75\nNRPreset=1\nNRIntensity=0.4\n") >= 0, "old INI");
	auto old = LoadPreferences(ini);
	Require(old.provider == FrameGenerationProvider::NVIDIA, "missing provider preserves NVIDIA startup");
	old.provider = FrameGenerationProvider::XeFG;
	old.generation.generatedFrames = 3;
	old.reflexMode = 2;
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old, "XeFG selection retains NVIDIA multiplier/latency and all NR settings");
	ini.SetLongValue("FrameGeneration", "Provider", 42);
	Require(LoadPreferences(ini).provider == FrameGenerationProvider::NVIDIA, "unknown provider safely defaults to NVIDIA");
	StorePreferences(ini, old);
	Require(!old.neuralCombat.Enabled() && old.neuralCombat.recoverySeconds == 5, "old INI keeps combat policy off");
	old.neuralCombat = {true, true, 7.5f};
	Require(old.neuralSecondPass.linked && old.neuralSecondPass.inputScale == .75f && old.neuralSecondPass.preset == 1 &&
		old.neuralSecondPass.tuning.intensity == .4f, "old config inherits first pass and stays linked");
	old.neuralSecondPass.linked = false;
	old.neuralSecondPass.inputScale = .25f;
	old.neuralSecondPass.preset = 0;
	old.neuralSecondPass.tuning = { 7, .25f, 1.5f, .3f, -1, true, true };
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old, "all pass 2 settings round trip independently");
	old.neuralSecondPass.linked = true;
	StorePreferences(ini, old);
	auto linked = LoadPreferences(ini);
	Require(linked == old, "relink preserves saved overrides");
	const auto effective = NeuralRendering::EffectiveSecondPass(linked.neuralSecondPass, linked.neuralReconstruction, linked.neuralTuning);
	Require(effective.inputScale == .75f && effective.preset == 1 && effective.tuning == linked.neuralTuning, "link overrides hidden custom values");
	NeuralOptions options; options.enabled = true; options.passes = 2; options.secondPass = old.neuralSecondPass;
	options.secondPass.linked = false; options.worldOnly = true;
	Require(!options.EffectiveSecond().tuning.uiCorrection && options.secondPass.tuning.uiCorrection, "world-only adapter suppresses UI correction without changing preference");
	NeuralHistory history;
	Require(history.ResetFor(options, true, false), "initial history");
	Require(!history.ResetFor(options, true, false), "stable history");
	options.secondPass.tuning.intensity = .8f;
	Require(history.ResetFor(options, true, false), "second tuning resets history");
	options.secondPass.linked = true;
	Require(history.ResetFor(options, true, false), "relink resets history");
	options.passOverride = NeuralRendering::PassOverride::Combat;
	Require(options.passes == 2 && options.EffectivePasses() == 1 && history.ResetFor(options, true, false),
		"override changes execution and resets image history without changing requested passes");
	options.passOverride = NeuralRendering::PassOverride::WeaponsDrawn;
	Require(!history.ResetFor(options, true, false), "reason-only changes preserve temporal history");
	options.passOverride = NeuralRendering::PassOverride::Recovery;
	Require(!history.ResetFor(options, true, false), "cooldown preserves temporal history");
	StorePreferences(ini, old);
	Require(LoadPreferences(ini) == old && LoadPreferences(ini).neuralPasses == 2,
		"saving while overridden preserves requested passes and independent tuning");
	options.passOverride = NeuralRendering::PassOverride::None;
	Require(options.EffectivePasses() == 2 && history.ResetFor(options, true, false), "restoration resets image history");
	options.combat = {true, false, 2};
	Require(!history.ResetFor(options, true, false), "policy settings alone do not reset image history");
	old.neuralSecondPass.inputScale = std::numeric_limits<float>::quiet_NaN();
	old.neuralSecondPass.preset = 99; old.neuralSecondPass.tuning.intensity = std::numeric_limits<float>::infinity();
	old = SanitizePreferences(old);
	Require(old.neuralSecondPass.inputScale == 1 && old.neuralSecondPass.preset == 0 && old.neuralSecondPass.tuning.intensity == 1, "invalid custom settings sanitized");
	std::puts("PASS: legacy defaults, independent INI persistence, relink preservation, sanitization and history/UI policy");
}
