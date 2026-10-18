#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace TheosRenderPipeline::NeuralRendering
{
    enum class ObjectGroup : unsigned { World, FirstPerson, Player, Humanoid, Creature };

    // Source-owned composition of the complete NR result. No inference is skipped.
    // Unknown/unavailable classification preserves normal NR behavior.
    struct ObjectMaskSettings
    {
        bool enabled{};
        std::array<float, 4> strength{0, 0, 0, 0};
        bool operator==(const ObjectMaskSettings&) const = default;
    };
    inline ObjectMaskSettings SanitizeObjectMask(ObjectMaskSettings value)
    {
        for (auto& strength : value.strength) {
            strength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 1.0f;
        }
        return value;
    }
    inline float ObjectStrength(const ObjectMaskSettings& settings, ObjectGroup group)
    {
        const auto index = static_cast<unsigned>(group);
        return settings.enabled && index >= 1 && index <= 4 ? settings.strength[index - 1] : 1.0f;
    }
    inline constexpr const char* kObjectGroups[]{"First-person arms / weapons", "Player body / equipment",
        "Other humanoids / equipment", "Creatures"};
    inline constexpr const char* kObjectMaskKeys[]{"NRFirstPersonStrength", "NRPlayerStrength",
        "NRHumanoidStrength", "NRCreatureStrength"};
    template<class Ini> ObjectMaskSettings LoadObjectMask(const Ini& ini, const char* section)
    {
        ObjectMaskSettings value;
        value.enabled = ini.GetBoolValue(section, "NRObjectMasks", false);
        for (unsigned i = 0; i < 4; ++i) {
            value.strength[i] = static_cast<float>(ini.GetDoubleValue(section, kObjectMaskKeys[i], 0));
        }
        return SanitizeObjectMask(value);
    }
    template<class Ini> void StoreObjectMask(Ini& ini, const char* section, ObjectMaskSettings value)
    {
        value = SanitizeObjectMask(value);
        ini.SetBoolValue(section, "NRObjectMasks", value.enabled);
        for (unsigned i = 0; i < 4; ++i) { ini.SetDoubleValue(section, kObjectMaskKeys[i], value.strength[i]); }
    }
}
