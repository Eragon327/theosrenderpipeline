#include <PCH.h>
#include "SourceDLSSGBackend.h"
#include "NeuralRenderingRuntimeIdentity.h"
#include "PerformanceTuning.h"
#include "SourceDLSSGSettings.h"
#include "../PluginPaths.h"
#include <algorithm>
#include <chrono>
#include <format>

namespace TheosRenderPipeline::SourceDLSSG
{
	void Backend::ConfigureNeuralRendering(NeuralOptions a_options)
	{
		const bool requested = a_options.enabled;
		a_options = SanitizeNeuralOptions(std::move(a_options));
		if (requested && !a_options.enabled) {
			logger::warn("[SourceDLSSG NR] optional runtime not found at {}; NR disabled, standard DLSS continues",
				a_options.runtimePath.string());
		}
		std::scoped_lock lock(neuralMutex_);
		ApplyNeuralFallback(a_options);
		if (a_options.runtimePath != neuralOptions_.runtimePath || (!neuralOptions_.enabled && a_options.enabled)) {
			neuralAvailability_.Reset();
		}
		neuralOptions_ = std::move(a_options);
	}
	const char* Backend::NeuralUnavailableReason(const NeuralOptions& options)
	{
		std::scoped_lock lock(neuralMutex_);
		if (!NvidiaAdapter() && options.enabled) { return "Neural Rendering requires an NVIDIA RTX GPU; XeSS and XeFG remain available"; }
		const auto retainedBuild = neuralPass_ ? neuralPass_->RetainedRuntimeBuild(options.runtimePath) :
			NeuralRendering::RuntimeBuild::Unknown;
		const auto reason = neuralAvailability_.UnavailableReason(options, [](const std::filesystem::path& path) {
			const auto identity = NeuralRenderingRuntimeIdentity::VerifySource(path, true);
			return identity.matched ? NeuralRendering::MatchRuntime(identity.size, identity.sha256, identity.version, true) :
				NeuralRendering::RuntimeBuild::Unknown;
		}, retainedBuild);
		if (reason && reason != neuralReportedUnavailable_) { logger::warn("[SourceDLSSG NR] {}", reason); }
		neuralReportedUnavailable_ = reason;
		return reason;
	}
	NeuralOptions Backend::NeuralConfiguration() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralOptions_;
	}
	NeuralSnapshot Backend::NeuralState() const
	{
		std::scoped_lock lock(neuralMutex_); return neuralSnapshot_;
	}
    bool Backend::RecreateNeuralIfNeeded(const NeuralOptions& options)
    {
        if (!neuralPass_ || !neuralPass_->NeedsRecreation(options, motion_.desc.Width, motion_.desc.Height)) { return true; }
        // Private outputs are copied into stable shared textures. Only our
        // queues read the feature, and no command list may be open here.
        if (!Check(interop_.Drain(), "retire NR before placement/reconstruction change")) { return false; }
        neuralPass_->RetireTelemetry();
        {
            std::scoped_lock lock(neuralMutex_);
            neuralSnapshot_.telemetry = neuralPass_->Telemetry();
        }
        neuralPass_.reset();
        logger::info("[SourceDLSSG NR] retired feature for placement={} passes={} preset={} inputScale={} resolve={} HDR={} producerColor={} peripheral={} fusion={}",
            options.beforeUpscaling ? "before DLSS" : "after DLSS", options.passes, options.reconstruction.preset,
            options.reconstruction.inputScale, static_cast<unsigned>(options.reconstruction.method), options.reconstruction.colorIsHDR,
            options.reconstruction.producerColor, options.reconstruction.peripheralCompression, options.reconstruction.fusedPreparation);
        if (options.passes == 2) {
            const auto second = options.EffectiveSecond();
            logger::info("[SourceDLSSG NR] pass2 linked={} preset={} inputScale={} intensity={}",
                options.secondPass.linked, second.preset, second.inputScale, second.tuning.intensity);
        }
        return true;
    }

    void Backend::ApplyNeuralFallback(NeuralOptions& options) const
    {
        auto failed = [&](const std::filesystem::path& path) {
            return std::any_of(neuralFailedRuntimes_.begin(), neuralFailedRuntimes_.end(),
                [&](const std::filesystem::path& entry) { return PluginPaths::EqualPath(entry, path); });
        };
        if (!options.enabled || !failed(options.runtimePath)) { return; }
        const auto fallback = PluginPaths::Directory() / std::filesystem::u8path(kDefaultNeuralRuntime);
        std::error_code error;
        if (!failed(fallback) && std::filesystem::is_regular_file(fallback, error)) { options.runtimePath = fallback; }
        else { options.enabled = false; }
    }

    void Backend::RecordNeuralGuard(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output)
    {
        static const char* reported{};
        neuralGuard_.Record(device12_.Get(), list, input, output);
        if (const auto* skipped = neuralGuard_.Skipped(); skipped && skipped != reported) {
            reported = skipped;
            logger::warn("[SourceDLSSG NR] output guard skipped a sample: {} (input format={} output format={})", skipped,
                static_cast<int>(neuralGuard_.InputFormat()), static_cast<int>(neuralGuard_.OutputFormat()));
        }
    }

    void Backend::SubmitNeuralGuard() { neuralGuard_.Submitted(queue_.Get()); }

    void Backend::PollNeuralGuard()
    {
        PatchStats input, output;
        if (!neuralGuard_.Poll(input, output)) { return; }
        std::scoped_lock lock(neuralMutex_);
        // Samples taken before a runtime change say nothing about the new one.
        static std::uint32_t logged{};
        if (!PluginPaths::EqualPath(neuralGuardRuntime_, neuralOptions_.runtimePath)) {
            neuralGuardRuntime_ = neuralOptions_.runtimePath;
            neuralBlack_.Reset();
            logged = 0;
        }
        // The first samples for each runtime, so a test shows what the guard saw.
        if (++logged <= 6) {
            logger::info("[SourceDLSSG NR] output guard sample {} runtime={} input lit {}/{} dark {} (format {}), output lit {}/{} dark {} (format {})",
                logged, neuralOptions_.runtimePath.filename().string(), input.lit, input.texels, input.dark,
                static_cast<int>(neuralGuard_.InputFormat()), output.lit, output.texels, output.dark, static_cast<int>(neuralGuard_.OutputFormat()));
        }
        if (!neuralOptions_.enabled || !neuralBlack_.Add(input, output)) { return; }
        neuralBlack_.Reset();
        const auto failedPath = neuralOptions_.runtimePath;
        neuralFailedRuntimes_.push_back(failedPath);
        ApplyNeuralFallback(neuralOptions_);
        neuralAvailability_.Reset();
        const auto name = failedPath.filename().string();
        neuralSnapshot_.fallback = neuralOptions_.enabled ?
            std::format("NR produced a black image with {}, so NVIDIA's {} is used for this session. That runtime may not support your GPU.",
                name, neuralOptions_.runtimePath.filename().string()) :
            std::format("NR produced a black image with {}, so NR is off for this session. That runtime may not support your GPU.", name);
        logger::warn("[SourceDLSSG NR] output guard: {} (input lit {}/{}, output dark {}/{})",
            neuralSnapshot_.fallback, input.lit, input.texels, output.dark, output.texels);
    }

    bool Backend::FailNeuralRecording()
    {
        std::scoped_lock lock(neuralMutex_);
        neuralSnapshot_.failed = true;
        neuralSnapshot_.active = false;
        neuralSnapshot_.status = neuralPass_->Status();
        logger::error("[SourceDLSSG NR] stopped before frame submission: {}", neuralSnapshot_.status);
        // The frame list is unsubmitted; separate feature-creation work may
        // already be queued. Its session retains that context on failed waits.
        return Check(E_FAIL, "source NR pass; relaunch required");
    }

    bool Backend::EvaluateNeuralBeforeUpscaling(const NeuralOptions& options, const sl::Constants* camera,
        bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        FrameExtent renderExtent, bool& reset)
    {
        return EvaluateNeuralWorld(options, camera, eligible, color, motion, depth, renderExtent, renderExtent, reset);
    }

    bool Backend::EvaluateNeuralWorld(const NeuralOptions& options, const sl::Constants* camera,
        bool eligible, ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* depth,
        FrameExtent renderExtent, FrameExtent colorExtent, bool& reset)
    {
        if (!Ready()) { return false; }
        frameNeuralOptions_ = options;
        // Preserve the saved after-DLSS preference while enforcing a world-only
        // input contract for the early stage. There are no UI pixels to correct.
        if (frameNeuralOptions_.WorldOnly()) { frameNeuralOptions_.tuning.uiCorrection = false; }
        neuralFrameBegun_ = true;
        neuralEvaluatedEarly_ = false;
        neuralEligible_ = eligible && !TransitionBlocked() && (!options.WorldOnly() || camera) &&
            !NeuralUnavailableReason(frameNeuralOptions_);
        frameNeuralReset_ = neuralHistory_.ResetFor(frameNeuralOptions_, neuralEligible_,
            reset || (camera && camera->reset == sl::eTrue));
        reset |= frameNeuralReset_;
        if (!options.enabled || !options.WorldOnly() || !neuralEligible_) { return true; }

        auto* timing = PerformanceTuning::GetSingleton();
        const bool measure = timing->TimingEnabled();
        const auto cpuBegin = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // One D3D11 clock domain spans input copies, the D3D12 dependency and
        // the return copy. This is elapsed GPU time, including scheduling gaps.
        ScopedD3D11PerformanceStage gpuTimer{context11_.Get(), PerformanceTuning::D3D11Stage::kNeuralEarlyRoundTrip};
        AllocatorWaitTiming allocatorWait{};
        if (!renderExtent.width || !renderExtent.height ||
            !EnsureGuide(motion, motion_, DXGI_FORMAT_UNKNOWN, renderExtent) ||
            !EnsureGuide(depth, depth_, DXGI_FORMAT_R32_FLOAT, renderExtent) ||
            !EnsureGuide(color, earlyNeuralColor_, DXGI_FORMAT_UNKNOWN, colorExtent) ||
            !RecreateNeuralIfNeeded(frameNeuralOptions_)) { return false; }
        if (!CopyDepth(depth) || !Check(interop_.CopyInputRegion(motion, motion_, renderExtent), "early NR motion copy") ||
            !Check(interop_.CopyInputRegion(color, earlyNeuralColor_, colorExtent), "early NR world copy") ||
            !Check(interop_.SignalD3D11(Work::Upscaling), "early NR inputs ready")) { return false; }
        ID3D12GraphicsCommandList* list = nullptr;
        if (!Check(interop_.Begin(Work::Upscaling, &list, measure ? &allocatorWait : nullptr), "begin NR before DLSS")) { return false; }
        if (!neuralPass_) { neuralPass_ = std::make_unique<NeuralPass>(); }
        if (!neuralPass_->Record(device12_.Get(), list, interop_.CurrentSlot(Work::Upscaling), frameNeuralOptions_,
            frameNeuralReset_, camera->depthInverted == sl::eTrue,
            camera->mvecScale.x * motion_.desc.Width, camera->mvecScale.y * motion_.desc.Height,
            motion_.texture12.Get(), depth_.texture12.Get(), nullptr, earlyNeuralColor_.texture12.Get(), nullptr,
            neuralTimestampFrequency_)) { return FailNeuralRecording(); }
        RecordNeuralGuard(list, earlyNeuralColor_.texture12.Get(), neuralPass_->Corrected());
        if (!Check(Interop::RecordCopy(list, neuralPass_->Corrected(), earlyNeuralColor_.texture12.Get()), "early NR result copy") ||
            !Check(interop_.Submit(Work::Upscaling), "submit NR before DLSS") ||
            !Check(interop_.WaitD3D11(Work::Upscaling), "DLSS waits for NR")) { return false; }
        SubmitNeuralGuard();
        // This copy and the subsequent NGX D3D11 call follow the GPU wait on
        // the same immediate context. No CPU-wide flush/drain on each frame.
        if (!Check(D3D11FrameCopy::Color(context11_.Get(), earlyNeuralColor_.texture11.Get(), color, colorExtent),
            "early NR return copy")) { return false; }
        neuralEvaluatedEarly_ = true;
        if (measure) {
            timing->RecordNeuralEarlyCPU(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - cpuBegin).count()), allocatorWait.nanoseconds, allocatorWait.waited);
        }
        return true;
    }
}
