#pragma once

#include <string>

namespace TheosRenderPipeline::Overlay
{
// The preset editor draws the Neural Rendering controls with per-setting state,
// compared with Base; Pass 2 is compared with Pass 1 the same way. Drawing with
// no decor shows no state column.
class PresetDecor
{
public:
    virtual ~PresetDecor() = default;
    virtual bool Changed(const char* key) const = 0;
    virtual std::string BaseValue(const char* key) const = 0;
    virtual void Reset(const char* key) = 0;
    // The time of day being edited, or null when the preset looks the same all day.
    virtual const char* EditingTime() const = 0;
    // What unchanged rows follow, as the state column names it.
    virtual const char* Reference() const { return "Base"; }
};
inline PresetDecor*& ActivePresetDecor()
{
    static PresetDecor* decor{};
    return decor;
}
inline constexpr const char* RestartsNRHelp = "Changing this restarts NR briefly. A preset that changes it can hitch "
                                              "when the weather changes.";
}
