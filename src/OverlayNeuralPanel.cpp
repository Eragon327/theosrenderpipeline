#include "OverlayUI.h"
#include "OverlayUIStyle.h"
#include "OverlaySettingRows.h"
#include "OverlayFrameView.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <vector>
#include <imgui_internal.h>
#include <string_view>
#include <PCH.h>

using namespace TheosRenderPipeline::Overlay;

#include "NeuralRenderingMode.h"
#include "FrameGen/NeuralObjectMask.h"
#include "FrameGen/NeuralRuntimeFiles.h"

namespace
{
// Base only: the live, session-only NR comparison. Set while the panel draws
// Base; weather overrides never show it.
std::function<void(TheosRenderPipeline::NeuralRendering::CompareSettings)> g_setCompare;

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
        DrawSettingsValue("Network", NeuralNetworkName(preset));
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

// Whether the NR runtime in use predates the reconstruction contract, which
// object tone and structure need. Unknown until NR has applied a runtime.
bool LegacyNeuralRuntime(const OverlayUI::FrameView& view)
{
    const auto& applied = view.neuralApplied;
    if (!applied.enabled || applied.runtimePath.empty()) { return false; }
    const auto name = TheosRenderPipeline::SourceDLSSG::FoldRuntimePath(applied.runtimePath.filename().string());
    return name == "nvngx_dlssnr.dll" || name.starts_with("nvngx_dlssnr_int8");
}

// An open, amber eye when shown; a dim, slashed eye when hidden. Fits a square of size.
void DrawEye(ImDrawList* draw, ImVec2 min, float size, bool shown, bool hovered)
{
    const float w = size, h = size * 0.62f;
    const ImVec2 left(min.x, min.y + size * 0.5f), right(min.x + w, min.y + size * 0.5f);
    const ImVec2 centre(min.x + w * 0.5f, min.y + size * 0.5f);
    const auto colour = ImGui::GetColorU32(shown ? kAmber : hovered ? kIvory : kMuted);
    const float stroke = (std::max)(1.0f, Px(1.4f));
    draw->AddBezierQuadratic(left, ImVec2(centre.x, centre.y - h), right, colour, stroke);
    draw->AddBezierQuadratic(left, ImVec2(centre.x, centre.y + h), right, colour, stroke);
    if (shown)
    {
        draw->AddCircleFilled(centre, size * 0.17f, colour);
    }
    else
    {
        draw->AddCircle(centre, size * 0.15f, colour, 0, stroke);
        draw->AddLine(ImVec2(min.x + w * 0.15f, min.y + size * 0.88f), ImVec2(min.x + w * 0.85f, min.y + size * 0.12f),
                      colour, stroke);
    }
}

// The groups shown in the preview, for its footer: "NPCs and Creatures".
std::string ShownGroups(unsigned groups, const char* const (&names)[4])
{
    std::vector<const char*> shown;
    for (unsigned i = 0; i < 4; ++i) { if ((groups >> i) & 1) { shown.push_back(names[i]); } }
    if (shown.size() == 4) { return "all groups"; }
    std::string text;
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
        text += i == 0 ? "" : i + 1 == shown.size() ? " and " : ", ";
        text += shown[i];
    }
    return text;
}

// One pass's object rules: the mask preview, then a row per group with its
// override checkbox and Strength, Tone and Structure. Aligned with the shared
// control column. Presets cannot change object rules, so they show greyed.
void DrawPassObjects(const OverlayUI::FrameView& view, TheosRenderPipeline::NeuralRendering::ObjectMaskSettings& masks,
                     unsigned pass, bool unavailable, bool preset)
{
    namespace NR = TheosRenderPipeline::NeuralRendering;
    static constexpr const char* names[]{"First person", "Player", "NPCs", "Creatures"};
    static const ImVec4 swatches[]{{0.37f, 0.72f, 0.65f, 1}, {0.44f, 0.62f, 0.88f, 1}, {0.85f, 0.65f, 0.28f, 1},
                                   {0.70f, 0.54f, 0.85f, 1}};
    const auto& style = ImGui::GetStyle();
    const auto* grid = Rows::ActiveGrid();
    const float tableX = ImGui::GetCursorScreenPos().x;
    const float label = grid ? (std::max)(Px(40.0f), grid->controlX - Rows::ColumnStart(tableX, 0))
                             : LabelWidth({"Peripheral compression"});
    const float control = grid ? grid->controlWidth : Px(Rows::MaxControl);
    const float check = ImGui::GetFrameHeight();
    const float slider = (std::max)(Px(30.0f), (control - check - style.ItemSpacing.x * 3) / 3);
    const bool legacy = LegacyNeuralRuntime(view);
    const bool locked = unavailable || preset || !view.neuralObjectsAvailable;
    const char* lockedReason = preset ? "Object rules are set in Base; weather overrides do not change them."
                             : !view.neuralObjectsAvailable ? view.neuralObjectStatus.c_str()
                                                            : "Neural Rendering is unavailable.";
    auto& rules = masks.passes[pass];
    if (!ImGui::BeginTable("passObjects", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);

    // Heading: the column names sit over the sliders below.
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Objects");
    ImGui::SameLine();
    ImGui::TextDisabled("experimental");
    DrawSettingsHelp(locked ? lockedReason
        : "Tick a group to override it in this pass. Strength blends this pass's result with its input: 0 keeps "
          "the input, 1 the full result, so Pass 2 at 0 keeps Pass 1's work. Tone and structure replace this "
          "pass's values for the group and pause its automatic skin mask. Rules are independent of the other pass "
          "and of weather overrides. Inference still runs at any strength.");
    ImGui::TableNextColumn();
    const float left = ImGui::GetCursorPosX() + check + style.ItemSpacing.x;
    int column = 0;
    for (const char* heading : {"Strength", "Tone", "Structure"})
    {
        const float x = left + column++ * (slider + style.ItemSpacing.x);
        if (column > 1) { ImGui::SameLine(); }
        ImGui::SetCursorPosX(x + (slider - ImGui::CalcTextSize(heading).x) * 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", heading);
    }

    ImGui::BeginDisabled(locked);
    for (unsigned i = 0; i < 4; ++i)
    {
        auto& rule = rules.groups[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The eye shows this group in the mask preview, whether or not it overrides.
        // Eye, swatch and name share one frame-height line, like the controls.
        const float frame = ImGui::GetFrameHeight();
        const float eye = std::round(ImGui::GetTextLineHeight());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14.0f));
        bool shown = (masks.previewGroups >> i) & 1;
        if (ImGui::InvisibleButton("##eye", ImVec2(eye, frame)))
        {
            shown = !shown;
            masks.previewGroups = shown ? (masks.previewGroups | (1u << i)) : (masks.previewGroups & ~(1u << i));
        }
        const bool eyeHovered = ImGui::IsItemHovered();
        if (eyeHovered)
        {
            ImGui::SetTooltip(shown ? "Hide %s in the mask preview." : "Show %s in the mask preview.", names[i]);
        }
        const auto eyeMin = ImGui::GetItemRectMin();
        DrawEye(ImGui::GetWindowDrawList(), ImVec2(eyeMin.x, eyeMin.y + (frame - eye) * 0.5f), eye, shown, eyeHovered);
        ImGui::SameLine(0, style.ItemInnerSpacing.x * 1.5f);
        const float box = std::round(Px(8.0f));
        ImGui::Dummy(ImVec2(box, frame));
        const auto swatch = ImGui::GetItemRectMin();
        const float top = swatch.y + (frame - box) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(swatch.x, top), ImVec2(swatch.x + box, top + box),
            ImGui::GetColorU32(swatches[i]), Px(2.0f));
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::AlignTextToFramePadding();
        // An overriding group's name is amber, as a changed setting is.
        if (rule.enabled) { ImGui::TextColored(kAmber, "%s", names[i]); } else { ImGui::TextUnformatted(names[i]); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", NR::kObjectGroups[i]); }
        ImGui::TableNextColumn();
        ImGui::Checkbox("##override", &rule.enabled);
        DrawSettingsHelp(rule.enabled ? "Overriding this group in this pass." : "Override this group in this pass.");
        ImGui::BeginDisabled(!rule.enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider);
        ImGui::SliderFloat("##strength", &rule.strength, 0, 1, "%.2f");
        DrawSettingsHelp("Strength: 0 keeps this pass's input, 1 its full result.");
        ImGui::BeginDisabled(legacy);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider);
        ImGui::SliderFloat("##tone", &rule.tone, 0, 1, "%.2f");
        DrawSettingsHelp(legacy ? "Tone needs the 310.9.16 runtime; Strength still applies." : "Local tone for this group in this pass.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##structure", &rule.structure, 0, 1, "%.2f");
        DrawSettingsHelp(legacy ? "Structure needs the 310.9.16 runtime; Strength still applies." : "Local structure for this group in this pass.");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(14.0f));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Mask preview");
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!masks.previewGroups);
    ImGui::Checkbox("Classes only", &masks.previewClassesOnly);
    ImGui::EndDisabled();
    DrawSettingsHelp(masks.previewGroups ? "Shows only the chosen groups' colours on black."
                                         : "Open a group's eye to start the preview.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    const auto shownText = masks.previewGroups ? "showing " + ShownGroups(masks.previewGroups, names) + " | not saved"
                                               : std::string("open an eye to preview a group | not saved");
    ImGui::TextDisabled("%s", shownText.c_str());
    DrawSettingsHelp("The preview covers the whole finished image, so it is the same on both pass tabs.");
    ImGui::EndDisabled();
    ImGui::EndTable();

    // What capture is doing now, when it matters.
    const bool inUse = NR::ObjectsInUse(masks, 2);
    if (!view.neuralObjectsAvailable || inUse)
    {
        ImGui::Indent(Px(14.0f));
        ImGui::TextDisabled("%s%s", view.neuralObjectStatus.c_str(),
            !inUse || !view.neuralObjectsAvailable ? ""
            : view.neuralObjectCurrent ? " | classifying this frame" : " | no classification this frame; normal NR");
        ImGui::Unindent(Px(14.0f));
    }
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
            case Kind::Style: text = NeuralStyleName(static_cast<int>(value)); break;
            case Kind::Percent: text = std::format("{:.1f}%", value * 100); break;
            case Kind::Toggle: text = value != 0 ? "On" : "Off"; break;
            case Kind::Network: text = NeuralNetworkName(value != 0 ? 1 : 0); break;
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

enum class PassAction { None, Add, Remove, FollowPass1 };

// The selected pass's folder tab joins the panel below; the others sit behind.
// Its background matches the panel's.
const ImVec4 kPassPanel{0.098f, 0.104f, 0.106f, 1.0f};
const ImVec4 kPassBehind{0.058f, 0.063f, 0.066f, 1.0f};

struct PassPanelGeometry
{
    float tabLeft{}, tabRight{};
    // The reset tab at the strip's right end, open into the panel too; 0 width when absent.
    float resetLeft{}, resetRight{};
};
PassPanelGeometry g_passGeometry;

void FillRect(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color)
{
    draw->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)), color);
}

// Pass tabs, then + to add Pass 2. note is drawn at the right, muted.
// changed[pass] tints a tab's name amber: it differs from Base in a preset, or
// Pass 2 differs from Pass 1.
// How the strip's right end offers "Reset all to Pass 1": not at all (Pass 1),
// greyed while Pass 2 follows Pass 1, or usable.
enum class PassReset { None, Nothing, Available };

PassAction DrawPassTabs(const OverlayUI::FrameView& view, int& selected, int passes, bool editable, const std::string& note,
    const bool (&changed)[2], PassReset reset)
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
    g_passGeometry = {};
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
                                                "memory; weather overrides switch it on and off without restarting NR.");
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
        if (active) { g_passGeometry.tabLeft = x; g_passGeometry.tabRight = x + tabWidth; }
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
    float right = origin.x + available;
    if (reset != PassReset::None)
    {
        // A tab on the page's own background at the strip's right end, open into
        // the panel like the selected tab, so it reads as part of Pass 2's page.
        const char* label = "Reset all to Pass 1";
        const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 3;
        const float left = right - width;
        const float top = origin.y + lift, bottom = origin.y + height;
        const bool usable = editable && reset == PassReset::Available;
        ImGui::SetCursorScreenPos(ImVec2(left, top));
        ImGui::BeginDisabled(!usable);
        if (ImGui::InvisibleButton("resetAll", ImVec2(width, bottom - top))) { action = PassAction::FollowPass1; }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (hovered)
        {
            ImGui::SetTooltip("%s", usable ? "Pass 2 follows Pass 1 again. Its own settings are kept for later."
                                           : "Pass 2 already follows Pass 1; nothing to reset.");
        }
        FillRect(draw, left, top, right, bottom + line, ImGui::GetColorU32(kPassPanel));
        FillRect(draw, left, top, left + line, bottom, border);
        FillRect(draw, left, top, right, top + line, border);
        FillRect(draw, right - line, top, right, bottom, border);
        const auto size = ImGui::CalcTextSize(label);
        draw->AddText(ImVec2(left + (width - size.x) * 0.5f, top + ((bottom - top) - size.y) * 0.5f),
                      ImGui::GetColorU32(!usable ? kMuted : hovered ? kAmber : kIvory), label);
        g_passGeometry.resetLeft = left;
        g_passGeometry.resetRight = right;
        right = left - style.ItemSpacing.x;
    }
    if (!note.empty())
    {
        const auto size = ImGui::CalcTextSize(note.c_str());
        draw->AddText(ImVec2(right - size.x - style.FramePadding.x, origin.y + height - size.y - style.FramePadding.y),
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
    // Open under the reset tab too, when there is one.
    const float end = panel.tab.resetRight > panel.tab.resetLeft ? panel.tab.resetLeft + line : max.x;
    FillRect(draw, (std::min)(max.x, panel.tab.tabRight - line), min.y, (std::min)(max.x, end), min.y + line, border);
}

bool DrawNeuralRuntimeCombo(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                            float width);

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
    const float label = LabelWidth({"Scene normalization"});
    const auto grid = NeuralRowsGrid();
    const GridScope gridScope(grid);
    const auto width = draft.neuralBeforeUpscaling ? view.renderWidth : view.outputWidth;
    const auto height = draft.neuralBeforeUpscaling ? view.renderHeight : view.outputHeight;


    if (BeginSettingRows("nrTop", label))
    {
        // The runtime is not a preset setting, so presets show only the switch.
        const bool base = !ActivePresetDecor();
        const bool lockSwitch = !TheosRenderPipeline::CanEditNeuralEnabled(draft.neuralEnabled, !unavailable);
        SettingRow("Neural Rendering", "NeuralRendering", false, base ? false : lockSwitch, [&] {
            ImGui::BeginDisabled(base && lockSwitch);
            bool changed = ImGui::Checkbox("##v", &draft.neuralEnabled);
            ImGui::EndDisabled();
            if (base)
            {
                const auto& style = ImGui::GetStyle();
                const float mark = ImGui::CalcTextSize("!").x + style.ItemSpacing.x;
                ImGui::SameLine();
                changed |= DrawNeuralRuntimeCombo(view, draft, ImGui::GetContentRegionAvail().x - mark);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kOchre, "!");
                if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Changing the runtime restarts NR briefly."); }
            }
            return changed;
        });
        SettingRow("Placement", "BeforeUpscaling", true, unavailable, [&] {
            int placement = draft.neuralBeforeUpscaling ? 0 : 1;
            const bool changed = Segmented("placement", placement, {
                {"Before upscaling", true, "NR runs on the render-resolution scene, before DLSS or XeSS."},
                {"After upscaling", true, "NR runs on the output-resolution image, after upscaling."}});
            if (changed) { draft.neuralBeforeUpscaling = placement == 0; }
            DrawSettingsHelp("Both passes use the selected side of upscaling. UI is composed afterwards.");
            return changed;
        });
        if (base && g_setCompare)
        {
            // Applies at once and is never saved, so it is not a draft change.
            SettingRow("Compare", nullptr, false, unavailable, [&] {
                auto compare = view.neuralApplied.compare;
                bool changed = ImGui::Checkbox("Split", &compare.split);
                DrawSettingsHelp("Session only, never saved. Left of the line shows the frame without NR.");
                ImGui::SameLine();
                ImGui::BeginDisabled(!compare.split);
                // The slider takes what Freeze frame leaves of the column.
                const auto& style = ImGui::GetStyle();
                const float freezeWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                                          ImGui::CalcTextSize("Freeze frame").x;
                ImGui::SetNextItemWidth((std::max)(Px(48.0f), ImGui::GetContentRegionAvail().x - freezeWidth -
                                                                  style.ItemSpacing.x * 3));
                float percent = compare.splitPosition * 100.0f;
                if (ImGui::SliderFloat("##splitAt", &percent, 0, 100, "%.0f%%"))
                {
                    compare.splitPosition = percent / 100.0f;
                    changed = true;
                }
                ImGui::EndDisabled();
                DrawSettingsHelp("Where the line sits, from the left edge.");
                // Follows the applied placement: the comparison is live, the draft is not.
                const bool before = view.neuralApplied.beforeUpscaling;
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                ImGui::BeginDisabled(before);
                bool freeze = compare.freeze && !before;
                if (ImGui::Checkbox("Freeze frame", &freeze))
                {
                    compare.freeze = freeze;
                    changed = true;
                }
                ImGui::EndDisabled();
                DrawSettingsHelp(before ? "Freeze frame needs After upscaling: before upscaling, DLSS needs a moving frame."
                                        : "Session only. NR keeps working on one held frame while the game runs on, so "
                                          "setting changes compare on identical content. Turn frame generation off for a "
                                          "steady image.");
                if (changed) { g_setCompare(compare); }
                return false;
            });
        }
        ImGui::EndTable();
    }
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
    const bool showingPass2 = selected == 1 && twoPasses;
    // Pass 2 always shows the reset tab; it is greyed while nothing differs.
    const auto reset = !showingPass2 ? PassReset::None
                       : !second.linked && (decor || passDecor.Differences() > 0) ? PassReset::Available
                                                                                   : PassReset::Nothing;
    const auto action = DrawPassTabs(view, selected, draft.neuralPasses, !unavailable,
        decor && decor->EditingTime() ? std::format("editing {}", decor->EditingTime()) : std::string(), tabChanged,
        reset);
    if (action == PassAction::Add) { draft.neuralPasses = 2; selected = 1; }
    if (action == PassAction::Remove) { draft.neuralPasses = 1; selected = 0; }
    const bool pass2 = selected == 1 && draft.neuralPasses == 2;

    // In Base, Pass 2's rows compare with Pass 1 as a preset's compare with Base.
    // A preset keeps its own comparison with Base.
    const bool comparePasses = pass2 && !decor;
    if (comparePasses) { ActivePresetDecor() = &passDecor; }
    const auto panel = BeginPassPanel();
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
    const float passLabel = label;
    if (BeginSettingRows("passLook", passLabel))
    {
        SettingRow("Style", key("Style").c_str(), false, unavailable, [&] {
            // Named styles as in OptiScaler forks. Custom keeps any other number for
            // users and for runtimes that may add styles: clicking it edits the
            // number in place, and it then reads "Custom 7".
            const bool legacy = LegacyNeuralRuntime(view);
            // One editor per override and pass, so a number being typed cannot land in another.
            ImGui::PushID(static_cast<int>(ActiveEditScope()));
            ImGui::PushID(pass2 ? 2 : 1);
            auto* storage = ImGui::GetStateStorage();
            const ImGuiID editId = ImGui::GetID("customEdit"), draftId = ImGui::GetID("customDraft"),
                          frameId = ImGui::GetID("customFrame");
            int editing = storage->GetInt(editId, 0); // 0 closed, 1 focus on this draw, 2 open
            // An edit left when its row stopped drawing (another tab, pass or override, or the
            // menu closed) is dropped rather than applied later.
            if (editing && storage->GetInt(frameId, -1) != ImGui::GetFrameCount() - 1) { editing = 0; }
            storage->SetInt(frameId, ImGui::GetFrameCount());
            const auto customLabel = tuning.style > 2 ? std::format("Custom {}", tuning.style) : std::string("Custom");
            int index = (std::min)(tuning.style, 3);
            SegmentedLayout layout;
            bool changed = Segmented("style", index, {
                {"Default", true, "Style 0."},
                {"Natural", true, "Style 1."},
                {"Cinematic", true, "Style 2."},
                {customLabel.c_str(), !legacy, legacy ? "310.8 runs styles 0 to 2 only."
                    : "Click to type a style number, 3 to 255."}}, &layout);
            if (changed) { tuning.style = index == 3 ? (std::max)(tuning.style, 3) : index; }
            if (layout.clicked == 3 && !legacy) {
                editing = 1;
                storage->SetInt(draftId, (std::max)(tuning.style, 3));
            }
            if (index != 3) { editing = 0; }
            if (editing) {
                // Over the Custom segment, or over the whole combo in a narrow menu.
                ImGui::SetCursorScreenPos(layout.lastMin);
                ImGui::SetNextItemWidth(layout.lastMax.x - layout.lastMin.x);
                if (editing == 1) { ImGui::SetKeyboardFocusHere(); editing = 2; }
                int number = storage->GetInt(draftId, 3);
                // Coloured as the selected segment it replaces.
                const ImVec4 selectedFill(0.23f, 0.18f, 0.08f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, selectedFill);
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::InputInt("##customStyle", &number, 0, 0, ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsDecimal);
                ImGui::PopStyleColor(4);
                storage->SetInt(draftId, number);
                DrawSettingsHelp("Style number, 3 to 255. Press Enter or click away to set it.");
                // Enter or clicking elsewhere sets the number.
                if (ImGui::IsItemDeactivated()) {
                    const int value = std::clamp(number, 3, 255);
                    changed |= value != tuning.style;
                    tuning.style = value;
                    editing = 0;
                }
            }
            storage->SetInt(editId, editing);
            ImGui::PopID();
            ImGui::PopID();
            return changed;
        });
        SettingRow("Intensity", key("Intensity").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.intensity, 0, 2); });
        SettingRow("Local tone", key("LocalTone").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localToneStrength, 0, 2); });
        SettingRow("Local structure", key("LocalStructure").c_str(), false, unavailable,
                   [&] { return ImGui::SliderFloat("##v", &tuning.localStructureStrength, 0, 2); });
        const auto skinKey = key("SkinStructure"), maskKey = key("AutoSkinMask");
        SettingRowFor("Skin structure", {skinKey.c_str(), maskKey.c_str()}, false, unavailable, [&] {
            const char* mask = "Auto mask";
            const float maskWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                    ImGui::CalcTextSize(mask).x;
            ImGui::SetNextItemWidth((std::max)(Px(60.0f), ImGui::GetContentRegionAvail().x - maskWidth -
                                                                 ImGui::GetStyle().ItemSpacing.x));
            // -1 follows local structure; the slider says so.
            bool changed = ImGui::SliderFloat("##v", &tuning.skinStructureStrength, -1, 2,
                                              tuning.skinStructureStrength <= -0.999f ? "Follows local" : "%.3f");
            DrawSettingsHelp("Structure on skin. At -1 it follows Local structure. Ctrl-click to type a value.");
            ImGui::SameLine();
            changed |= ImGui::Checkbox(mask, &tuning.useAutoSkinMask);
            DrawSettingsHelp("Finds skin automatically for Skin structure.");
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (BeginSettingRows("passInput", passLabel))
    {
        auto extent = reconstruction;
        extent.inputScale = scale;
        // The slider shows the model's input size beside the percentage.
        const auto format = width && height ? std::format("%.1f%% | {} x {}", NR::ModelExtent(width, extent),
                                                          NR::ModelExtent(height, extent))
                                            : std::string("%.1f%%");
        SettingRow("Input", key("InputScale").c_str(), true, unavailable, [&] {
            float percent = scale * 100;
            const bool changed = ImGui::SliderFloat("##v", &percent, 25, 100, format.c_str());
            if (changed) { scale = percent / 100; }
            DrawSettingsHelp("NR input resolution, relative to the selected stage's scene size, and the model input it "
                             "gives. Apply to update.");
            return changed;
        });
        SettingRow("Network", key("Network").c_str(), true, unavailable, [&] {
            // 310.8 has only the old model, so it runs Old whichever is saved.
            const bool legacy = LegacyNeuralRuntime(view);
            int index = legacy || network != 0 ? 1 : 0;
            const bool changed = Segmented("network", index, {
                {"Small", !legacy, legacy ? "310.9.16 only. 310.8 always runs the old model."
                                          : "310.9.16's small FP8 model: faster and softer. NVIDIA's "
                                            "CC_SMALL_FP8; Preset B in OptiScaler forks."},
                {"Old", true, "The model 310.8 runs, also included in 310.9.16. NVIDIA's CC_SILVER_AARDWOLD; "
                              "Preset A in OptiScaler forks."}});
            if (changed) { network = index; }
            DrawSettingsHelp("NR network, separate from the DLSS model preset.");
            return changed;
        });
        SettingRowFor("Peripheral compression", {"PeripheralCompression", "CombinedPreparation"}, true, unavailable, [&] {
            bool changed = ImGui::Checkbox("##peripheral", &reconstruction.peripheralCompression);
            DrawSettingsHelp("Peripheral compression preserves sampling density across the central 80% of each axis and "
                             "compresses the edges. Uses about 19% fewer model pixels at the same input resolution. Pass 1 "
                             "compresses the image once and Pass 2 works on it, so it applies to both passes. Inspect "
                             "edge quality when moving.");
            ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
            changed |= ImGui::Checkbox("Combined preparation", &reconstruction.fusedPreparation);
            DrawSettingsHelp("Combines colour encoding with downsampling where needed, and depth/motion packing with "
                             "peripheral compression. Some configurations have no passes to combine. Applies to both "
                             "passes.");
            return changed;
        });
        // UI correction applies only after upscaling without Community Shaders.
        const bool noUICorrection = cs || draft.neuralBeforeUpscaling;
        SettingRow("UI correction", key("UICorrection").c_str(), false, unavailable || noUICorrection, [&] {
            const bool changed = ImGui::Checkbox("##v", &tuning.uiCorrection);
            DrawSettingsHelp(cs ? "NVIDIA's UI correction flag. Asks NR to treat the area around the HUD separately. Only "
                                  "used when NR runs after upscaling. Community Shaders draws the UI after NR, so it is off."
                                : "NVIDIA's UI correction flag. Asks NR to treat the area around the HUD separately. Only "
                                  "used when NR runs after upscaling.");
            return changed;
        });
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::PushID(pass2 ? 2 : 1);
    DrawPassObjects(view, draft.neuralObjectMasks, pass2 ? 1u : 0u, unavailable, decor != nullptr);
    ImGui::PopID();
    if (pass2)
    {
        // When to skip Pass 2. They only ever turn Pass 2 off, so they live in its tab.
        ImGui::Spacing();
        if (BeginSettingRows("passSkip", passLabel))
        {
            SettingRowFor("Skip during", {"OnePassInCombat", "OnePassWeaponsDrawn"}, false, unavailable, [&] {
                const char* help = "Temporarily skips Pass 2 while the condition is active. Keeps your NR resolution, "
                                   "tuning and saved two-pass setting. The image may change when switching.";
                bool changed = ImGui::Checkbox("Combat", &draft.neuralCombat.inCombat);
                DrawSettingsHelp(help);
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2);
                changed |= ImGui::Checkbox("Weapons drawn", &draft.neuralCombat.weaponsDrawn);
                DrawSettingsHelp(help);
                return changed;
            });
            SettingRow("Resume after", "ReturnDelay", false, unavailable || !draft.neuralCombat.Enabled(), [&] {
                const bool changed = ImGui::SliderFloat("##v", &draft.neuralCombat.recoverySeconds, 0.0f, 30.0f, "%.1f s");
                DrawSettingsHelp(draft.neuralCombat.Enabled()
                                     ? "Waits this long after the conditions clear before running Pass 2 again. Pausing "
                                       "the game pauses the delay."
                                     : "Tick Combat or Weapons drawn first.");
                return changed;
            });
            ImGui::EndTable();
        }
    }
    EndPassPanel(panel);
    if (comparePasses) { ActivePresetDecor() = nullptr; }
    if (shown != before)
    {
        second = shown;
        second.linked = false;
    }
    if (action == PassAction::FollowPass1) { second.linked = true; }

    ImGui::Spacing();
    if (BeginSettingRows("sharpening", label))
    {
        SettingRowFor("Sharpening", {"SharpeningEnabled", "Sharpness"}, false, cs, [&] {
            bool changed = ImGui::Checkbox("##enabled", &sharpening);
            DrawSettingsHelp(cs ? "Community Shaders controls sharpening." : "Applies with or without NR.");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(!sharpening);
            changed |= ImGui::SliderFloat("##strength", &sharpness, 0, 1, "%.2f");
            ImGui::EndDisabled();
            DrawSettingsHelp(cs ? "Community Shaders controls sharpening." : "Sharpening strength.");
            return changed;
        });
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (BeginSettingRows("reconstruction", label))
    {
        SettingRow("Reconstruction", "ReconstructionMethod", true, unavailable || producerColor, [&] {
            int method = std::clamp(static_cast<int>(reconstruction.method), 0, 2);
            const bool changed = Segmented("method", method, {
                {"Auto", true, "Reconstructs reduced input."},
                {"Residual", true, "Adds NR's changes; clamps colour to 0..1."},
                {"Ratio", true, "Transfers lighting and colour; use for linear HDR input."}});
            if (changed) { reconstruction.method = static_cast<NR::ResolveMethod>(method); }
            DrawSettingsHelp(producerColor ? "CS restores scene colour automatically before DLSS."
                                           : "Residual clamps colour to 0..1. Use Ratio for linear HDR input.");
            return changed;
        });
        // Ratio's settings stay visible, greyed, until Ratio is selected.
        const bool ratioOff = unavailable || (!producerColor && reconstruction.method != NR::ResolveMethod::Ratio);
        const char* ratioHelp = producerColor ? "" : " Used by Ratio reconstruction.";
        SettingRowFor("Effect \xC2\xB7 colour", {"EffectStrength", "ColourStrength"}, false, ratioOff, [&] {
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            ImGui::SetNextItemWidth(half);
            bool changed = ImGui::SliderFloat("##effect", &reconstruction.transferStrength, 0, 2);
            DrawSettingsHelp(std::format("{}{}", producerColor ? "Effect strength." : "Ratio effect strength.", ratioHelp).c_str());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::SliderFloat("##colour", &reconstruction.colourStrength, 0, 2);
            DrawSettingsHelp(std::format("{}{}", producerColor ? "Colour strength." : "Ratio colour strength.", ratioHelp).c_str());
            return changed;
        });
        SettingRow(producerColor ? "Maximum scene gain" : "Maximum luma ratio", "MaximumRatio", false, ratioOff,
                   [&] { return ImGui::SliderFloat("##v", &reconstruction.maxRatio, producerColor ? 1.0f : 0.01f, 16); });
        if (!producerColor)
        {
            SettingRow("HDR input", "InputHDR", true, ratioOff, [&] {
                const bool changed = ImGui::Checkbox("##hdr", &reconstruction.colorIsHDR);
                DrawSettingsHelp("Input colour is linear HDR. Used by Ratio reconstruction; it does not change "
                                 "the display HDR mode.");
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("linear HDR");
                return changed;
            });
            // How linear HDR is scaled for NR's view. Ratio's result is applied to
            // the untouched frame, so neither choice changes its brightness.
            const bool exposureOff = ratioOff || !reconstruction.colorIsHDR;
            SettingRow("Exposure", "AdaptiveExposure", false, exposureOff, [&] {
                int mode = reconstruction.adaptiveExposure ? 1 : 0;
                bool changed = Segmented("exposure", mode, {
                    {"Fixed", true, "Divides linear HDR input by the white point before NR sees it."},
                    {"Adaptive", true, "TRP measures the frame's average brightness and adapts over about a second, so "
                                       "NR sees interiors and exteriors at a similar brightness. Only NR's view "
                                       "changes; its effect is carried back onto the untouched frame."}});
                if (changed) { reconstruction.adaptiveExposure = mode == 1; }
                DrawSettingsHelp(exposureOff ? "Used by Ratio reconstruction with linear HDR input."
                                             : "How linear HDR input is scaled for NR's view.");
                return changed;
            });
            // The value row follows the choice: a fixed white point or an EV bias.
            if (reconstruction.adaptiveExposure)
            {
                SettingRow("Bias", "ExposureBias", false, exposureOff, [&] {
                    const bool changed = ImGui::SliderFloat("##bias", &reconstruction.exposureBias, -2, 2, "%+.1f EV");
                    DrawSettingsHelp(exposureOff ? "Used by Ratio reconstruction with linear HDR input."
                                                 : "Brightens NR's view when positive, darkens it when negative.");
                    return changed;
                });
            }
            else
            {
                SettingRow("White point", "WhitePoint", false, exposureOff, [&] {
                    const bool changed = ImGui::InputFloat("##white", &reconstruction.whitePoint, 0, 0, "%.4f");
                    DrawSettingsHelp(exposureOff ? "Used by Ratio reconstruction with linear HDR input."
                                                 : "White point for encoding linear HDR input before model evaluation.");
                    return changed;
                });
            }
        }
        else SettingRowFor("Scene normalization", {"InputHDR", "WhitePoint"}, true, ratioOff, [&] {
            ImGui::BeginDisabled(producerColor);
            bool changed = ImGui::Checkbox("##hdr", &reconstruction.colorIsHDR);
            ImGui::EndDisabled();
            DrawSettingsHelp(producerColor ? "Community Shaders supplies scene colour; the input is not switched here."
                                           : "Input colour is linear HDR. Used by Ratio reconstruction; it does not change "
                                             "the display HDR mode.");
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("white point");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            changed |= ImGui::InputFloat("##white", &reconstruction.whitePoint, 0, 0, "%.4f");
            DrawSettingsHelp(producerColor ? "Scene normalization sets the brightness range NR sees."
                                           : "White point for encoding linear HDR input before model evaluation.");
            return changed;
        });
        ImGui::EndTable();
    }
}

// Which nvngx_dlssnr*.dll NR loads, after the Neural Rendering checkbox. It stays
// editable while NR is unavailable, so a missing runtime can be swapped for an
// installed one; Apply switches it. While Automatic runs, the closed dropdown
// also names the runtime it chose.
bool DrawNeuralRuntimeCombo(const OverlayUI::FrameView& view, TheosRenderPipeline::SourceDLSSG::Preferences& draft,
                            float width)
{
    using TheosRenderPipeline::SourceDLSSG::NeuralRuntimeLabel;
    const auto& applied = view.neuralApplied;
    const bool automatic = TheosRenderPipeline::SourceDLSSG::IsAutomaticNeuralRuntime(draft.neuralRuntime);
    const auto chosen = automatic && applied.automaticRuntime && applied.enabled
                            ? NeuralRuntimeLabel(applied.runtimePath.string())
                            : std::string();
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##runtime", nullptr, ImGuiComboFlags_CustomPreview))
    {
        for (const auto& runtime : view.neuralRuntimes)
        {
            ImGui::PushID(runtime.c_str());
            const bool selected = runtime == draft.neuralRuntime;
            if (ImGui::Selectable(NeuralRuntimeLabel(runtime).c_str(), selected) && !selected)
            {
                draft.neuralRuntime = runtime;
                changed = true;
            }
            if (selected) { ImGui::SetItemDefaultFocus(); }
            if (ImGui::IsItemHovered()) { ImGui::SetTooltip("File: %s", runtime.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto help = std::format("{}NR runtime. Automatic chooses the runtime for your GPU. Universal unlocks NVIDIA "
        "310.9.16 on RTX 20/30/40; RTX 50 uses it natively. The kernel pack uses FP16 fallback on RTX 20/30 and FP8 on "
        "RTX 40. Legacy INT8 is an RTX 20/30 alternative. Hover an entry for its filename.",
        chosen.empty() ? std::string() : std::format("Automatic is using {}.\n\n", chosen));
    DrawSettingsHelp(help.c_str());
    if (ImGui::BeginComboPreview())
    {
        // One line, clipped with an ellipsis before the arrow.
        const auto label = NeuralRuntimeLabel(draft.neuralRuntime);
        const auto detail = chosen.empty() ? std::string() : "  |  " + chosen;
        const auto start = ImGui::GetCursorScreenPos();
        const float right = ImGui::GetItemRectMax().x - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x;
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        const ImVec2 end(right, start.y + ImGui::GetTextLineHeight());
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), start, end, right, right, label.c_str(), nullptr, nullptr);
        if (!detail.empty() && start.x + labelWidth < right)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(start.x + labelWidth, start.y), end, right, right,
                                      detail.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
        ImGui::EndComboPreview();
    }
    return changed;
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
        else {
            DrawBaseSettings(view, [&] {
                g_setCompare = [this](auto compare) { SetNeuralCompare(compare); };
                DrawSourceNeuralControls(view, settingsDraft.sourceDLSSG, settingsDraft.sharpening, settingsDraft.sharpness,
                                         settingsDraft.upscaleType, nrRuntimePresent);
                g_setCompare = nullptr;
            });
        }
        EndSettingsColumns();
    }
    ImGui::EndTabItem();
}
