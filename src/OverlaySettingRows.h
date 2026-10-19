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
// The screen x range of the settings table being drawn, for full-width tints.
inline float& TableLeft() { static float x{}; return x; }
inline float& TableRight() { static float x{}; return x; }
// Tints the current table row across the whole window, edge to edge. The table's
// own row background covers its width, under the cells; strips extend it to the
// window's edges on either side. Call right after TableNextRow.
inline void TintRowFullWidth(ImU32 colour)
{
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, colour);
    const auto& style = ImGui::GetStyle();
    const auto pos = ImGui::GetWindowPos();
    const float left = pos.x + ImGui::GetWindowContentRegionMin().x - style.WindowPadding.x;
    const float right = pos.x + ImGui::GetWindowContentRegionMax().x + style.WindowPadding.x;
    const float top = ImGui::GetCursorScreenPos().y;
    const float bottom = top + ImGui::GetFrameHeight() + style.CellPadding.y * 2;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(left, top), ImVec2(right, bottom), false);
    if (TableLeft() > left) { draw->AddRectFilled(ImVec2(left, top), ImVec2(TableLeft(), bottom), colour); }
    if (TableRight() < right) { draw->AddRectFilled(ImVec2(TableRight(), top), ImVec2(right, bottom), colour); }
    draw->PopClipRect();
}

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
    TableLeft() = ImGui::GetCursorScreenPos().x;
    TableRight() = TableLeft() + available;
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
// One option of a segmented control. A disabled option shows greyed, with
// reason as its tooltip.
struct Segment
{
    const char* label;
    bool enabled{true};
    const char* reason{nullptr};
};
// A row of joined buttons for a short, fixed set of options; the selected one is
// amber. Fills the next item width. When the labels do not fit it falls back to
// a combo with the same options. Returns true when the selection changes.
inline bool Segmented(const char* id, int& selected, std::initializer_list<Segment> segments)
{
    const auto& style = ImGui::GetStyle();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const auto count = static_cast<int>(segments.size());
    float needed = 0;
    for (const auto& segment : segments) { needed += ImGui::CalcTextSize(segment.label).x + style.FramePadding.x * 2; }
    bool changed = false;
    ImGui::PushID(id);
    if (needed > width || count == 0)
    {
        // Too narrow: the same choice as a combo.
        const auto* current = selected >= 0 && selected < count ? (segments.begin() + selected)->label : "";
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("##segments", current))
        {
            int index = 0;
            for (const auto& segment : segments)
            {
                ImGui::BeginDisabled(!segment.enabled);
                if (ImGui::Selectable(segment.label, index == selected) && index != selected) { selected = index; changed = true; }
                ImGui::EndDisabled();
                if (segment.reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", segment.reason); }
                ++index;
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }
    // Widths share the spare room equally, so short labels do not look cramped.
    const float spare = (width - needed) / count;
    auto* draw = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto border = ImGui::GetColorU32(ImGuiCol_Border);
    float x = origin.x;
    int index = 0;
    for (const auto& segment : segments)
    {
        const float w = index + 1 == count ? origin.x + width - x
                                           : std::round(ImGui::CalcTextSize(segment.label).x + style.FramePadding.x * 2 + spare);
        ImGui::SetCursorScreenPos(ImVec2(x, origin.y));
        ImGui::PushID(index);
        ImGui::BeginDisabled(!segment.enabled);
        if (ImGui::InvisibleButton("##segment", ImVec2(w, height)) && index != selected) { selected = index; changed = true; }
        ImGui::EndDisabled();
        const bool hovered = ImGui::IsItemHovered();
        if (segment.reason && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { ImGui::SetTooltip("%s", segment.reason); }
        const bool on = index == selected;
        // Unavailable options sit darker than the rest, so they read as out of reach.
        const auto fill = on ? ImGui::GetColorU32(ImVec4(0.23f, 0.18f, 0.08f, 1.0f))
                        : !segment.enabled ? ImGui::GetColorU32(ImVec4(0.07f, 0.075f, 0.078f, 1.0f))
                        : hovered ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg);
        draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + w, origin.y + height), fill);
        if (index > 0) { draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), border); }
        const auto size = ImGui::CalcTextSize(segment.label);
        // A disabled row already dims everything drawn; a disabled option uses the disabled text colour.
        const auto colour = !segment.enabled ? ImGui::GetColorU32(ImVec4(kMuted.x * 0.7f, kMuted.y * 0.7f, kMuted.z * 0.7f, 1.0f))
                          : on ? ImGui::GetColorU32(kAmber) : ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddText(ImVec2(x + (w - size.x) * 0.5f, origin.y + (height - size.y) * 0.5f), colour, segment.label);
        ImGui::PopID();
        x += w;
        ++index;
    }
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), border, style.FrameRounding);
    // Register the whole control as one item for layout and SameLine.
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
    return changed;
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
    if (changed) { TintRowFullWidth(ChangedTint()); }
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
