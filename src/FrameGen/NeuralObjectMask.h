#pragma once
#include "NeuralRenderingPassSettings.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace TheosRenderPipeline::NeuralRendering
{
    enum class ObjectGroup : unsigned { World, FirstPerson, Player, Humanoid, Creature };
    struct ObjectGroupTuning
    {
        bool enabled{};
        float tone{1}, structure{1};
        float strength{1}; // Blend this pass's result with its own input, not the original scene.
        bool operator==(const ObjectGroupTuning&) const = default;
    };
    struct ObjectPassTuning
    {
        std::array<ObjectGroupTuning,4> groups{};
        bool Active() const { return std::any_of(groups.begin(),groups.end(),[](const auto& g){return g.enabled;}); }
        std::array<float,4> Strengths() const
        {
            std::array<float,4> result{1,1,1,1};
            for (unsigned i=0;i<groups.size();++i) { if (groups[i].enabled) { result[i]=groups[i].strength; } }
            return result;
        }
        bool operator==(const ObjectPassTuning&) const = default;
    };

    // Object rules live in each NR pass; nothing applies to the chain as a whole.
    // Unknown/unavailable classification preserves normal NR behavior and no
    // inference is skipped.
    struct ObjectMaskSettings
    {
        std::array<ObjectPassTuning,kMaxNeuralPasses> passes{};
        // Session-only mask preview: bit g-1 shows group g; no bits is off.
        // Classes only blacks out everything that is not shown.
        unsigned previewGroups{};
        bool previewClassesOnly{};
        bool operator==(const ObjectMaskSettings&) const = default;
    };
    inline ObjectMaskSettings SanitizeObjectMask(ObjectMaskSettings value)
    {
        for (auto& pass : value.passes) for (auto& group : pass.groups) {
            group.strength = std::isfinite(group.strength) ? std::clamp(group.strength,0.0f,1.0f) : 1.0f;
            group.tone = std::isfinite(group.tone) ? std::clamp(group.tone,0.0f,1.0f) : 1.0f;
            group.structure = std::isfinite(group.structure) ? std::clamp(group.structure,0.0f,1.0f) : 1.0f;
        }
        value.previewGroups &= 0xFu;
        return value;
    }
    // The captured classification is one R32_FLOAT per pixel: the object's depth
    // with its three lowest mantissa bits replaced by the group. D3D11 cannot
    // open two-channel 32-bit textures shared with D3D12, and the blend needs
    // full depth precision; this costs at most 7 ulp (under 1e-6 relative).
    // The capture and blend shaders use the same layout.
    inline float PackObjectPixel(ObjectGroup group, float depth)
    {
        return std::bit_cast<float>((std::bit_cast<std::uint32_t>(depth) & ~7u) | (static_cast<unsigned>(group) & 7u));
    }
    inline ObjectGroup PackedObjectGroup(float value) { return static_cast<ObjectGroup>(std::bit_cast<std::uint32_t>(value) & 7u); }
    inline float PackedObjectDepth(float value) { return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & ~7u); }
    // Classification is captured while a running pass overrides a group or the
    // mask preview is on. passes is the number of passes that run.
    inline bool ObjectsInUse(const ObjectMaskSettings& settings, int passes)
    {
        const auto count = static_cast<std::size_t>(std::clamp(passes, 0, kMaxNeuralPasses));
        return settings.previewGroups != 0 ||
               std::any_of(settings.passes.begin(), settings.passes.begin() + count, [](const auto& pass) { return pass.Active(); });
    }
    inline constexpr const char* kObjectGroups[]{"First-person arms / weapons", "Player body / equipment",
        "Other humanoids / equipment", "Creatures"};
    template<class Ini> ObjectMaskSettings LoadObjectMask(const Ini& ini, const char* section)
    {
        ObjectMaskSettings value;
        for (unsigned pass=0;pass<kMaxNeuralPasses;++pass) for (unsigned group=0;group<4;++group) {
            auto prefix=std::string("NRObjectPass")+std::to_string(pass+1)+"Group"+std::to_string(group+1);
            auto& rule=value.passes[pass].groups[group];
            rule.enabled=ini.GetBoolValue(section,(prefix+"Enabled").c_str(),false);
            rule.strength=static_cast<float>(ini.GetDoubleValue(section,(prefix+"Strength").c_str(),1));
            rule.tone=static_cast<float>(ini.GetDoubleValue(section,(prefix+"Tone").c_str(),1));
            rule.structure=static_cast<float>(ini.GetDoubleValue(section,(prefix+"Structure").c_str(),1));
        }
        // Diagnostic preview is session-only; loading settings always clears it.
        return SanitizeObjectMask(value);
    }
    template<class Ini> void StoreObjectMask(Ini& ini, const char* section, ObjectMaskSettings value)
    {
        value = SanitizeObjectMask(value);
        // Released whole-chain keys (NRObjectMasks, NR*Strength) are no longer read or written.
        for (unsigned pass=0;pass<kMaxNeuralPasses;++pass) for (unsigned group=0;group<4;++group) {
            auto prefix=std::string("NRObjectPass")+std::to_string(pass+1)+"Group"+std::to_string(group+1);
            const auto& rule=value.passes[pass].groups[group];
            ini.SetBoolValue(section,(prefix+"Enabled").c_str(),rule.enabled);
            ini.SetDoubleValue(section,(prefix+"Strength").c_str(),rule.strength);
            ini.SetDoubleValue(section,(prefix+"Tone").c_str(),rule.tone);
            ini.SetDoubleValue(section,(prefix+"Structure").c_str(),rule.structure);
        }
    }
}
