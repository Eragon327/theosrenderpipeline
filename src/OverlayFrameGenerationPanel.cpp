#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlayFrameView.h"
#include "OverlayNumericInput.h"
#include "OverlaySettingRows.h"
#include "FrameGen/DlssgPreset.h"
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

void OverlayUI::DrawFrameGenerationPanel(float tabCardHeight, const FrameView& view)
{
    const auto& sourceDLSSGActive = view.sourceDLSSGActive;
    const auto& frameGenerationRuntimeActive = view.frameGenerationRuntimeActive;
    const auto& activeDisplayMultiplier = view.activeDisplayMultiplier;

    if (ImGui::BeginTabItem("Frame generation", nullptr,
                            requestedPage == SettingsPage::FrameGeneration ? ImGuiTabItemFlags_SetSelected
                                                                           : ImGuiTabItemFlags_None))
    {
        const auto& sourceState = view.session;
        const bool intel = view.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
        const bool editIntel = settingsDraft.sourceDLSSG.provider == TheosRenderPipeline::FrameGenerationProvider::XeFG;
        const auto& unlock = view.mfg;
        // NVIDIA's limits are known only once DLSS-G has run and answered a state
        // query; before that, leave the list open and let the runtime limit it.
        const bool nvidiaReported = !intel && sourceState.stateQueries > 0;
        // Standard offers Intel's official x2 only; the x3-x6 unlock is in Universal.
        const auto usableMaximum = editIntel ? (view.xefgUnlockAvailable ? TheosRenderPipeline::XeFGMaxGeneratedFrames : 1u) :
            !nvidiaReported ? 5u :
            TheosRenderPipeline::SourceDLSSG::MFGContract::Maximum(
            unlock.UsesCompatibilityUnlock(), unlock.Ready(), sourceState.state.numFramesToGenerateMax);
        const bool supportsDynamic = !editIntel && nvidiaReported && TheosRenderPipeline::SourceDLSSG::MFGContract::Dynamic(
                                         unlock.UsesCompatibilityUnlock(), unlock.Ready(),
                                         sourceState.state.bIsDynamicMFGSupported == sl::eTrue) &&
                                     sourceState.state.numFramesToGenerateMax > 1;
        if (!BeginSettingsColumns("generation", tabCardHeight, view))
        {
            ImGui::EndTabItem();
            return;
        }
        // ---- Status: what is running now.
        const char* intelPath = view.xefg.unlockReady && view.xefg.generatedFrames > 1
            ? "Experimental XeFG MFG" : "Intel XeFG x2";
        const char* path = intel                       ? intelPath
                           : unlock.UsesTuringUnlock() ? "Turing MFG (experimental)"
                           : unlock.UsesAmpereUnlock() ? "Ampere MFG (experimental)"
                           : unlock.UsesAdaUnlock()    ? "Ada MFG"
                                                       : "Native NVIDIA";
        DrawStatusLabel(frameGenerationRuntimeActive ? (intel ? "XeFG active" : "DLSS-G active") : "Frame generation inactive",
                        frameGenerationRuntimeActive ? UIHealth::kHealthy : UIHealth::kIdle);
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", path);
        const auto availableMaximum = intel ? view.xefg.maxGeneratedFrames : nvidiaReported ? usableMaximum : 0u;
        DrawSettingsValue("Multiplier", (availableMaximum > 1 ? std::format("x{} of x2 to x{}", activeDisplayMultiplier, availableMaximum + 1)
                                         : availableMaximum == 1 ? std::format("x{} (x2 only)", activeDisplayMultiplier)
                                                                 : std::format("x{} | availability waiting", activeDisplayMultiplier)).c_str());
        DrawSettingsValue("Display", std::format("{:.0f} Hz", view.refreshRate).c_str());
        if (view.vsyncObserved) {
            DrawSettingsValue("VSync", std::format("game {} -> presenter {}", view.vsyncIncoming ? "On" : "Off",
                                                   view.vsyncRequested ? "On" : "Off").c_str());
            DrawSettingsHelp("What the game asked for, and what the presenter requested for presentation.");
            if (view.vsyncFailed) ImGui::TextColored(kOchre, "Last presentation request failed.");
        }
        DrawSettingsValue("Latency", intel ? "XeLL (Intel)"
            : std::format("Reflex | {}", TheosRenderPipeline::SourceDLSSG::ReflexModeName(sourceState.reflexSubmitted)).c_str());
        DrawSettingsValue("Output cap", intel ? (view.xefg.frameLimitUs ?
            std::format("{:.1f} FPS", 1000000.0 / view.xefg.frameLimitUs).c_str() : "Off") :
                          sourceState.frameLimitSubmittedUs
                              ? std::format("{:.1f} FPS", 1000000.0 / sourceState.frameLimitSubmittedUs).c_str()
                              : "Off");
        DrawSettingsValue("UI composition",
            intel ? (!view.uiRecomposition ? "Interpolated with the scene" : view.xefg.uiTexture ? "UI layer + HUD-less" : "Extracted from frame")
            : !sourceState.uiRecompositionRequested ? "Interpolated with the scene"
            : !frameGenerationRuntimeActive ? "Recompose requested"
            : sourceState.options.enableUserInterfaceRecomposition == sl::eTrue ? "Recomposed by DLSS-G"
                                                                                  : "Waiting for HUD-less and UI layers");
        if (intel)
        {
            const auto& state = view.xefg;
            DrawSettingsValue("Outputs", std::format("{} | {} generated presents", state.framesPresented, state.generatedPresents).c_str());
            if (state.frameTimeMs > 0)
                DrawSettingsValue("Frame time sent", std::format("{:.2f} ms", state.frameTimeMs).c_str());
            if (view.xefgTensorRequested)
            {
                const auto& pacing = state.pacing;
                DrawSettingsValue("Tensor model", view.xefgTensorStatus.c_str());
                if (pacing.flags & TheosRenderPipeline::XeFGPacing::Installed)
                {
                    DrawSettingsValue("Driver metering", std::format("{} | {} accepted batches",
                        pacing.flags & TheosRenderPipeline::XeFGPacing::Unavailable ? "unavailable" :
                        pacing.flags & TheosRenderPipeline::XeFGPacing::Metered ? "active" : "idle", pacing.meteredCalls).c_str());
                    DrawSettingsHelp(view.xefgPacingStatus.c_str());
                }
            }
            if (showDeveloperControls)
                DrawLabXeFGControls();
            ImGui::TextDisabled("%s", view.xefgUnlockStatus.c_str());
        }
        if (!view.providerStatus.empty()) { ImGui::TextDisabled("%s", view.providerStatus.c_str()); }
        if (showDeveloperControls)
            DrawLabGenerationControls(view);
        if (!intel && unlock.UsesCompatibilityUnlock() && !unlock.Ready())
            ImGui::TextWrapped("%s", unlock.status);
        if (!intel && sourceState.stateQueryResult == sl::Result::eWarnOutOfVRAM)
            ImGui::TextColored(kOchre, "NVIDIA VRAM budget warning");
        if (view.warmupPresentsRemaining > 0)
            ImGui::Text("Warmup: %d frames", view.warmupPresentsRemaining);
        if (showDeveloperControls && ImGui::CollapsingHeader("Runtime details"))
            DrawLabGenerationDetails(view);
        DrawStageMeasurements(view, SettingsPage::FrameGeneration);
        NextSettingsColumn(tabCardHeight);

        // ---- Settings: one list for both providers. Rows that only one provider
        // uses stay visible and greyed, with the reason, for the other.
        namespace SD = TheosRenderPipeline::SourceDLSSG;
        auto& draft = settingsDraft.sourceDLSSG;
        auto& request = draft.generation;
        const auto& style = ImGui::GetStyle();
        const float label = LabelWidth({"Frame generation", "Output FPS limit", "Scene change", "Flip metering"});
        if (!sourceDLSSGActive)
        {
            ImGui::TextColored(kOchre, "NVIDIA host is unavailable. Check runtime status and restart Skyrim.");
        }
        ImGui::BeginDisabled(!sourceDLSSGActive);
        if (BeginSettingRows("fgShared", label))
        {
            SettingRow("Frame generation", nullptr, false, false, [&] {
                bool requested = view.frameGenerationRequested;
                if (ImGui::Checkbox("##on", &requested)) { RequestFrameGeneration(requested); }
                DrawSettingsHelp("Takes effect immediately. Save as default to keep this choice for the next launch.");
                ImGui::SameLine();
                const float mark = ImGui::CalcTextSize("!").x + style.ItemSpacing.x;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - mark);
                int provider = std::clamp(static_cast<int>(draft.provider), 0, 1);
                const char* providers[]{"NVIDIA DLSS-G", "Intel XeFG (experimental)"};
                bool changed = false;
                if (ImGui::BeginCombo("##provider", providers[provider]))
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        ImGui::BeginDisabled(i == 0 && !view.nvidiaAdapter);
                        if (ImGui::Selectable(providers[i], provider == i) && provider != i)
                        {
                            draft.provider = static_cast<TheosRenderPipeline::FrameGenerationProvider>(i);
                            changed = true;
                        }
                        ImGui::EndDisabled();
                        if (i == 0 && !view.nvidiaAdapter && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip("Needs an NVIDIA GPU.");
                    }
                    ImGui::EndCombo();
                }
                DrawSettingsHelp("Frame generation provider. NVIDIA DLSS-G requires NVIDIA hardware; XeFG uses XeLL latency "
                                 "reduction. On NVIDIA cards XeFG costs noticeably more GPU time than DLSS-G; it is meant for "
                                 "other GPUs.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kOchre, "!");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Apply switches provider after a completed world frame, with a pause of under a second. "
                                      "Upscaling and NR keep their settings.");
                }
                return changed;
            });

            // Multiplier: unavailable counts are greyed with the reason.
            auto& requestedCount = editIntel ? draft.xefg.generatedFrames : request.generatedFrames;
            const unsigned listedMaximum = editIntel ? TheosRenderPipeline::XeFGMaxGeneratedFrames : 5u;
            const auto limitReason = editIntel ? std::string(view.xefgUnlockAvailable ? "" : "x3 to x6 need the Universal edition.")
                                   : std::format("This NVIDIA runtime supports up to x{}.", usableMaximum + 1);
            const char* experimental = "Experimental: patches Intel's runtime in memory. The first Apply recreates the Intel "
                                       "presenter; later changes apply live.";
            const auto option = [&](unsigned count) {
                const bool usable = count <= usableMaximum && count <= listedMaximum;
                return Segment{nullptr, usable, !usable ? limitReason.c_str() : editIntel && count > 1 ? experimental : nullptr};
            };
            SettingRow("Multiplier", nullptr, false, false, [&] {
                const char* dynamicLabel = "Dynamic";
                const float dynamicWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize(dynamicLabel).x;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - dynamicWidth - style.ItemSpacing.x);
                int index = static_cast<int>(std::clamp(requestedCount, 1u, listedMaximum)) - 1;
                auto s2 = option(1), s3 = option(2), s4 = option(3), s5 = option(4), s6 = option(5);
                s2.label = "x2"; s3.label = "x3"; s4.label = "x4"; s5.label = "x5"; s6.label = "x6";
                bool changed = Segmented("multiplier", index, {s2, s3, s4, s5, s6});
                if (changed)
                {
                    requestedCount = static_cast<unsigned>(index + 1);
                    if (editIntel) { draft.xefg.experimentalMFG = requestedCount > 1; }
                }
                DrawSettingsHelp(!editIntel && !nvidiaReported ? "NVIDIA reports its supported multipliers once DLSS-G is running."
                                 : editIntel ? "x2 is Intel's official mode; x3 to x6 are experimental."
                                             : "Generated frames per rendered frame, plus one.");
                ImGui::SameLine();
                ImGui::BeginDisabled(editIntel || !supportsDynamic);
                changed |= ImGui::Checkbox(dynamicLabel, &request.dynamic);
                ImGui::EndDisabled();
                DrawSettingsHelp(editIntel ? "NVIDIA only." : !nvidiaReported ? "Available once DLSS-G is running."
                                 : !supportsDynamic ? "Not supported by this NVIDIA runtime."
                                                    : "NVIDIA chooses the multiplier to reach the target FPS.");
                return changed;
            });
            SettingRow("Target FPS", nullptr, false, editIntel || !request.dynamic, [&] {
                int target = static_cast<int>(request.dynamicTargetFPS);
                ImGui::SetNextItemWidth(Px(64.0f));
                // Keep intermediate digits while typing 120; Apply validates.
                const bool changed = ImGui::InputInt("##target", &target, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
                if (changed) { request.dynamicTargetFPS = static_cast<unsigned>(std::clamp(target, 0, 1000)); }
                DrawSettingsHelp("Dynamic multiplier target. 0 follows the display refresh rate; explicit targets must exceed "
                                 "60 FPS.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", request.dynamicTargetFPS ? "FPS" : "display refresh");
                return changed;
            });

            // HUD handling: XeFG uses A or B; Lite (C) is NVIDIA's.
            SettingRow("HUD", nullptr, false, false, [&] {
                auto& preset = draft.preset;
                int index = preset == SD::DlssgPreset::C ? 2 : preset == SD::DlssgPreset::B ? 1 : 0;
                const bool changed = Segmented("hud", index, {
                    {"Interpolate", true, "Preset A: the HUD is interpolated with the scene."},
                    {"Recompose", true, "Preset B: the scene and HUD are generated separately for less HUD ghosting in "
                                        "motion, at a small GPU and VRAM cost."},
                    {"Lite", !editIntel, editIntel ? "NVIDIA only; on XeFG it runs as Interpolate."
                                                   : "Preset C: NVIDIA's lighter Lite blending (DLSS-G 310.9.16 or newer), "
                                                     "without UI recomposition."}});
                if (changed) { preset = index == 2 ? SD::DlssgPreset::C : index == 1 ? SD::DlssgPreset::B : SD::DlssgPreset::A; }
                return changed;
            });
            SettingRow("Latency", nullptr, false, editIntel, [&] {
                if (editIntel)
                {
                    int none = -1;
                    Segmented("xell", none, {{"XeLL (Intel)", false,
                        "XeFG uses XeLL latency reduction. Your Reflex choice is kept for NVIDIA."}});
                    return false;
                }
                const sl::ReflexMode modes[]{sl::ReflexMode::eOff, sl::ReflexMode::eLowLatency, sl::ReflexMode::eLowLatencyWithBoost};
                int index = 0;
                for (int i = 0; i < 3; ++i) { if (draft.reflexMode == static_cast<int>(modes[i])) { index = i; } }
                const bool changed = Segmented("reflex", index, {
                    {"Off", true, "NVIDIA Reflex off."},
                    {"Low latency", true, "NVIDIA Reflex low latency."},
                    {"Boost", true, "NVIDIA Reflex low latency with boost: keeps GPU clocks up for lower latency."}});
                if (changed) { draft.reflexMode = static_cast<int>(modes[index]); }
                return changed;
            });
            SettingRow("VSync", nullptr, false, false, [&] {
                int index = std::clamp(static_cast<int>(draft.vsync), 0, 2);
                const bool changed = Segmented("vsync", index, {
                    {"Auto", true, "Turns VSync off for available XeFG driver metering and otherwise follows the game."},
                    {"Off", true, "Requests immediate presentation."},
                    {"On", true, "Requests VSync. Prevents XeFG driver metering."}});
                if (changed) { draft.vsync = static_cast<SD::VSyncMode>(index); }
                DrawSettingsHelp("Apply changes the request live; Save as default keeps it. Other mods' frame limits still "
                                 "apply. DLSS-G manages its final output timing.");
                return changed;
            });
            SettingRow("Output FPS limit", nullptr, false, false, [&] {
                ImGui::SetNextItemWidth(Px(64.0f));
                const bool changed = ImGui::InputInt("##limit", &draft.outputFPSLimit, 0, 0, ImGuiInputTextFlags_AutoSelectAll);
                draft.outputFPSLimit = std::clamp(draft.outputFPSLimit, 0, 1000);
                DrawSettingsHelp("The cap includes generated frames. Avoid stacking it with another limiter.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", draft.outputFPSLimit ? "FPS, including generated frames" : "no cap");
                return changed;
            });
            ImGui::EndTable();
        }

        // Notes about the shared rows: pending switches and runtime limits.
        if (!editIntel && intel && view.nvidiaSwitchBlocked)
            ImGui::TextColored(kOchre, "Switching to NVIDIA needs a restart this session: NR started before NVIDIA's "
                                       "runtime. Save as default, then restart Skyrim.");
        else if (view.requestedProvider != view.provider)
            ImGui::TextDisabled("Provider change pending; waiting for world inputs...");
        if (view.frameGenerationRequested != view.hostFrameGenerationEnabled)
            ImGui::TextDisabled("Waiting for the current GPU frame to retire...");
        if (!editIntel && request.dynamic && nvidiaReported && !supportsDynamic)
        {
            ImGui::TextColored(kOchre, "A saved dynamic request is unsupported by this NVIDIA runtime.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Use fixed multiplier")) { request.dynamic = false; }
        }
        if (!editIntel && nvidiaReported && sourceState.generationLimited)
            ImGui::TextColored(kOchre, "The runtime uses its supported count; your requested multiplier is kept.");
        if (!editIntel)
        {
            const auto& lite = view.liteBlending;
            if (SD::UsesLiteBlending(draft.preset) &&
                (lite.hook != SD::LiteBlending::HookState::Installed || !lite.runtimeSupports))
                ImGui::TextColored(kOchre, lite.hook == SD::LiteBlending::HookState::NotInstalled
                                               ? "Lite is checked when DLSS-G loads"
                                               : "This DLSS-G build has no Lite blending; it runs as Interpolate");
            if (lite.driverPreset)
                ImGui::TextColored(kOchre, "NVIDIA driver profile sets Preset %s; DLSS-G uses it instead",
                                   SD::DriverPresetName(lite.driverPreset).c_str());
        }
        else if (draft.preset == SD::DlssgPreset::C)
        {
            ImGui::TextDisabled("Lite is NVIDIA only; XeFG runs it as Interpolate.");
        }

        // XeFG's own settings, together at the bottom.
        ImGui::Spacing();
        if (BeginSettingRows("fgXeFG", label))
        {
            const char* xefgOnly = "XeFG only. Choose Intel XeFG as the provider to use it.";
            SettingRow("Scene change", nullptr, false, !editIntel, [&] {
                auto& sceneChange = draft.xefgSceneChangeThreshold;
                const bool atDefault = sceneChange == TheosRenderPipeline::XeFGDefaultSceneChangeThreshold;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x);
                bool changed = ImGui::SliderFloat("##sceneChange", &sceneChange, 0.0f, 1.0f, "%.2f");
                sceneChange = TheosRenderPipeline::SanitizeXeFGSceneChange(sceneChange);
                DrawSettingsHelp(!editIntel ? xefgOnly
                    : "How readily XeFG treats a sudden change, such as a camera cut or a fast turn, as a new scene and "
                      "shows the rendered frame instead of generating one. Higher avoids smeared transitions but generates "
                      "less in fast motion. Apply to compare live.");
                ImGui::SameLine();
                ImGui::BeginDisabled(atDefault);
                if (UndoButton("##sceneDefault"))
                {
                    sceneChange = TheosRenderPipeline::XeFGDefaultSceneChangeThreshold;
                    changed = true;
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip(atDefault ? "At Intel's default (0.70)." : "Was 0.70 (Intel's default). Click to reset.");
                return changed;
            });
            SettingRow("Frame time", nullptr, false, !editIntel, [&] {
                const bool changed = ImGui::Checkbox("##frameTime", &draft.xefgFrameTime);
                DrawSettingsHelp(!editIntel ? xefgOnly
                    : "Gives Intel's frame pacing the measured frame time. On AMD and NVIDIA cards Intel uses it to "
                      "sanity-check pacing. Apply to compare live.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("send to XeFG");
                return changed;
            });
            SettingRow("Tensor model", nullptr, false, !editIntel, [&] {
                ImGui::BeginDisabled(!view.xefgUnlockAvailable);
                const bool changed = ImGui::Checkbox("##tensor", &draft.xefgTensorCores);
                ImGui::EndDisabled();
                DrawSettingsHelp(!editIntel ? xefgOnly
                    : !view.xefgUnlockAvailable ? "Universal only."
                    : "Uses the private NVIDIA tensor backend when its runtime bundle is installed. Save as default and "
                      "restart Skyrim to change it.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                if (editIntel && draft.xefgTensorCores != view.xefgTensorRequested)
                    ImGui::TextColored(kOchre, "save and restart to change");
                else
                    ImGui::TextDisabled("experimental | restart");
                return changed;
            });
            const auto& pacing = view.xefg.pacing;
            const bool meteringReady = view.xefgTensorRequested && (pacing.flags & TheosRenderPipeline::XeFGPacing::Installed);
            const bool vsyncOn = draft.vsync == SD::VSyncMode::On;
            SettingRow("Flip metering", nullptr, false, !editIntel || !meteringReady || vsyncOn, [&] {
                bool meter = view.xefgFlipMetering;
                const bool changed = ImGui::Checkbox("##metering", &meter);
                if (changed) { SetXeFGFlipMetering(meter); }
                DrawSettingsHelp(!editIntel ? xefgOnly
                    : !meteringReady ? "Needs the XeFG tensor model running."
                    : vsyncOn ? "Unavailable while VSync is On."
                    : "NVIDIA flip metering: compares driver timing with software timing. Changes live and is not saved. "
                      "The driver must accept metering for it to become active.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("session only%s", meteringReady
                    ? (pacing.flags & TheosRenderPipeline::XeFGPacing::Unavailable ? " | driver unavailable"
                       : pacing.flags & TheosRenderPipeline::XeFGPacing::Metered ? " | driver active" : " | driver idle") : "");
                // Live and session-only: not part of the settings draft.
                return false;
            });
            ImGui::EndTable();
        }
        ImGui::EndDisabled();

        EndSettingsColumns();
        ImGui::EndTabItem();
    }
}
