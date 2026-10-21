#pragma once
#include <cstdint>

// Model rate: run NR's model on every frame, or on every other frame and carry
// its change to the frames between with motion vectors and depth. The approach
// follows cavy8's Alternate Frames for Community Shaders NR (Bottled Shaders
// PR #6): evaluate on alternate frames, reproject the previous result with
// motion, depth and material category, compose two frames of motion for the
// model's history, and evaluate whenever the chain is broken.
namespace TheosRenderPipeline::NeuralRendering
{
	enum class ModelRate : std::uint32_t { EveryFrame, EveryOtherFrame };
	inline ModelRate SanitizeModelRate(ModelRate value)
	{
		return value == ModelRate::EveryOtherFrame ? value : ModelRate::EveryFrame;
	}

	// One frame's facts, sampled by the NR pass before it records anything.
	struct ModelRateFrame
	{
		ModelRate rate{};
		// NR history restarts this frame (camera cut, settings, freeze released,
		// object controls switching, a new feature).
		bool reset{};
		// The stored change or guides cannot stand for this frame: a setting that
		// changes NR's output, the passes that run, a pass created this frame, a
		// gap since the previous frame.
		bool changed{};
		// Compare > Freeze frame: the held frame is evaluated every frame.
		bool frozen{};
	};

	struct ModelRateDecision
	{
		bool evaluate{ true };
		// The model's history is two frames old: give it this frame's motion
		// composed with the skipped frame's.
		bool composeMotion{};
		// Keep this frame's motion, depth and object group for the next frame.
		bool captureGuides{};
		// Keep this evaluation's change for the next frame.
		bool storeChange{};
		bool operator==(const ModelRateDecision&) const = default;
	};

	// A frame may skip the model only when the frame immediately before it
	// evaluated and stored its change, and nothing since has invalidated it.
	// It therefore never skips two frames in a row. Next is called once per
	// recorded frame; a failed recording ends the pass, so there is no rollback.
	class ModelRateSchedule
	{
	public:
		ModelRateDecision Next(const ModelRateFrame& frame)
		{
			ModelRateDecision decision;
			const bool alternate = SanitizeModelRate(frame.rate) == ModelRate::EveryOtherFrame && !frame.frozen;
			decision.evaluate = !(alternate && stored_ && !frame.reset && !frame.changed);
			// Composition also follows a switch back to every frame: the history
			// is still the frame before the skipped one. A reset has no history.
			decision.composeMotion = decision.evaluate && skipped_ && !frame.reset;
			decision.captureGuides = alternate || decision.composeMotion;
			decision.storeChange = decision.evaluate && alternate;
			stored_ = decision.storeChange;
			skipped_ = !decision.evaluate;
			return decision;
		}
		// Forget the stored frame, for example after a recording failure.
		void Invalidate() { stored_ = skipped_ = false; }
	private:
		bool stored_{};
		bool skipped_{};
	};
}
