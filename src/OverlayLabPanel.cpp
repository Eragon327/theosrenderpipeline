#include <PCH.h>
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "DLSSBackend.h"
#include "RenderPipeline.h"
#include "FrameGen/NvidiaHost.h"
#include "FrameGen/SourceDLSSGBackend.h"
#include "FrameGen/FGInputDebugView.h"
#include "FrameGen/FGMotionRepair.h"

using namespace TheosRenderPipeline::Overlay;

// Lab mode details that read or change renderer internals directly. They stay
// out of the shared menu code; the offline preview draws a note instead.

void OverlayUI::DrawLabImageDetails(const FrameView& view)
{
    auto* upscaler = RenderPipeline::GetSingleton();
    auto* backend = DLSSBackend::GetSingleton();
    const bool cs = view.communityShaders;
    ImGui::Separator();
    ImGui::Text("Host Present calls: %.1f FPS | %s: %s", presentedFps, view.outputLabel, view.outputText.c_str());
    ImGui::Text("Active path: %s%s", view.activeUpscaleStage, cs || upscaler->IsEnabled() ? "" : " (inactive)");
    ImGui::Text("Render %d x %d -> Native %d x %d (%.1f%%)", upscaler->mRenderSizeX, upscaler->mRenderSizeY,
                view.nativeWidth, view.nativeHeight,
                view.nativeWidth > 0
                    ? static_cast<float>(upscaler->mRenderSizeX) / static_cast<float>(view.nativeWidth) * 100.0f
                    : 100.0f);
    if (!cs)
    {
        ImGui::Text("Jitter: (%.4f, %.4f) | phases: %d", upscaler->mJitterOffsets[0], upscaler->mJitterOffsets[1],
                    upscaler->JitterPhaseCount());
        ImGui::Text("Mip LOD bias: %.3f", upscaler->mMipLodBias);
        ImGui::Text("NGX evals ok/failed: %llu / %llu | last 0x%08X",
                    static_cast<unsigned long long>(backend->EvalSuccessCount()),
                    static_cast<unsigned long long>(backend->EvalFailCount()), backend->LastEvalResult());
        ImGui::Text("Feature: %s | formats in/out: %d / %d | Linear HDR input: %s",
                    backend->HasFeature() ? "created" : "none", backend->InputFormat(), backend->OutputFormat(),
                    backend->IsHDRInput() ? "yes" : "no");
    }
    if (upscaler->mGraphicsState)
    {
        auto& runtimeData = upscaler->mGraphicsState->GetRuntimeData();
        ImGui::Text("Engine resolution ratio at present: %.3f x %.3f", runtimeData.dynamicResolutionWidthRatio,
                    runtimeData.dynamicResolutionHeightRatio);
    }

    if (!cs)
    {
        auto* performance = PerformanceTuning::GetSingleton();
        for (auto item : {PerformanceTuning::Optimization::kDirectRCASOutput,
                          PerformanceTuning::Optimization::kDirectDLSSOutput})
        {
            const auto& route = performance->GetRouteStatus(item);
            ImGui::SeparatorText(item == PerformanceTuning::Optimization::kDirectRCASOutput ? "RCAS output"
                                                                                            : "DLSS output");
            ImGui::TextWrapped("%s: %s",
                               route.sessionRejected   ? "Fallback latched"
                               : route.activeLastFrame ? "Active"
                               : route.requested       ? "Armed"
                                                       : "Off",
                               !route.activeLastFrame && route.reason == "active" ? "not used this frame"
                                                                                : route.reason.c_str());
            ImGui::TextWrapped("Frames %llu | fallbacks %llu", static_cast<unsigned long long>(route.activeFrames),
                               static_cast<unsigned long long>(route.fallbackCount));
        }
    }
}

void OverlayUI::DrawLabXeFGControls()
{
    auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    // Session-only diagnostics: separate generated-frame artifacts from upscaler ones.
    bool onlyGenerated = sourceBackend.XeFGOnlyGenerated(), tagGenerated = sourceBackend.XeFGTagGenerated();
    bool changed = ImGui::Checkbox("Show only generated frames (Lab)", &onlyGenerated);
    changed |= ImGui::Checkbox("Tag generated frames (Lab)", &tagGenerated);
    if (changed)
        sourceBackend.ConfigureXeFGDebugView(onlyGenerated, tagGenerated);
    DrawSettingsHelp("Intel debug views, not saved. Only generated frames hides every real frame; "
                     "tagging marks generated frames with corner boxes.");
    bool gpuInputWait = sourceBackend.XeFGGpuInputWait();
    if (ImGui::Checkbox("Input reuse: GPU fence wait (Lab)", &gpuInputWait))
        sourceBackend.ConfigureXeFGGpuInputWait(gpuInputWait);
    DrawSettingsHelp("A/B for the post-Present wait, not saved. Off drains every host lane on the CPU; "
                     "on queues a GPU wait for Intel's input copies. The log reports both waits every 600 frames. "
                     "With heavy NR, on can make rendered frames alternate long and short.");
}

void OverlayUI::DrawLabGenerationControls(const FrameView& view)
{
    auto& sourceBackend = TheosRenderPipeline::SourceDLSSG::Backend::Get();
    if (view.provider != TheosRenderPipeline::FrameGenerationProvider::XeFG)
    {
        // Session-only diagnostic: an artifact only in generated frames comes from generation.
        bool onlyGenerated = sourceBackend.XeFGOnlyGenerated();
        if (ImGui::Checkbox("Show only generated frames (Lab)", &onlyGenerated))
            sourceBackend.ConfigureXeFGDebugView(onlyGenerated, sourceBackend.XeFGTagGenerated());
        DrawSettingsHelp("NVIDIA debug view, not saved. Hides every rendered frame, so an artifact still visible here "
                         "comes from frame generation; one that disappears comes from rendering, the upscaler or NR.");
        bool overlap = sourceBackend.NvidiaOverlapNextFrame();
        if (ImGui::Checkbox("Overlap the next frame with generation (Lab)", &overlap))
            sourceBackend.ConfigureNvidiaOverlapNextFrame(overlap);
        DrawSettingsHelp("A/B, not saved. Off: the game's next frame waits on the GPU until frame generation has read this "
                         "frame's inputs. On: it waits only where TRP first writes them, so the world can render meanwhile. "
                         "Compare FPS and the frame generation time.");
    }
    auto& repair = TheosRenderPipeline::FGMotionRepair::Get();
    bool repairMotion = repair.Enabled();
    if (ImGui::Checkbox("Repair missing motion (Lab)", &repairMotion))
        repair.Enable(repairMotion);
    DrawSettingsHelp("On by default; turning it off lasts until the game restarts. Underwater, Skyrim writes no real motion, "
                     "and generated frames show an offset copy. When nearly the whole frame disagrees with the camera, "
                     "NR and generation get the camera's motion instead.");
    auto& inputs = TheosRenderPipeline::FGInputDebugView::Get();
    bool showInputs = inputs.Enabled();
    if (ImGui::Checkbox("Show frame generation inputs (Lab)", &showInputs))
        inputs.Enable(showInputs);
    DrawSettingsHelp("Not saved. Shows what generation receives, after any repair. Left: motion vectors (grey = none). Middle: depth (dark = near). "
                     "Right: where the motion vectors disagree with the camera's own motion at that depth "
                     "(black = agree, red = 8+ pixels off). Static scenery should stay black while the camera moves.");
    if (showInputs)
    {
        if (auto* strip = inputs.Strip())
        {
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = width * float(inputs.PanelHeight()) / float(inputs.PanelWidth() * 3);
            ImGui::Image(reinterpret_cast<ImTextureID>(strip), ImVec2(width, height));
        }
        else
            ImGui::TextWrapped("Inputs: %s", inputs.Status());
    }
}

void OverlayUI::DrawLabGenerationDetails(const FrameView& view)
{
    auto* nvidiaHost = NvidiaHost::GetSingleton();
    const auto& sourceState = view.session;
    const auto& unlock = view.mfg;
    const bool intel = view.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
    const bool editIntel = settingsDraft.sourceDLSSG.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
    const bool nvidiaReported = !intel && sourceState.stateQueries > 0;
    const bool supportsDynamic = !editIntel && nvidiaReported && TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                     unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                     sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                 sourceState.state.numFramesToGenerateMax > 1;
    ImGui::Text("Evaluations: %llu", static_cast<unsigned long long>(nvidiaHost->EvaluationCount()));
    ImGui::Text("Host Presents: %llu | failures %llu | last 0x%08X",
                static_cast<unsigned long long>(nvidiaHost->PresentCount()),
                static_cast<unsigned long long>(nvidiaHost->FailedPresentCount()),
                static_cast<unsigned int>(nvidiaHost->LastPresentResult()));
    if (!intel && nvidiaHost->RuntimeStateObservationCount() > 0)
    {
        ImGui::Text("Last query: %u outputs | max generated %u | min dimension %u",
                    nvidiaHost->RuntimeFramesActuallyPresented(), nvidiaHost->RuntimeMaxGeneratedFrames(),
                    nvidiaHost->RuntimeMinWidthOrHeight());
        ImGui::Text("DLSS-G status: %u | observations: %llu", nvidiaHost->RuntimeDLSSGStatus(),
                    static_cast<unsigned long long>(nvidiaHost->RuntimeStateObservationCount()));
    }
    else if (!intel)
    {
        ImGui::TextDisabled("DLSS-G state: waiting");
    }
    if (intel)
    {
        const auto& state = view.xefg;
        ImGui::Text("XeFG frame: %u | result: %d | warnings: %llu", state.frameId, state.interpolationResult,
                    static_cast<unsigned long long>(state.warnings));
        ImGui::Text("XeLL output interval: %u us", state.frameLimitUs);
    }
    ImGui::TextWrapped("%s", nvidiaHost->Status().c_str());
    if (view.sourceDLSSGActive && !intel)
    {
        ImGui::Text("Configured multiplier: x%u", sourceState.options.numFramesToGenerate + 1);
        ImGui::Text("Output limit submitted interval: %u us", sourceState.frameLimitSubmittedUs);
        ImGui::TextWrapped("MFG: %s", unlock.status);
    }

    ImGui::Text("Dynamic multiplier: %s", supportsDynamic ? "supported" : "unavailable");
}

void OverlayUI::DrawLabMenuDiagnostics()
{
    auto* upscaler = RenderPipeline::GetSingleton();
    ImGui::Spacing();
    ImGui::TextColored(kRust, "These controls deliberately break or instrument the normal render path.");
    if (ImGui::Checkbox("Log menu/Console metrics (debug)", &upscaler->mLogMenuMetrics))
    {
        upscaler->mConsoleDiagnosticsActive.store(upscaler->mLogMenuMetrics &&
                                                      upscaler->mConsoleOpen.load(std::memory_order_relaxed),
                                                  std::memory_order_relaxed);
    }
    ImGui::Spacing();
    ImGui::SeparatorText("MAGIC PREVIEW DRAW ISOLATION");
    static const char* drawIsolationModes[] = {"Normal",      "Only draw 1", "Only draw 2",
                                               "Only draw 3", "Only draw 4", "Hide draw 1",
                                               "Hide draw 2", "Hide draw 3", "Hide draw 4"};
    if (ImGui::Combo("Inventory3D draw view", &upscaler->mInventory3DDrawIsolationMode, drawIsolationModes,
                     static_cast<int>(std::size(drawIsolationModes))))
    {
        upscaler->mInventory3DLastObservedDraws.store(0, std::memory_order_relaxed);
        upscaler->mInventory3DLastSkippedDraws.store(0, std::memory_order_relaxed);
        logger::info("[Inventory3DDrawIsolation] mode {} ({})", upscaler->mInventory3DDrawIsolationMode,
                     drawIsolationModes[upscaler->mInventory3DDrawIsolationMode]);
    }
    ImGui::TextDisabled("Runtime only. Last preview frame: %u draws observed, %u skipped.",
                        upscaler->mInventory3DLastObservedDraws.load(std::memory_order_relaxed),
                        upscaler->mInventory3DLastSkippedDraws.load(std::memory_order_relaxed));
}
