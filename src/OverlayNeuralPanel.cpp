#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include "OverlayFrameView.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

#include "NeuralRenderingMode.h"
#include "FrameGen/NeuralObjectMask.h"
#include "FrameGen/NeuralRuntimeFiles.h"

namespace
{
void DrawNRAppliedPasses(const OverlayUI::FrameView& view, const TheosRenderPipeline::SourceDLSSG::NeuralOptions& applied)
{
    const auto width = applied.beforeUpscaling ? view.renderWidth : view.outputWidth;
    const auto height = applied.beforeUpscaling ? view.renderHeight : view.outputHeight;
    DrawSettingsValue("Placement", applied.beforeUpscaling ? "Before upscaling" : "After upscaling");
    DrawSettingsValue("Scene", std::format("{} x {}", width, height).c_str());
    auto pass = [&](const char* label, float scale, int preset) {
        auto config = applied.reconstruction;
        config.inputScale = scale;
        ImGui::Separator();
        DrawSettingsValue(label,
                          std::format("{} x {}", TheosRenderPipeline::NeuralRendering::ModelExtent(width, config),
                                      TheosRenderPipeline::NeuralRendering::ModelExtent(height, config))
                              .c_str());
        DrawSettingsValue("Network", preset == 1 ? "Shipping" : "Default");
    };
    pass("Pass 1", applied.reconstruction.inputScale, applied.reconstruction.preset);
    if (applied.passes == 2)
    {
        const auto second = applied.EffectiveSecond();
        pass("Pass 2", second.inputScale, second.preset);
        DrawSettingsValue("Settings", second.linked ? "Linked" : "Independent");
    }
}

// Why NR cannot run now, or null. Shown above Base's controls.
const char* NeuralUnavailableReason(const OverlayUI::FrameView& view, int upscaleType, bool nrRuntimePresent)
{
    if (view.sourceReady && !view.nvidiaAdapter) { return "Neural Rendering requires NVIDIA RTX hardware. XeSS upscaling and XeFG remain available."; }
    if (view.neuralRuntimePath.empty())
    {
        return "NR runtime path is empty. Configure NeuralRenderingRuntimePath in TheosRenderPipeline.ini "
               "and restart Skyrim.";
    }
    if (!nrRuntimePresent)
    {
        return "NR runtime DLL not found. Install the optional nvngx_dlssnr.dll at the path below and restart Skyrim.";
    }
    if (!view.sourceReady)
    {
        return "The NVIDIA host is not ready. Check runtime status and restart Skyrim.";
    }
    if (view.neuralState.failed)
    {
        return "NR failed. See the error in the left column.";
    }
    if (!TheosRenderPipeline::SupportsNeuralRenderingMode(upscaleType, view.communityShaders))
    {
        return "NR requires DLSS or DLAA mode. Select either in Image and restart Skyrim.";
    }
    if (!view.communityShaders && !view.dedicatedUITexture)
    {
        return "NR requires dedicated UI composition. Set NativeUICompositionMode=0 in the INI and "
               "restart Skyrim. If it is already 0, check the log for a composition failure.";
    }
    return nullptr;
}

void DrawNeuralObjectSettings(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                              bool unavailable)
{
    // Object rules belong to each pass; there is no switch or limit for the chain.
    namespace NR = TheosRenderPipeline::NeuralRendering;
    auto& masks = draft.neuralObjectMasks;
    ImGui::BeginDisabled(unavailable);
    ImGui::TextUnformatted("Object-specific NR (experimental)");
    // Session-only mask preview: tick the groups to show over the finished image.
    ImGui::TextUnformatted("Mask preview (session only)");
    for (unsigned group=0;group<4;++group) {
        bool shown=(masks.previewGroups>>group)&1;
        if (group) { ImGui::SameLine(); }
        if (ImGui::Checkbox((std::string("Show ")+NR::kObjectGroups[group]).c_str(),&shown)) {
            masks.previewGroups=shown?(masks.previewGroups|(1u<<group)):(masks.previewGroups&~(1u<<group));
        }
    }
    ImGui::BeginDisabled(!masks.previewGroups);
    ImGui::Checkbox("Classes only",&masks.previewClassesOnly);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Green: first-person | Blue: player | Orange: humanoids | Purple: creatures");
    ImGui::TextWrapped("Tick a group to override it in that pass. Strength 0 keeps this pass's input; 1 keeps its full "
        "result. Pass 2 at 0 keeps Pass 1's work. Tone and structure replace that pass's values and require a "
        "reconstruction NR runtime; active controls pause its automatic skin mask. Object rules are independent of "
        "Match Pass 1 and weather presets.");
    if (ImGui::BeginTable("objectPasses", 2, ImGuiTableFlags_SizingStretchSame)) {
        for(unsigned pass=0;pass<2;++pass) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(pass));
            ImGui::BeginDisabled(pass==1&&draft.neuralPasses!=2);
            ImGui::TextColored(kAmber, "Pass %u",pass+1);
            for(unsigned group=0;group<4;++group) {
                ImGui::PushID(static_cast<int>(group));
                auto& rule=masks.passes[pass].groups[group];
                ImGui::Checkbox(NR::kObjectGroups[group],&rule.enabled);
                ImGui::BeginDisabled(!rule.enabled);
                ImGui::SliderFloat("Strength",&rule.strength,0,1,"%.2f");
                ImGui::SliderFloat("Tone",&rule.tone,0,1,"%.2f");
                ImGui::SliderFloat("Structure",&rule.structure,0,1,"%.2f");
                ImGui::EndDisabled();ImGui::PopID();
            }
            ImGui::EndDisabled();ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("%s", view.neuralObjectStatus.c_str());
    ImGui::TextDisabled(view.neuralObjectCurrent ?
        "Current-frame classification available" : "No current-frame classification; normal NR");
    DrawSettingsHelp("Experimental opaque-object capture for Skyrim 1.6.1170, captured only while a running pass overrides a "
        "group or the preview is on. Transparent/alpha-tested or unclassified pixels keep normal NR. Strength adds blending "
        "work and memory; both inference passes still run. UI is composed afterwards.");
    ImGui::EndDisabled();
}

// The pass shown under the pass tabs. Base and presets share it.
int& SelectedPass()
{
    static int selected{};
    return selected;
}

// Pass 2's settings compared with Pass 1's, for the state column.
struct PassDecor final : PresetDecor
{
    TheosRenderPipeline::NeuralRendering::SecondPassSettings& shown;
    const TheosRenderPipeline::NeuralRendering::Reconstruction& first;
    const TheosRenderPipeline::NeuralRendering::Tuning& tuning;
    PassDecor(TheosRenderPipeline::NeuralRendering::SecondPassSettings& shown,
        const TheosRenderPipeline::NeuralRendering::Reconstruction& first,
        const TheosRenderPipeline::NeuralRendering::Tuning& tuning)
        : shown(shown), first(first), tuning(tuning) {}

    static constexpr const char* kKeys[]{"Pass2Style", "Pass2Intensity", "Pass2LocalTone", "Pass2LocalStructure",
        "Pass2SkinStructure", "Pass2AutoSkinMask", "Pass2UICorrection", "Pass2InputScale", "Pass2Network"};
    enum class Kind { Style, Value, Percent, Toggle, Network };

    // Calls visit(pass2 value, pass1 value, kind) for key; false for other keys.
    template <class Visit> bool With(std::string_view key, Visit&& visit) const
    {
        auto& own = shown.tuning;
        if (key == "Pass2Style") { visit(own.style, tuning.style, Kind::Style); }
        else if (key == "Pass2Intensity") { visit(own.intensity, tuning.intensity, Kind::Value); }
        else if (key == "Pass2LocalTone") { visit(own.localToneStrength, tuning.localToneStrength, Kind::Value); }
        else if (key == "Pass2LocalStructure") { visit(own.localStructureStrength, tuning.localStructureStrength, Kind::Value); }
        else if (key == "Pass2SkinStructure") { visit(own.skinStructureStrength, tuning.skinStructureStrength, Kind::Value); }
        else if (key == "Pass2AutoSkinMask") { visit(own.useAutoSkinMask, tuning.useAutoSkinMask, Kind::Toggle); }
        else if (key == "Pass2UICorrection") { visit(own.uiCorrection, tuning.uiCorrection, Kind::Toggle); }
        else if (key == "Pass2InputScale") { visit(shown.inputScale, first.inputScale, Kind::Percent); }
        else if (key == "Pass2Network") { visit(shown.preset, first.preset, Kind::Network); }
        else { return false; }
        return true;
    }
    bool Changed(const char* key) const override
    {
        bool changed = false;
        With(key, [&](auto& own, const auto& reference, Kind) { changed = own != reference; });
        return changed;
    }
    std::string BaseValue(const char* key) const override
    {
        std::string text;
        With(key, [&](auto&, const auto& reference, Kind kind) {
            const auto value = static_cast<double>(reference);
            switch (kind)
            {
            case Kind::Style: text = std::format("Style {}", static_cast<int>(value)); break;
            case Kind::Percent: text = std::format("{:.1f}%", value * 100); break;
            case Kind::Toggle: text = value != 0 ? "On" : "Off"; break;
            case Kind::Network: text = value != 0 ? "Previous model" : "Default"; break;
            default: text = std::format("{:.3f}", value); break;
            }
        });
        return text;
    }
    void Reset(const char* key) override
    {
        With(key, [](auto& own, const auto& reference, Kind) { own = reference; });
    }
    const char* EditingTime() const override { return nullptr; }
    const char* Reference() const override { return "Pass 1"; }
    int Differences() const
    {
        return static_cast<int>(std::ranges::count_if(kKeys, [&](const char* key) { return Changed(key); }));
    }
};

enum class PassAction { None, Add, Remove };

// The selected pass's folder tab joins the panel below; the others sit behind.
// Its background matches the panel's.
const ImVec4 kPassPanel{0.098f, 0.104f, 0.106f, 1.0f};
const ImVec4 kPassBehind{0.058f, 0.063f, 0.066f, 1.0f};

struct PassPanelGeometry
{
    float tabLeft{}, tabRight{};
};
PassPanelGeometry g_passGeometry;

void FillRect(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color)
{
    draw->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)), color);
}

// Pass tabs, then + to add Pass 2. note is drawn at the right, muted.
// changed[pass] tints a tab's name amber: it differs from Base in a preset, or
// Pass 2 differs from Pass 1.
PassAction DrawPassTabs(const OverlayUI::FrameView& view, int& selected, int passes, bool editable, const std::string& note,
    const bool (&changed)[2])
{
    auto* draw = ImGui::GetWindowDrawList();
    const auto& style = ImGui::GetStyle();
    const auto origin = ImGui::GetCursorScreenPos();
    const float available = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() + Px(6.0f);
    const float lift = Px(4.0f);
    const float tabWidth = Px(132.0f);
    const float addWidth = ImGui::GetFrameHeight() + Px(4.0f);
    const float line = std::round((std::max)(1.0f, Px(1.0f)));
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    const auto& live = view.sourceNeural;
    PassAction action = PassAction::None;

    ImGui::PushID("passTabs");
    // Register the strip's space first; the tabs are drawn over it.
    ImGui::Dummy(ImVec2(available, height));
    float x = origin.x;
    for (int pass = 0; pass < passes; ++pass)
    {
        const bool active = pass == selected;
        const float top = origin.y + (active ? 0.0f : lift);
        const float bottom = origin.y + height;
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::PushID(pass);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("tab", ImVec2(tabWidth, bottom - top))) { selected = pass; }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            ImGui::SetTooltip("%s", pass == 0 ? "Runs on the scene."
                                              : "Processes Pass 1's result with its own history. Adds GPU time and "
                                                "memory; presets switch it on and off without restarting NR.");
        }
        FillRect(draw, x, top, x + tabWidth, bottom + (active ? line : 0.0f),
                 ImGui::GetColorU32(active ? kPassPanel : hovered ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : kPassBehind));
        FillRect(draw, x, top, x + line, bottom, border);
        FillRect(draw, x, top, x + tabWidth, top + line, border);
        FillRect(draw, x + tabWidth - line, top, x + tabWidth, bottom, border);
        if (active) { FillRect(draw, x, top, x + tabWidth, top + Px(2.0f), ImGui::GetColorU32(kAmber)); }
        const auto name = std::format("Pass {}", pass + 1);
        const float textY = top + ((bottom - top) - ImGui::GetTextLineHeight()) * 0.5f;
        const auto nameColor = changed[pass] ? (active ? kAmber : kAmberDim) : active ? kIvory : kMuted;
        draw->AddText(ImVec2(x + style.FramePadding.x, textY), ImGui::GetColorU32(nameColor), name.c_str());

        float right = x + tabWidth - style.FramePadding.x;
        if (pass == 1 && editable && (active || hovered))
        {
            // Remove Pass 2. Its own settings are kept for when it is added again.
            const float size = ImGui::GetTextLineHeight();
            ImGui::SetCursorScreenPos(ImVec2(right - size, textY));
            if (ImGui::InvisibleButton("remove", ImVec2(size, size))) { action = PassAction::Remove; }
            const bool over = ImGui::IsItemHovered();
            if (over) { ImGui::SetTooltip("Remove Pass 2. Its own settings are kept for later."); }
            const float inset = size * 0.28f;
            const auto color = ImGui::GetColorU32(over ? kAmber : kMuted);
            draw->AddLine(ImVec2(right - size + inset, textY + inset), ImVec2(right - inset, textY + size - inset), color, Px(1.5f));
            draw->AddLine(ImVec2(right - inset, textY + inset), ImVec2(right - size + inset, textY + size - inset), color, Px(1.5f));
            right -= size + style.ItemInnerSpacing.x;
        }
        // Live state: whether this pass is running now.
        const char* badge = nullptr;
        ImVec4 badgeColor = kSage;
        if (live.active && pass < view.neuralApplied.passes)
        {
            badge = pass == 0 || live.effectivePasses == 2 ? "ON" : "PAUSED";
            if (pass == 1 && live.effectivePasses != 2) { badgeColor = kOchre; }
        }
        if (badge)
        {
            const float badgeWidth = ImGui::CalcTextSize(badge).x;
            draw->AddText(ImVec2(right - badgeWidth, textY), ImGui::GetColorU32(badgeColor), badge);
        }
        if (active) { g_passGeometry = {x, x + tabWidth}; }
        ImGui::PopID();
        x += tabWidth - line;
    }
    if (passes < 2)
    {
        const float top = origin.y + lift, bottom = origin.y + height;
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::BeginDisabled(!editable);
        if (ImGui::InvisibleButton("add", ImVec2(addWidth, bottom - top))) { action = PassAction::Add; }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (hovered) { ImGui::SetTooltip("Add Pass 2. It starts out following Pass 1."); }
        FillRect(draw, x, top, x + addWidth, bottom,
                 ImGui::GetColorU32(hovered && editable ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : kPassBehind));
        FillRect(draw, x, top, x + line, bottom, border);
        FillRect(draw, x, top, x + addWidth, top + line, border);
        FillRect(draw, x + addWidth - line, top, x + addWidth, bottom, border);
        const auto plus = ImGui::CalcTextSize("+");
        draw->AddText(ImVec2(x + (addWidth - plus.x) * 0.5f, top + ((bottom - top) - plus.y) * 0.5f),
                      ImGui::GetColorU32(editable ? (hovered ? kAmber : kIvory) : kMuted), "+");
    }
    if (!note.empty())
    {
        const auto size = ImGui::CalcTextSize(note.c_str());
        draw->AddText(ImVec2(origin.x + available - size.x - style.FramePadding.x,
                             origin.y + height - size.y - style.FramePadding.y),
                      ImGui::GetColorU32(kMuted), note.c_str());
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
    return action;
}

// The selected pass's settings, in a panel that the selected tab opens into.
struct PassPanel
{
    PassPanelGeometry tab;
};
PassPanel BeginPassPanel()
{
    PassPanel panel{g_passGeometry};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPassPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::BeginChild("passPanel", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    return panel;
}
void EndPassPanel(const PassPanel& panel)
{
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    // The border, open under the selected tab.
    auto* draw = ImGui::GetWindowDrawList();
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float line = std::round((std::max)(1.0f, Px(1.0f)));
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    FillRect(draw, min.x, min.y, min.x + line, max.y, border);
    FillRect(draw, max.x - line, min.y, max.x, max.y, border);
    FillRect(draw, min.x, max.y - line, max.x, max.y, border);
    FillRect(draw, min.x, min.y, (std::max)(min.x, panel.tab.tabLeft + line), min.y + line, border);
    FillRect(draw, (std::min)(max.x, panel.tab.tabRight - line), min.y, max.x, min.y + line, border);
}

// Base and presets share these controls. Presets add each row's state column.
void DrawNeuralSettings(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                        bool& sharpening, float& sharpness, bool unavailable)
{
    namespace NR = TheosRenderPipeline::NeuralRendering;
    auto& reconstruction = draft.neuralReconstruction;
    auto& second = draft.neuralSecondPass;
    const bool cs = view.communityShaders;
    const bool producerColor = cs && draft.neuralBeforeUpscaling;
    const bool twoPasses = draft.neuralPasses == 2;
    const float label = LabelWidth({"One pass while weapons are drawn", "Input colour is linear HDR", "NR input resolution"});
    const auto width = draft.neuralBeforeUpscaling ? view.renderWidth : view.outputWidth;
    const auto height = draft.neuralBeforeUpscaling ? view.renderHeight : view.outputHeight;
    const char* placements[]{"Before upscaling", "After upscaling"};
    const char* styles[]{"Style 0", "Style 1", "Style 2", "Style 3", "Style 4", "Style 5", "Style 6", "Style 7"};
    const char* networks[]{"Default", "Previous model"};
    const char* methods[]{"Auto | Reconstruct reduced input", "Residual | Add NR changes",
                          "Ratio | Transfer lighting and colour"};

    if (BeginSettingRows("nrTop", label))
    {
        SettingRow("Neural Rendering", "NeuralRendering", false,
                   !TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable),
                   [&] { return ImGui::Checkbox("##v", &draft.neuralEnabled); });
        SettingRow("Placement", "BeforeUpscaling", true, unavailable, [&] {
            int placement = draft.neuralBeforeUpscaling ? 0 : 1;
            const bool changed = ImGui::Combo("##v", &placement, placements, IM_ARRAYSIZE(placements));
            if (changed) { draft.neuralBeforeUpscaling = placement == 0; }
            DrawSettingsHelp("Both passes use the selected side of upscaling. UI is composed afterwards.");
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::TextDisabled("! restarts NR briefly when changed");

    auto* decor = ActivePresetDecor();
    // Pass 2 follows Pass 1 until one of its own settings changes.
    auto shown = NR::EffectiveSecondPass(second, reconstruction, draft.neuralTuning);
    const auto before = shown;
    auto& selected = SelectedPass();
    if (!twoPasses) { selected = 0; }
    ImGui::Spacing();
    PassDecor passDecor{shown, reconstruction, draft.neuralTuning};
    bool tabChanged[2]{};
    if (decor)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            const std::string prefix = pass ? "Pass2" : "Pass1";
            tabChanged[pass] = std::ranges::any_of(PassDecor::kKeys, [&](const char* key) {
                return decor->Changed((prefix + (key + 5)).c_str());
            });
        }
        tabChanged[1] |= AnyChanged({"Passes", "Pass2SameAsPass1"});
    }
    else
    {
        tabChanged[1] = passDecor.Differences() > 0;
    }
    const auto action = DrawPassTabs(view, selected, draft.neuralPasses, !unavailable,
        decor && decor->EditingTime() ? std::format("editing {}", decor->EditingTime()) : std::string(), tabChanged);
    if (action == PassAction::Add) { draft.neuralPasses = 2; selected = 1; }
    if (action == PassAction::Remove) { draft.neuralPasses = 1; selected = 0; }
    const bool pass2 = selected == 1 && draft.neuralPasses == 2;

    // In Base, Pass 2's rows compare with Pass 1 as a preset's compare with Base.
    // A preset keeps its own comparison with Base.
    const bool comparePasses = pass2 && !decor;
    if (comparePasses) { ActivePresetDecor() = &passDecor; }
    bool followPass1 = false;
    const auto panel = BeginPassPanel();
    if (pass2)
    {
        const int differences = comparePasses ? passDecor.Differences() : 0;
        const char* note = second.linked ? "Pass 2 refines Pass 1's result and follows its settings."
                                         : "Pass 2 refines Pass 1's result. Amber rows differ from Pass 1.";
        Flow flow;
        flow.Text(note);
        ImGui::BeginDisabled(second.linked && !differences);
        if (flow.Button("Reset all to Pass 1")) { followPass1 = true; }
        ImGui::EndDisabled();
        DrawSettingsHelp("Pass 2 follows Pass 1 again. Its own settings are kept for later.");
    }
    if (decor && AnyChanged({"Passes", "Pass2SameAsPass1"}))
    {
        // A preset that turns Pass 2 on or off, or makes it follow Pass 1.
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmber, "Passes differ from %s", decor->Reference());
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##passes"))
        {
            for (const auto* key : {"Passes", "Pass2SameAsPass1"}) { if (decor->Changed(key)) { decor->Reset(key); } }
        }
    }

    const std::string prefix = pass2 ? "Pass2" : "Pass1";
    const auto key = [&](const char* name) { return prefix + name; };
    auto& tuning = pass2 ? shown.tuning : draft.neuralTuning;
    float& scale = pass2 ? shown.inputScale : reconstruction.inputScale;
    int& network = pass2 ? shown.preset : reconstruction.preset;
    if (BeginSettingRows("passLook", label))
    {
        SettingRow("Style", key("Style").c_str(), false, unavailable,
                   [&] { return ImGui::Combo("##v", &tuning.style, styles, IM_ARRAYSIZE(styles)); });
        DescribedSettingRow("Intensity", "Strength of the NR result", key("Intensity").c_str(), false, unavailable,
                            [&] { return ImGui::SliderFloat("##v", &tuning.intensity, 0, 2); });
        SettingRow("Local tone", key("LocalTone").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localToneStrength, 0, 2); });
        SettingRow("Local structure", key("LocalStructure").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localStructureStrength, 0, 2); });
        DescribedSettingRow("Skin structure", "-1 follows local structure", key("SkinStructure").c_str(), false,
                            unavailable, [&] {
            const bool changed = ImGui::SliderFloat("##v", &tuning.skinStructureStrength, -1, 2);
            DrawSettingsHelp("Ctrl-click a slider to type a value.");
            return changed;
        });
        SettingRow("Automatic skin mask", key("AutoSkinMask").c_str(), false, unavailable,
                   [&] { return ImGui::Checkbox("##v", &tuning.useAutoSkinMask); });
        SettingRow("UI correction", key("UICorrection").c_str(), false, unavailable, [&] {
            ImGui::BeginDisabled(cs || draft.neuralBeforeUpscaling);
            const bool changed = ImGui::Checkbox("##v", &tuning.uiCorrection);
            ImGui::EndDisabled();
            if (cs || draft.neuralBeforeUpscaling)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(cs ? "CS draws UI after NR" : "after upscaling only");
            }
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (BeginSettingRows("passInput", label))
    {
        auto extent = reconstruction;
        extent.inputScale = scale;
        const auto model = width && height ? std::format("{} x {} model input", NR::ModelExtent(width, extent),
                                                         NR::ModelExtent(height, extent))
                                           : std::string();
        DescribedSettingRow("NR input resolution", model.c_str(), key("InputScale").c_str(), true, unavailable, [&] {
            float percent = scale * 100;
            const bool changed = ImGui::SliderFloat("##v", &percent, 25, 100, "%.1f%%");
            if (changed) { scale = percent / 100; }
            DrawSettingsHelp("Relative to the selected stage's scene size. Apply to update.");
            return changed;
        });
        DescribedSettingRow("Network", "Default is the runtime's newest", key("Network").c_str(), true, unavailable, [&] {
            const bool changed = ImGui::Combo("##v", &network, networks, IM_ARRAYSIZE(networks));
            DrawSettingsHelp("Default runs the runtime's newest network. Previous model runs the 310.8 network, which "
                             "310.9.16 also includes. Separate from the DLSS model preset.");
            return changed;
        });
        ImGui::EndTable();
    }
    EndPassPanel(panel);
    if (comparePasses) { ActivePresetDecor() = nullptr; }
    if (shown != before)
    {
        second = shown;
        second.linked = false;
    }
    if (followPass1) { second.linked = true; }

    DrawSettingsHeading("Sharpening");
    if (BeginSettingRows("sharpening", label))
    {
        SettingRow("Sharpening", "SharpeningEnabled", false, cs, [&] {
            const bool changed = ImGui::Checkbox("##v", &sharpening);
            ImGui::SameLine();
            ImGui::TextDisabled(cs ? "Community Shaders controls sharpening" : "with or without NR");
            return changed;
        });
        SettingRow("Sharpening strength", "Sharpness", false, cs || !sharpening,
                   [&] { return ImGui::SliderFloat("##v", &sharpness, 0, 1, "%.2f"); });
        ImGui::EndTable();
    }

    DrawSettingsHeading("Performance");
    if (BeginSettingRows("performance", label))
    {
        const char* twoPassHelp = "Temporarily skips Pass 2 when either selected condition is active. Keeps your NR "
                                  "resolution, tuning and saved two-pass setting. The image may change when switching.";
        SettingRow("One pass in combat", "OnePassInCombat", false, unavailable || !twoPasses, [&] {
            const bool changed = ImGui::Checkbox("##v", &draft.neuralCombat.inCombat);
            DrawSettingsHelp(twoPassHelp);
            return changed;
        });
        SettingRow("One pass while weapons are drawn", "OnePassWeaponsDrawn", false, unavailable || !twoPasses, [&] {
            const bool changed = ImGui::Checkbox("##v", &draft.neuralCombat.weaponsDrawn);
            DrawSettingsHelp(twoPassHelp);
            return changed;
        });
        SettingRow("Return delay", "ReturnDelay", false, unavailable || !twoPasses || !draft.neuralCombat.Enabled(), [&] {
            const bool changed = ImGui::SliderFloat("##v", &draft.neuralCombat.recoverySeconds, 0.0f, 30.0f, "%.1f s");
            DrawSettingsHelp("Waits this long after all selected conditions clear before restoring two passes. "
                             "Pausing the game pauses the delay.");
            return changed;
        });
        SettingRow("Peripheral compression", "PeripheralCompression", true, unavailable, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.peripheralCompression);
            DrawSettingsHelp("Preserves sampling density across the central 80% of each axis and compresses the edges. "
                             "Uses about 19% fewer model pixels at the same input resolution. Inspect edge quality "
                             "when moving.");
            return changed;
        });
        SettingRow("Combined preparation", "CombinedPreparation", true, unavailable, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.fusedPreparation);
            DrawSettingsHelp("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                             "peripheral compression. Some configurations have no passes to combine. Applies to both "
                             "passes.");
            return changed;
        });
        ImGui::EndTable();
    }

    DrawSettingsHeading("Reconstruction");
    if (BeginSettingRows("reconstruction", label))
    {
        SettingRow("Method", "ReconstructionMethod", true, unavailable || producerColor, [&] {
            int method = static_cast<int>(reconstruction.method);
            const bool changed = ImGui::Combo("##v", &method, methods, IM_ARRAYSIZE(methods));
            if (changed) { reconstruction.method = static_cast<NR::ResolveMethod>(method); }
            DrawSettingsHelp(producerColor ? "CS restores scene colour automatically before DLSS."
                                           : "Residual clamps colour to 0..1. Use Ratio for linear HDR input.");
            return changed;
        });
        const bool ratioOff = unavailable || (!producerColor && reconstruction.method != NR::ResolveMethod::Ratio);
        SettingRow(producerColor ? "Effect strength" : "Ratio effect strength", "EffectStrength", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.transferStrength, 0, 2); });
        SettingRow(producerColor ? "Colour strength" : "Ratio colour strength", "ColourStrength", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.colourStrength, 0, 2); });
        SettingRow(producerColor ? "Maximum scene gain" : "Maximum luma ratio", "MaximumRatio", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.maxRatio, producerColor ? 1.0f : 0.01f, 16); });
        SettingRow(producerColor ? "Scene normalization" : "HDR white point", "WhitePoint", true, ratioOff, [&] {
            const bool changed = ImGui::InputFloat("##v", &reconstruction.whitePoint, 0, 0, "%.4f");
            DrawSettingsHelp(producerColor ? "Scene normalization sets the brightness range NR sees."
                                           : "White point for encoding linear HDR input before model evaluation.");
            return changed;
        });
        SettingRow("Input colour is linear HDR", "InputHDR", true, ratioOff || producerColor, [&] {
            const bool changed = ImGui::Checkbox("##v", &reconstruction.colorIsHDR);
            DrawSettingsHelp("HDR input is used by Ratio reconstruction; it does not change the display HDR mode.");
            return changed;
        });
        ImGui::EndTable();
    }
}

// Which nvngx_dlssnr*.dll NR loads. Editable even while NR is unavailable, so a
// missing runtime can be swapped for an installed one. Apply switches it.
void DrawNeuralRuntime(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft)
{
    const auto& runtimes = view.neuralRuntimes;
    using TheosRenderPipeline::SourceDLSSG::NeuralRuntimeLabel;
    if (BeginSettingRows("nrRuntime", LabelWidth({"One pass while weapons are drawn", "Input colour is linear HDR", "NR input resolution"})))
    {
        SettingRow("Runtime", "NeuralRenderingRuntimePath", true, false, [&] {
            bool changed = false;
            if (ImGui::BeginCombo("##v", NeuralRuntimeLabel(draft.neuralRuntime).c_str())) {
                for (const auto& runtime : runtimes) {
                    ImGui::PushID(runtime.c_str());
                    const bool selected = runtime == draft.neuralRuntime;
                    if (ImGui::Selectable(NeuralRuntimeLabel(runtime).c_str(), selected) && !selected) { draft.neuralRuntime = runtime; changed = true; }
                    if (selected) { ImGui::SetItemDefaultFocus(); }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("File: %s", runtime.c_str()); }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            DrawSettingsHelp("Automatic chooses the runtime for your GPU. Universal unlocks NVIDIA 310.9.16 on RTX 20/30/40; RTX 50 uses it natively. The kernel pack uses FP16 fallback on RTX 20/30 and FP8 on RTX 40. Legacy INT8 is an RTX 20/30 alternative. Hover an entry for its filename.");
            return changed;
        });
        ImGui::EndTable();
    }
    const auto& applied = view.neuralApplied;
    if (applied.automaticRuntime && applied.enabled && TheosRenderPipeline::SourceDLSSG::IsAutomaticNeuralRuntime(draft.neuralRuntime))
        ImGui::TextDisabled("Automatic is using %s", NeuralRuntimeLabel(applied.runtimePath.string()).c_str());
}

void DrawSourceNeuralControls(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                              bool& sharpening, float& sharpness, int upscaleType, bool nrRuntimePresent)
{
    const auto& state = view.neuralState;
    const char* unavailableReason = NeuralUnavailableReason(view, upscaleType, nrRuntimePresent);
    const bool unavailable = unavailableReason != nullptr;
    const auto& applied = view.neuralApplied;
    if (unavailable)
    {
        ImGui::TextWrapped("%s", unavailableReason);
        ImGui::TextWrapped("NR runtime path: %s", view.neuralRuntimePath.c_str());
    }
    if (applied.enabled && !state.active && !unavailable)
    {
        ImGui::TextWrapped("%s", state.status.c_str());
    }
    if (!state.fallback.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kOchre);
        ImGui::TextWrapped("%s", state.fallback.c_str());
        ImGui::PopStyleColor();
    }
    DrawNeuralRuntime(view, draft);
    DrawNeuralObjectSettings(view, draft, unavailable);
    DrawNeuralSettings(view, draft, sharpening, sharpness, unavailable);
}
} // namespace

void OverlayUI::DrawNeuralRenderingPanel(float height, const FrameView& view)
{
    if (!ImGui::BeginTabItem("Neural Rendering", nullptr,
                             requestedPage == SettingsPage::NeuralRendering ? ImGuiTabItemFlags_SetSelected : 0))
        return;
    if (BeginSettingsColumns("neural", height, view))
    {
        const auto& applied = view.neuralApplied;
        const auto& state = view.sourceNeural;
        DrawStatusLabel(state.failed      ? "NR failed"
                        : state.active    ? "NR active"
                        : applied.enabled ? "NR inactive"
                                          : "NR off",
                        state.failed   ? UIHealth::kError
                        : state.active ? UIHealth::kHealthy
                                       : UIHealth::kIdle);
        const auto& timing = state.telemetry;
        DrawSettingsValue("Inference", state.active && timing.gpuSamples
                                           ? std::format("{:.2f} ms", timing.AverageGPUMicroseconds() / 1000.0).c_str()
                                           : "unavailable");
        DrawSettingsHelp("Combined model inference and inter-pass preparation. Excludes input preparation, final Pass "
                         "2 restoration, reconstruction, UI composition and the D3D11/D3D12 handoff.");
        if (state.active)
            DrawStageMeasurements(view, SettingsPage::NeuralRendering);
        DrawNRAppliedPasses(view, applied);
        if (state.active) {
            const auto execution = state.passOverride == TheosRenderPipeline::NeuralRendering::PassOverride::None ?
                std::format("{} pass(es)", state.effectivePasses) :
                std::format("1 pass - {}", TheosRenderPipeline::NeuralRendering::PassOverrideName(state.passOverride));
            DrawSettingsValue("Running", execution.c_str());
        }
        if (state.failed || (applied.enabled && !state.active))
            ImGui::TextWrapped("%s", state.status.c_str());
        if (showDeveloperControls)
        {
            ImGui::Separator();
            if (!state.failed && (!applied.enabled || state.active))
                ImGui::TextWrapped("%s", state.status.c_str());
            ImGui::TextWrapped("Submitted frames %llu | history resets %llu",
                               static_cast<unsigned long long>(state.evaluations),
                               static_cast<unsigned long long>(state.resets));
            if (timing.gpuSamples)
                ImGui::TextWrapped("Inference max %.3f ms | %llu samples | %llu query failures",
                                   timing.MaximumGPUMicroseconds() / 1000.0,
                                   static_cast<unsigned long long>(timing.gpuSamples),
                                   static_cast<unsigned long long>(timing.gpuQueryFailures));
        }
        DrawPresetList(view);
        NextSettingsColumn(height);
        if (PresetEditorSelected()) {
            const bool unavailable = NeuralUnavailableReason(view, settingsDraft.upscaleType, nrRuntimePresent) != nullptr;
            DrawPresetEditor(view, [&](TheosRenderPipeline::SourceDLSSG::Preferences& draft, bool& sharpening, float& sharpness) {
                DrawNeuralSettings(view, draft, sharpening, sharpness, unavailable);
            });
        }
        else { DrawSourceNeuralControls(view, settingsDraft.sourceDLSSG, settingsDraft.sharpening, settingsDraft.sharpness, settingsDraft.upscaleType, nrRuntimePresent); }
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}
