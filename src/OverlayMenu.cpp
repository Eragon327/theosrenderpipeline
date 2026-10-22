#include <PCH.h>
#include "Localization.h"
#include "OverlayUI.h"
#include "OverlayFrameView.h"
#include "OverlayUIStyle.h"
#include "RenderPipeline.h"
#include "RendererSettingsAction.h"

#include <imgui_internal.h>

using namespace TheosRenderPipeline::Overlay;

// "Zoom - 133% +" at the right end of the title bar, so the size can be changed
// without finding Advanced. Clicking the percentage returns to automatic size.
void OverlayUI::DrawZoomButtons()
{
    auto* window = ImGui::GetCurrentWindow();
    const auto title = window->TitleBarRect();
    const auto& style = ImGui::GetStyle();
    const auto display = ImGui::GetIO().DisplaySize;
    const float current = UIScale();
    const float automatic = ResolveUIScale(0, display.x, display.y);
    const bool manual = layout.uiScale > 0;
    char percent[32];
    std::snprintf(percent, sizeof(percent), "%.0f%%###zoomReset", current * 100.0f);
    const float buttonWidth = ImGui::CalcTextSize("+").x + style.FramePadding.x * 2.0f;
    // Sized for the widest value so the controls do not shift as the zoom changes.
    const float percentWidth = ImGui::CalcTextSize("300%").x + style.FramePadding.x * 2.0f;
    const float total = ImGui::CalcTextSize(TR("Zoom")).x + percentWidth + buttonWidth * 2.0f + style.ItemSpacing.x * 3.0f;
    // Title-bar items must not extend the content region the window scrolls.
    const auto cursor = ImGui::GetCursorScreenPos();
    const auto maxPos = window->DC.CursorMaxPos;
    const auto idealMaxPos = window->DC.IdealMaxPos;
    ImGui::PushClipRect(title.Min, title.Max, false);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(title.Max.x - style.FramePadding.x - total,
                                     title.Min.y + (title.GetHeight() - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextColored(kMuted, TR("Zoom"));
    const auto step = [&](const char* label, int direction, const char* action) {
        ImGui::SameLine();
        const float next = StepUIScale(current, direction);
        // Disable a step that the size limits would leave unchanged.
        ImGui::BeginDisabled(std::abs(ResolveUIScale(next, display.x, display.y) - current) < 0.001f);
        if (ImGui::Button(label, ImVec2(buttonWidth, 0.0f)))
            layout.uiScale = next;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(TR("%s by 25%%. Save as default remembers the zoom."), action);
    };
    step("-##zoomOut", -1, TR("Zoom out"));
    ImGui::SameLine();
    // A flat readout; it highlights as a button only when there is a manual zoom to clear.
    const ImVec4 clear(0, 0, 0, 0);
    ImGui::PushStyleColor(ImGuiCol_Button, clear);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, manual ? style.Colors[ImGuiCol_ButtonHovered] : clear);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, manual ? style.Colors[ImGuiCol_ButtonActive] : clear);
    if (ImGui::Button(percent, ImVec2(percentWidth, 0.0f)) && manual)
        layout.uiScale = 0;
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (manual)
            ImGui::SetTooltip(TR("Click to return to automatic zoom (%.0f%% for this output)."), automatic * 100.0f);
        else
            ImGui::SetTooltip(TR("Automatic zoom from the output height; 1080p is 100%%."));
    }
    step("+##zoomIn", 1, TR("Zoom in"));
    ImGui::PopStyleVar();
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(cursor);
    window->DC.CursorMaxPos = maxPos;
    window->DC.IdealMaxPos = idealMaxPos;
}

void OverlayUI::BuildUI()
{
    const auto view = CaptureFrameView();
    const auto displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0 || displaySize.y <= 0)
        return;
    const float scale = UIScale();
    if (layoutPending || layoutDisplayWidth != displaySize.x || layoutDisplayHeight != displaySize.y)
    {
        const auto screen = FitLayout(layout, displaySize.x, displaySize.y, scale);
        ImGui::SetNextWindowSize(ImVec2(screen.width, screen.height), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(screen.x, screen.y), ImGuiCond_Always);
        layoutDisplayWidth = displaySize.x;
        layoutDisplayHeight = displaySize.y;
        layoutPending = false;
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2((std::min)(Px(MinWindowWidth), displaySize.x), (std::min)(Px(MinWindowHeight), displaySize.y)),
        displaySize);
    if (!ImGui::Begin(Plugin::DISPLAY_NAME.data(), nullptr, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    // Keep the remembered geometry in 1x units so it follows later scale changes.
    const auto windowPos = ImGui::GetWindowPos();
    const auto windowSize = ImGui::GetWindowSize();
    layout.x = windowPos.x / scale;
    layout.y = windowPos.y / scale;
    layout.width = windowSize.x / scale;
    layout.height = windowSize.y / scale;
    DrawZoomButtons();
    DrawPipelineSummary(view);
    const auto& layoutStyle = ImGui::GetStyle();
    float reservedActionHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() +
                                       ImGui::GetTextLineHeightWithSpacing() +
                                       layoutStyle.CellPadding.y * 2.0f + layoutStyle.ItemSpacing.y * 2.0f + 1.0f;
    // The action bar starts with the language row and, on failure, its warning.
    reservedActionHeight += ImGui::GetFrameHeightWithSpacing();
    if (!TheosRenderPipeline::Localization::LastError().empty()) {
        reservedActionHeight += ImGui::GetTextLineHeightWithSpacing();
    }
    if (actionMessageIsError && !actionMessage.empty()) {
        const float statusWidth = (std::max)(1.0f, ImGui::GetContentRegionAvail().x - Px(450.0f) - layoutStyle.CellPadding.x * 4.0f);
        reservedActionHeight += (std::max)(0.0f, ImGui::CalcTextSize(actionMessage.c_str(), nullptr, false, statusWidth).y - ImGui::GetFrameHeight());
    }
    const float tabCardHeight = (std::max)(Px(220.0f), ImGui::GetContentRegionAvail().y - reservedActionHeight);

    if (ImGui::BeginTabBar("##theosrenderpipelineTabs", ImGuiTabBarFlags_None))
    {
        DrawImagePanel(tabCardHeight, view);

#if !defined(TRP_NO_NEURAL_RENDERING)
        DrawNeuralRenderingPanel(tabCardHeight, view);
#endif

        DrawFrameGenerationPanel(tabCardHeight, view);
        DrawAdvancedPanel(tabCardHeight, view);
        ImGui::EndTabBar();
    }

    requestedPage = SettingsPage::None;
    DrawSettingsActions();

    ImGui::End();
}

void OverlayUI::DrawPipelineSummary(const FrameView& view)
{
#if !defined(TRP_NO_NEURAL_RENDERING)
    const auto neuralTooltip = view.sourceNeural.status + "\nClick to open Neural Rendering settings.";
#endif
    PipelineDiagram diagram{
        {{{TR("World"), view.renderDetail, TR("Game-rendered scene.\nClick to open Image settings."),
           SettingsPage::Image},
#if !defined(TRP_NO_NEURAL_RENDERING)
          {TR("Neural Rendering"), view.neuralDetail, neuralTooltip.c_str(), SettingsPage::NeuralRendering,
           !view.neuralEnabled},
#endif
          {view.upscaleTitle, view.upscaleDetail,
           view.communityShaders ? TR("Upscaling is controlled in the Community Shaders menu.\nClick to open Image status.") :
           TR("DLSS or XeSS reconstruction, or native-resolution DLAA.\nClick to open Image settings."), SettingsPage::Image},
          {TR("Frame generation"), view.generationTitle,
           TR("Adds generated frames between game-rendered frames.\nClick to open Frame generation settings."),
           SettingsPage::FrameGeneration, !view.frameGenerationRuntimeActive},
          {TR("Output"), view.nativeDetail,
           TR("Final output resolution.\nClick to open Image settings."), SettingsPage::Image}}},
        view.nativeDetail,
        view.nativeUIHealth == UIHealth::kHealthy,
        view.pipelineLabel,
        HealthColor(view.pipelineHealth)};
#if !defined(TRP_NO_NEURAL_RENDERING)
    if (!view.neuralBeforeUpscaling)
    {
        std::swap(diagram.stages[1], diagram.stages[2]);
    }
#endif
    // Only a click navigates; the preview requests its tab before the summary.
    if (const auto page = DrawPipelineDiagram(diagram); page != SettingsPage::None)
    {
        requestedPage = page;
    }
}

void OverlayUI::DrawSettingsActions()
{
    DrawLanguageControl();
    ImGui::Separator();
    const int stagedChanges = CountStagedChanges();
    if (ImGui::BeginTable("##actionBar", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##actionStatus", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, Px(450.0f));
        ImGui::TableNextColumn();
        const auto status = TheosRenderPipeline::SettingsStatus(stagedChanges, actionMessage, actionMessageIsError);
        using StatusKind = TheosRenderPipeline::SettingsStatusKind;
        ImGui::PushTextWrapPos(0);
        if (status.kind == StatusKind::Neutral) { ImGui::TextDisabled("%s", status.text.c_str()); }
        else { ImGui::TextColored(status.kind == StatusKind::Error ? kRust :
            status.kind == StatusKind::Pending ? kAmber : kSage, "%s", status.text.c_str()); }
        ImGui::PopTextWrapPos();
        ImGui::TableNextColumn();
        const bool capturingHotkey = hotkeys.IsCapturing();
        ImGui::BeginDisabled(stagedChanges == 0 && !capturingHotkey);
        if (ImGui::Button(TR("Discard"), ImVec2(Px(110.0f), 0.0f)))
        {
            CaptureSettingsDraft();
            actionMessage = TR("Unapplied edits discarded.");
            actionMessageIsError = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                TR("Discard edits you have not applied. Applied settings and saved defaults stay as they are."));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(stagedChanges == 0 || capturingHotkey);
        if (ImGui::Button(TR("Apply"), ImVec2(Px(100.0f), 0.0f)))
        {
            ApplySettingsDraft(false);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Apply live settings for this session. Saved defaults stay unchanged.\nMode and render "
                              "scale changes require Save and restart.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.07f, 0.04f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, kAmber);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOchre);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAmberDim);
        ImGui::BeginDisabled(capturingHotkey);
        if (ImGui::Button(TR("Save as default"), ImVec2(Px(200.0f), 0.0f)))
        {
            ApplySettingsDraft(true);
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Apply live settings and save your choices, zoom, window layout and divider for future launches.\n"
                              "Mode and render scale changes take effect after restarting.");
        }
        ImGui::EndTable();
    }
}

namespace
{
	// Dictionary diagnostics go to the plugin log; without a sink Localization
	// drops them, which keeps the unit tests quiet.
	void LogLocalizationMessage(bool a_error, std::string_view a_message)
	{
		if (a_error) {
			logger::error("[Localization] {}", a_message);
		} else {
			logger::warn("[Localization] {}", a_message);
		}
	}
}

// Make a_requested the active language. English is always available, so a
// missing or unreadable dictionary only costs a warning and leaves whichever
// labels were already active in place.
void OverlayUI::ActivateLanguageFromSettings(std::string_view a_requested)
{
	using namespace TheosRenderPipeline::Localization;
	SetLogSink(&LogLocalizationMessage);
	if (!Activate(a_requested)) {
		logger::warn("[Localization] '{}' is unavailable ({}); keeping the current language", a_requested,
			LastError());
	}
	if (const auto table = Active()) {
		logger::info("[Localization] {} ({}): {} strings, {} glyphs", table->Description().name,
			table->Description().code, table->Count(), table->Codepoints().size());
	} else {
		logger::info("[Localization] English (built-in labels)");
	}
}

void OverlayUI::DrawLanguageControl()
{
    using namespace TheosRenderPipeline::Localization;
    struct Choice
    {
        std::string label, value;
    };
    std::vector<Choice> choices;
    choices.push_back({ TR("Automatic (Windows)"), "auto" });
    choices.push_back({ TR("English"), "en" });
    for (const auto& description : Catalog()) {
        if (description.code.empty()) {
            continue;
        }
        choices.push_back({ std::format("{} ({})", description.name, description.code), description.code });
    }
    int selected = -1;
    for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        if (choices[i].value == languageDraft) {
            selected = i;
            break;
        }
    }
    if (selected < 0) {
        // A value matched by primary language, or a dictionary the catalog no
        // longer lists, still has to show up as the current selection.
        choices.push_back({ languageDraft, languageDraft });
        selected = static_cast<int>(choices.size()) - 1;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(TR("Language"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(220.0f));
    if (ImGui::BeginCombo("##language", choices[selected].label.c_str())) {
        for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
            const bool current = i == selected;
            if (ImGui::Selectable(choices[i].label.c_str(), current) && !current) {
                languageDraft = choices[i].value;
                RenderPipeline::GetSingleton()->mLanguage = languageDraft;
                ActivateLanguageFromSettings(languageDraft);
                // The atlas cannot be rebuilt inside a frame: the labels drawn
                // later in this one already reference the old glyphs.
                fontRebuildPending = true;
            }
            if (current) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s",
            TR("Loads the overlay labels from the language dictionary folder. Automatic follows the Windows UI "
               "language. Changes apply immediately; Save as default keeps them."));
    }
    ImGui::SameLine();
    if (const auto table = Active()) {
        ImGui::TextDisabled("%s", TRF("Active: {} ({}) - {} strings", table->Description().name,
            table->Description().code, table->Count()).c_str());
    } else {
        ImGui::TextDisabled("%s", TR("Active: English (built-in)"));
    }
    if (const std::string error(LastError()); !error.empty()) {
        ImGui::TextColored(kRust, "%s", error.c_str());
    }
}
