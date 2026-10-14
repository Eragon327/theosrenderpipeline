#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG
{
    inline constexpr std::string_view kNeuralRuntimeFolder = "TheosRenderPipeline/NVIDIA";

    // Every nvngx_dlssnr*.dll in the NVIDIA folder, as INI paths relative to
    // Data/SKSE/Plugins, sorted with NVIDIA's default name first. `current` is
    // appended when it names a file elsewhere, so a custom path stays selectable.
    inline std::vector<std::string> ListNeuralRuntimes(const std::filesystem::path& pluginDirectory, const std::string& current)
    {
        // Case- and separator-insensitive, as Windows paths are.
        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::vector<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator it(pluginDirectory / std::filesystem::u8path(kNeuralRuntimeFolder), error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) { continue; }
            const auto utf8 = it->path().filename().u8string();
            const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            const auto folded = lower(name);
            if (folded.starts_with("nvngx_dlssnr") && folded.ends_with(".dll")) { names.push_back(name); }
        }
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            const bool aDefault = lower(a) == "nvngx_dlssnr.dll", bDefault = lower(b) == "nvngx_dlssnr.dll";
            return aDefault != bDefault ? aDefault : lower(a) < lower(b);
        });
        std::vector<std::string> runtimes;
        for (const auto& name : names) { runtimes.push_back(std::string(kNeuralRuntimeFolder) + "/" + name); }
        if (!current.empty() && std::none_of(runtimes.begin(), runtimes.end(), [&](const std::string& r) { return lower(r) == lower(current); })) {
            runtimes.push_back(current);
        }
        return runtimes;
    }
}
