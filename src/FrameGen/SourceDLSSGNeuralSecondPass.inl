// Included by SourceDLSSGNeuralRendering.cpp inside its namespace. Passes 2 to 5
// each evaluate once per active source frame with their own feature history.
// No CPU wait is introduced here.
bool NeuralPass::RecordExtra(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned pass,
	const NeuralOptions& options, const NeuralRendering::FeatureSession::EvaluationInput& first,
	ID3D12Resource* source, const ResolveConstants& originalConstants, ID3D12Resource* motion, ID3D12Resource* depth,
	ID3D12Resource* ui, NeuralRendering::FeatureSession::EvaluationInput& evaluation)
{
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
	constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	auto& extra = extra_[pass - 1];
	auto constants = originalConstants;
	auto dispatch = [&](unsigned step, ResolveKernel kernel, ID3D12Resource* a, ID3D12Resource* b,
		ID3D12Resource* original, ID3D12Resource* output, ID3D12Resource* second = nullptr) {
		const auto hr = resolve_.Record(device, list, slot, NeuralResolveKernels::ExtraPassStage(pass, step), kernel,
			constants, a, b, original, output, second);
		if (FAILED(hr)) { status_ = std::format("NR pass {} preparation failed 0x{:08X}", pass + 1, static_cast<UINT>(hr)); }
		return SUCCEEDED(hr);
	};
	// Pass 2 reads Pass 1's output, still writable from its evaluation. Later
	// passes read an earlier FinishExtra's result, already COMMON.
	if (pass == 1) { Transition(list, source, write, common); }
	// The guides and their scales are Pass 1's; only colour comes from the previous pass.
	evaluation = first;
	evaluation.color = source;
	evaluation.output = extra.output.Get();
	evaluation.tuning = options.EffectiveExtra(pass - 1).tuning;
	evaluation.controlMask = extra.controlMask;
	if (evaluation.controlMask) {
		// RGB already carries this pass's absolute intensity/tone/structure.
		evaluation.tuning.useAutoSkinMask = false;
		evaluation.tuning.intensity = evaluation.tuning.localToneStrength = evaluation.tuning.localStructureStrength = 1;
	}
	evaluation.reset = first.reset || extra.historyInvalid || extra.feature.EvaluationsRecorded() == 0;
	if (extra.input) {
		// Already encoded/warped model colour: resize it without encoding or
		// applying the peripheral mapping a second time.
		constants.sourceWidth = static_cast<UINT>(source->GetDesc().Width);
		constants.sourceHeight = source->GetDesc().Height;
		constants.targetWidth = static_cast<UINT>(evaluation.output->GetDesc().Width);
		constants.targetHeight = evaluation.output->GetDesc().Height;
		constants.peripheral = 0;
		if (!dispatch(0, ResolveKernel::ResizeColor, source, nullptr, nullptr, extra.input.Get())) { return false; }
		evaluation.color = extra.input.Get();
		if (extra.motion) {
			// Repack the original guides, including motion endpoints, at this pass's
			// own model extent. Reusing Pass 1's packed vectors changes their units.
			constants = originalConstants;
			constants.targetWidth = constants.workWidth = static_cast<UINT>(evaluation.output->GetDesc().Width);
			constants.targetHeight = constants.workHeight = evaluation.output->GetDesc().Height;
			if (!dispatch(1, ResolveKernel::PackGuides, depth, motion, nullptr, extra.depth.Get(), extra.motion.Get())) { return false; }
			evaluation.depth = extra.depth.Get(); evaluation.motionVectors = extra.motion.Get();
			for (auto* r : { evaluation.depth, evaluation.motionVectors }) { Transition(list, r, common, read); }
			if (ui) {
				if (!dispatch(2, ResolveKernel::Downsample, ui, nullptr, nullptr, extra.ui.Get())) { return false; }
				evaluation.ui = extra.ui.Get(); Transition(list, evaluation.ui, common, read);
			}
		} else {
			const auto ratio = float(evaluation.output->GetDesc().Width) / originalConstants.sourceWidth;
			evaluation.motionVectorScaleX = originalConstants.motionScaleX * ratio;
			evaluation.motionVectorScaleY = originalConstants.motionScaleY * ratio;
		}
	}
	if (NeuralRendering::UsesReconstructionContract(extra.feature.Build()) && evaluation.tuning.uiCorrection &&
		FAILED(Interop::RecordCopy(list, evaluation.color, evaluation.output))) {
		status_ = std::format("NR pass {} background copy rejected", pass + 1); return false;
	}
	Barriers{list}.Add(evaluation.color, common, read).Add(evaluation.output, common, write).Flush();
	evaluation.backbuffer = NeuralRendering::UsesReconstructionContract(extra.feature.Build()) ? evaluation.output : first.backbuffer;
	if (!extra.feature.RecordEvaluation(evaluation)) { status_ = std::format("NR pass {}: {}", pass + 1, extra.feature.Status()); return false; }
	extra.historyInvalid = false;
	return true;
}

// The last pass's restoration/copy stays outside the existing inference timing boundary.
bool NeuralPass::FinishExtra(ID3D12Device* device, ID3D12GraphicsCommandList* list, std::size_t slot, unsigned pass,
	const NeuralRendering::FeatureSession::EvaluationInput& first, ID3D12Resource* source,
	const NeuralRendering::FeatureSession::EvaluationInput& evaluation, const ResolveConstants& originalConstants,
	ID3D12Resource** directResult)
{
	constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	constexpr auto common = D3D12_RESOURCE_STATE_COMMON;
	constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	const auto& extra = extra_[pass - 1];
	auto constants = originalConstants;
	Barriers barriers{list};
	barriers.Add(evaluation.color, read, common).Add(evaluation.output, write, common);
	if (extra.motion) {
		for (auto* r : { evaluation.depth, evaluation.motionVectors, evaluation.ui }) { barriers.Add(r, read, common); }
	}
	barriers.Flush();
	auto* result = evaluation.output;
	if (extra.input) {
		// Transfer only this pass's RGB change back to Pass 1's grid. This preserves
		// the earlier passes' detail/alpha even when this model operates at 25%.
		constants.sourceWidth = static_cast<UINT>(evaluation.output->GetDesc().Width);
		constants.sourceHeight = evaluation.output->GetDesc().Height;
		constants.targetWidth = static_cast<UINT>(source->GetDesc().Width);
		constants.targetHeight = source->GetDesc().Height;
		constants.peripheral = 0;
		const auto hr = resolve_.Record(device, list, slot, NeuralResolveKernels::ExtraPassStage(pass, 3),
			ResolveKernel::RestoreSecond, constants, evaluation.color, evaluation.output, source, extra.restored.Get());
		if (FAILED(hr)) { status_ = std::format("NR pass {} preparation failed 0x{:08X}", pass + 1, static_cast<UINT>(hr)); return false; }
		result = extra.restored.Get();
	}
	if (directResult) {
		// The next pass or the resolve reads the result in place. Each feature keeps
		// its own history, and corrected_ remains the stable composition endpoint.
		*directResult = result;
		return true;
	}
	// Auto has no resolve, so corrected_ itself must receive the result.
	if (FAILED(Interop::RecordCopy(list, result, first.output))) { status_ = std::format("NR pass {} result copy rejected", pass + 1); return false; }
	Transition(list, first.output, common, write);
	return true;
}
