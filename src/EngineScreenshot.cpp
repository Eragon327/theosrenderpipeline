#include <PCH.h>
#include "EngineScreenshot.h"
#include "HookDetour.h"

#include <array>
#include <deque>
#include <mutex>

namespace TheosRenderPipeline::EngineScreenshot
{
    namespace
    {
        using Format = RE::BSGraphics::TextureFileFormat;
        using Save = void (*)(RE::BSGraphics::Renderer*, RE::RENDER_TARGETS::RENDER_TARGET, const char*, Format);
        Save g_original{};
        std::mutex g_mutex;
        std::deque<Request> g_pending;

        void Thunk(RE::BSGraphics::Renderer* a_renderer, RE::RENDER_TARGETS::RENDER_TARGET a_target, const char* a_path,
            Format a_format)
        {
            // The engine's file is written first and stays as the fallback.
            g_original(a_renderer, a_target, a_path, a_format);
            try {
                const bool replaceable = a_format == Format::kBMP || a_format == Format::kJPG || a_format == Format::kPNG;
                logger::info("[Screenshot] Skyrim saved target {} to {} (format {}){}", static_cast<int>(a_target),
                    a_path ? a_path : "(no path)", static_cast<int>(a_format),
                    a_target == RE::RENDER_TARGETS::kSCREENSHOT && a_path && replaceable
                        ? "; replacing it with the finished frame" : "");
                if (a_target != RE::RENDER_TARGETS::kSCREENSHOT || !a_path || !replaceable) { return; }
                std::lock_guard lock(g_mutex);
                if (g_pending.size() < 4) { g_pending.push_back({ a_path }); }
            } catch (...) {
                // A screenshot must never take the game down; the engine's file stays.
            }
        }
    }

    void Install()
    {
        const auto site = REL::RelocationID(75522, 77316).address();
        std::array<std::uint8_t, 16> entry{};
        if (!HookSafety::Read(site, entry.data(), entry.size())) {
            logger::warn("[Screenshot] Skyrim's screenshot save is unreadable; screenshots stay at the render size");
            return;
        }
        // Another plugin's jump is chained by InstallEntryDetour; original code must be safe to move.
        const bool chained = entry[0] == 0xE9 || (entry[0] == 0xFF && entry[1] == 0x25);
        if (!chained && !HookSafety::SafePrologueLength(entry.data(), entry.size())) {
            std::string bytes;
            for (const auto byte : entry) { bytes += std::format("{:02X} ", byte); }
            logger::warn("[Screenshot] Skyrim's screenshot save starts with code TRP cannot hook safely ({}); "
                         "screenshots stay at the render size", bytes);
            return;
        }
        g_original = reinterpret_cast<Save>(HookSafety::InstallEntryDetour(site, reinterpret_cast<std::uintptr_t>(&Thunk)));
        if (g_original) {
            logger::info("[Screenshot] screenshot save hooked{}; saved screenshots get the finished frame",
                chained ? " behind another plugin" : "");
        } else {
            logger::warn("[Screenshot] could not hook Skyrim's screenshot save; screenshots stay at the render size");
        }
    }

    bool Take(Request& a_request)
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty()) { return false; }
        a_request = std::move(g_pending.front());
        g_pending.pop_front();
        return true;
    }
}
