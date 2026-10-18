#include <PCH.h>
#include "NeuralObjectHooks.h"
#include "HookDetour.h"
#include "FrameGen/NvidiaHost.h"
#include <array>

namespace TheosRenderPipeline::NeuralObjects
{
    namespace
    {
        using Group = NeuralRendering::ObjectGroup;
        // The actual 1.6.1170 ABI is (pass, technique, alphaTest, flags).
        // It is a static draw helper, despite the member declaration in CommonLib.
        using DrawPass = void(*)(RE::BSRenderPass*, std::uint32_t, bool, std::uint32_t);
        DrawPass original{};
        std::atomic<bool> enabled{};
        thread_local Group currentGroup{};
        NeuralRendering::ObjectCapture capture;
        std::array<bool,4> loggedGroups{};
        std::atomic<const char*> status{"Object capture is unavailable on this Skyrim version"};

        Group Classify(RE::BSGeometry* geometry)
        {
            if (!geometry) { return Group::World; }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) { return Group::World; }
            // The pass owns live geometry. Retain the starting node and inspect
            // a bounded ancestry; do not follow unrelated cached scene pointers.
            RE::NiPointer<RE::NiAVObject> retained{geometry};
            RE::NiPointer<RE::NiAVObject> firstPerson{player->Get3D(true)};
            RE::TESObjectREFR* owner{};
            std::array<RE::NiAVObject*, 64> seen{};
            auto* node = retained.get();
            for (unsigned depth = 0; node && depth < seen.size(); ++depth) {
                if (std::find(seen.begin(), seen.begin()+depth, node) != seen.begin()+depth) { return Group::World; }
                seen[depth] = node;
                if (firstPerson && node == firstPerson.get()) { return Group::FirstPerson; }
                if (!owner && node->userData) { owner = node->userData; }
                node = node->parent;
            }
            if (!owner) { return Group::World; }
            if (owner == player) { return Group::Player; }
            RE::NiPointer<RE::TESObjectREFR> retainedOwner{owner};
            auto* actor = retainedOwner->As<RE::Actor>();
            if (!actor) { return Group::World; }
            auto* race = actor->GetRace();
            return race && race->HasKeywordString("ActorTypeNPC") ? Group::Humanoid : Group::Creature;
        }
        void Pass(RE::BSRenderPass* pass, std::uint32_t technique, bool alphaTest, std::uint32_t flags)
        {
            const auto previous = currentGroup;
            currentGroup = enabled.load(std::memory_order_relaxed) && pass && pass->shader && pass->shader->shaderType == RE::BSShader::Type::Lighting && !alphaTest ? Classify(pass->geometry) : Group::World;
            original(pass, technique, alphaTest, flags);
            currentGroup = previous;
        }
    }
    void Install()
    {
        if (original || REL::Module::get().version() != REL::Version{1,6,1170,0}) { return; }
        const auto site = REL::ID(107644).address();
        // Verified in the retained, statically unpacked executable. Recheck live
        // bytes before patching, and decline competing entry patches safely.
        constexpr std::array<std::uint8_t, 19> prefix{0x48,0x8b,0xc4,0x57,0x41,0x54,0x41,0x55,
            0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00};
        if (!HookSafety::Bytes(site, prefix)) {
            status.store("Object capture hook conflicts with another patch; normal NR retained");
            logger::warn("[NR Objects] draw-helper prologue mismatch; capture unavailable"); return;
        }
        original = reinterpret_cast<DrawPass>(HookSafety::InstallEntryDetour(site, reinterpret_cast<std::uintptr_t>(&Pass)));
        status.store(original ? "Experimental opaque-object capture ready" : "Object capture hook unavailable; normal NR retained");
    }
    void Configure(bool value)
    {
        // Settings are applied outside the game's scene-draw scope. Leave the
        // engine entry point untouched until this feature is explicitly enabled.
        if (value && !original) { Install(); }
        enabled.store(value && original, std::memory_order_relaxed);
    }
    bool Active() { return enabled.load(std::memory_order_relaxed); }
    void ResetAfterRetirement()
    {
        capture.Reset(); loggedGroups = {};
    }
    void RecordDraw(ID3D11DeviceContext* context, const std::function<void()>& draw)
    {
        if (!enabled.load(std::memory_order_relaxed) || currentGroup == Group::World) { return; }
        auto* host = NvidiaHost::GetSingleton();
        if (!host->UpscalerReady() || !host->SourceContext(context) || host->NativeUIPassActive()) { return; }
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) { return; }
        const auto& targets = renderer->GetDepthStencilData().depthStencils;
        const auto hr = capture.Record(context, currentGroup, host->PresentCount(), {host->RenderWidth(),host->RenderHeight()},
            reinterpret_cast<ID3D11Resource*>(targets[0].texture), reinterpret_cast<ID3D11Resource*>(targets[7].texture), draw);
        const auto group = static_cast<unsigned>(currentGroup)-1;
        if (hr == S_OK && group < loggedGroups.size() && !loggedGroups[group]) {
            loggedGroups[group] = true;
            logger::info("[NR Objects] captured opaque draw group={} frame={}", NeuralRendering::kObjectGroups[group], host->PresentCount());
        }
        if (FAILED(hr)) {
            enabled.store(false); status.store("Object capture failed; normal NR retained until Apply");
            logger::warn("[NR Objects] capture failed hr=0x{:08X}; normal NR retained", static_cast<unsigned>(hr));
        }
    }
    ID3D11Texture2D* Current()
    {
        return enabled.load(std::memory_order_relaxed) ? capture.Current(NvidiaHost::GetSingleton()->PresentCount()) : nullptr;
    }
    const char* Status() { return status.load(); }
}
