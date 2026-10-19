#pragma once

#include "OverlayPresetDecor.h"
#include "OverlayUIStyle.h"
#include <algorithm>
#include <cfloat>
#include <format>
#include <initializer_list>
#include <string>

namespace TheosRenderPipeline::Overlay
{
// Lays out items left to right, wrapping when the next one would not fit.
class Flow
{
public:
    Flow() : right_(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x) {}
    void Next(float width)
    {
        if (!first_ && lastEnd_ + ImGui::GetStyle().ItemSpacing.x + width <= right_) { ImGui::SameLine(); }
        first_ = false;
        lastEnd_ = ImGui::GetCursorPosX() + width;
    }
    bool Checkbox(const char* label, bool* value)
    {
        Next(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x);
        return ImGui::Checkbox(label, value);
    }
    bool Button(const char* label)
    {
        Next(ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2);
        return ImGui::Button(label);
    }
    void Text(const char* text)
    {
        Next(ImGui::CalcTextSize(text).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
    }
private:
    float right_, lastEnd_{};
    bool first_{true};
};

inline float LabelWidth(std::initializer_list<const char*> labels)
{
    float width = 0;
    for (const auto* label : labels) { width = (std::max)(width, ImGui::CalcTextSize(label).x); }
    return width + ImGui::GetStyle().ItemSpacing.x * 3;
}
inline ImU32 ChangedTint() { return ImGui::GetColorU32(ImVec4(kAmber.x, kAmber.y, kAmber.z, 0.13f)); }

namespace Rows
{
// Widths in 1x units; callers multiply by the zoom.
inline constexpr float MaxControl = 520, MinControl = 140;
// The state column holds one undo button, a frame high and square.
inline float StateColumn() { return ImGui::GetFrameHeight(); }
// One control column shared by every row table drawn while it is active, so
// controls start and end at the same x inside and outside the pass panel.
struct Grid
{
    float controlX{}, controlWidth{};
};
inline const Grid*& ActiveGrid()
{
    static const Grid* grid{};
    return grid;
}
// Screen x of a table column's content, for a table starting at tableX.
inline float ColumnStart(float tableX, float labelWidth)
{
    return tableX + labelWidth + ImGui::GetStyle().CellPadding.x * 3;
}
inline void Label(const char* label, bool changed, bool restarts)
{
    ImGui::AlignTextToFramePadding();
    if (changed) { ImGui::TextColored(kAmber, "%s", label); } else { ImGui::TextUnformatted(label); }
    if (restarts) {
        ImGui::SameLine(0, Px(5.0f));
        ImGui::TextColored(kOchre, "!");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", RestartsNRHelp); }
    }
}
}

// Label left, control right. With a decor (a preset, or Pass 2 compared with
// Pass 1) a narrow state column holds Reset on rows that differ.
inline bool BeginSettingRows(const char* id, float labelWidth)
{
    const float available = ImGui::GetContentRegionAvail().x;
    const float state = ActivePresetDecor() ? Rows::StateColumn() : 0;
    float control = std::clamp(available - labelWidth - state - ImGui::GetStyle().CellPadding.x * 6,
        Px(Rows::MinControl), Px(Rows::MaxControl));
    if (const auto* grid = Rows::ActiveGrid())
    {
        // The label column takes up whatever lies before the shared control column.
        const float tableX = ImGui::GetCursorScreenPos().x;
        labelWidth = (std::max)(Px(20.0f), labelWidth + grid->controlX - Rows::ColumnStart(tableX, labelWidth));
        control = grid->controlWidth;
    }
    if (!ImGui::BeginTable(id, ActivePresetDecor() ? 3 : 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX)) { return false; }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
    ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, control);
    if (ActivePresetDecor()) { ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch); }
    return true;
}
// Whether any of keys is changed by the preset being edited.
inline bool AnyChanged(std::initializer_list<const char*> keys)
{
    const auto* decor = ActivePresetDecor();
    return decor && std::ranges::any_of(keys, [&](const char* key) { return key && decor->Changed(key); });
}
// A square button with an undo arrow, a frame high. Returns true when clicked.
inline bool UndoButton(const char* id)
{
    const float size = ImGui::GetFrameHeight();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const auto min = ImGui::GetItemRectMin();
    auto* draw = ImGui::GetWindowDrawList();
    const auto& style = ImGui::GetStyle();
    draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size),
        ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), style.FrameRounding);
    draw->AddRect(min, ImVec2(min.x + size, min.y + size), ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding);
    // The arrow in 16-unit space: a hook running right, round and back, with its head at the top left.
    const float icon = size * 0.56f, unit = icon / 16.0f;
    const ImVec2 origin(min.x + (size - icon) * 0.5f, min.y + (size - icon) * 0.5f);
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * unit, origin.y + y * unit); };
    const auto colour = ImGui::GetColorU32(hovered ? kIvory : kAmber);
    const float stroke = (std::max)(1.0f, Px(1.6f));
    draw->PathLineTo(at(5.2f, 4.2f));
    draw->PathLineTo(at(10.0f, 4.2f));
    draw->PathArcTo(at(10.0f, 7.8f), 3.6f * unit, -1.5707964f, 1.5707964f);
    draw->PathLineTo(at(5.0f, 11.4f));
    draw->PathStroke(colour, 0, stroke);
    draw->PathLineTo(at(7.4f, 1.6f));
    draw->PathLineTo(at(4.6f, 4.2f));
    draw->PathLineTo(at(7.4f, 6.8f));
    draw->PathStroke(colour, 0, stroke);
    return clicked;
}
// The state cell for one row: empty, or an undo button when it differs. The
// tooltip gives the value it restores.
inline void StateCell(std::initializer_list<const char*> keys)
{
    auto* decor = ActivePresetDecor();
    if (!decor) { return; }
    ImGui::TableNextColumn();
    std::string base;
    for (const auto* key : keys) {
        if (key && decor->Changed(key)) { base += std::format("{}{}", base.empty() ? "" : ", ", decor->BaseValue(key)); }
    }
    if (base.empty()) { return; }
    // Rows disable only their label and control, so Reset stays usable.
    ImGui::PushID(*keys.begin());
    if (UndoButton("##reset")) {
        for (const auto* key : keys) { if (key && decor->Changed(key)) { decor->Reset(key); } }
    }
    ImGui::PopID();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Was %s in %s. Click to reset.", base.c_str(), decor->Reference()); }
}
// One row for the settings in keys; Reset restores all of them that differ.
// disabled greys the label and control that cannot apply now; the state cell stays usable.
template <class Control>
bool SettingRowFor(const char* label, std::initializer_list<const char*> keys, bool restarts, bool disabled,
    Control&& control)
{
    const bool changed = AnyChanged(keys);
    ImGui::TableNextRow();
    if (changed) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ChangedTint()); }
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(disabled);
    Rows::Label(label, changed, restarts);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    const bool result = control();
    ImGui::PopID();
    ImGui::EndDisabled();
    StateCell(keys);
    return result;
}
template <class Control>
bool SettingRow(const char* label, const char* key, bool restarts, bool disabled, Control&& control)
{
    return SettingRowFor(label, {key}, restarts, disabled, std::forward<Control>(control));
}

// The Neural Rendering settings' shared column, from the current cursor: room for
// the longest label outside the pass panel, or inside it after the panel's
// padding, and always room for a Reset column on the right.
inline Rows::Grid NeuralRowsGrid()
{
    const auto& style = ImGui::GetStyle();
    const float origin = ImGui::GetCursorScreenPos().x;
    const float available = ImGui::GetContentRegionAvail().x;
    const float inset = style.WindowPadding.x;
    const float outside = LabelWidth({"Object-specific NR", "Scene normalization", "Maximum scene gain",
                                      "Maximum luma ratio", "Neural Rendering", "Reconstruction"});
    const float inside = LabelWidth({"Peripheral compression", "Local structure", "UI correction", "Skin structure"});
    Rows::Grid grid;
    grid.controlX = Rows::ColumnStart(origin, (std::max)(outside, inset + inside));
    // Controls fill the column up to the undo button, which ends at the pass
    // panel's inner edge; there is no maximum width, so no gap opens beyond it.
    const float right = origin + available - inset - Rows::StateColumn() - style.CellPadding.x * 3;
    grid.controlWidth = (std::max)(Px(Rows::MinControl), right - grid.controlX);
    return grid;
}
// Makes grid the active column for its lifetime.
struct GridScope
{
    const Rows::Grid* previous;
    explicit GridScope(const Rows::Grid& grid) : previous(Rows::ActiveGrid()) { Rows::ActiveGrid() = &grid; }
    ~GridScope() { Rows::ActiveGrid() = previous; }
    GridScope(const GridScope&) = delete;
    GridScope& operator=(const GridScope&) = delete;
};

}
