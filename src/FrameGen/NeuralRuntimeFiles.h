#pragma once

#include "../NvidiaBaselinePolicy.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace TheosRenderPipeline::SourceDLSSG
{
    inline constexpr std::string_view kNeuralRuntimeFolder = "TheosRenderPipeline/NVIDIA";
    // Runtimes relative to Data/SKSE/Plugins. NVIDIA's 310.8 runs on every RTX GPU
    // and is the fallback. NVIDIA's 310.9.16 has kernels only for RTX 50. TRP's
    // Universal adds Ada kernels in memory from a pinned adjacent pack. The
    // standalone RTX 40 build remains a fallback. The bundled community compat
    // rebuild supplies the RTX 20/30 path. See tools/nr-runtime.
    inline constexpr std::string_view kDefaultNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll";
    inline constexpr std::string_view kLatestNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16.dll";
    inline constexpr std::string_view kRTX40NeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16_rtx40.dll";
    inline constexpr std::string_view kCompatNeuralRuntime = "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_310.9.16_compat.dll";
    // The saved value that lets TRP choose a bundled runtime for the GPU.
    inline constexpr std::string_view kAutomaticNeuralRuntime = "Automatic";

    // Case- and separator-insensitive, as Windows paths are.
    inline std::string FoldRuntimePath(std::string_view path)
    {
        std::string text(path);
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(c)); });
        return text;
    }

    inline bool IsAutomaticNeuralRuntime(std::string_view value)
    {
        return FoldRuntimePath(value) == FoldRuntimePath(kAutomaticNeuralRuntime);
    }

    // Automatic's choices for a CUDA compute capability (89 is RTX 40, 120 is
    // RTX 50, 0 unknown), best first; missing or refused files are skipped. The
    // Universal installer tries the original 310.9.16 first on RTX 40; a safe
    // installation refusal falls through to the remaining choices. The standalone
    // RTX 40 build keeps NVIDIA's RTX 50 code, so it also stands in there. The
    // community FP16 rebuild supplies the new model on RTX 20/30; 310.8 remains
    // the fallback if that file is missing or feature creation is refused.
    inline std::vector<std::string_view> AutomaticNeuralRuntimes(int computeCapability, bool adaKernels = false)
    {
        if (computeCapability >= 120) { return { kLatestNeuralRuntime, kRTX40NeuralRuntime, kCompatNeuralRuntime, kDefaultNeuralRuntime }; }
        if (computeCapability == 89) {
            if (adaKernels) { return { kLatestNeuralRuntime, kRTX40NeuralRuntime, kCompatNeuralRuntime, kDefaultNeuralRuntime }; }
            return { kRTX40NeuralRuntime, kCompatNeuralRuntime, kDefaultNeuralRuntime };
        }
        if (computeCapability == 75 || computeCapability == 86) {
            return { kCompatNeuralRuntime, kDefaultNeuralRuntime };
        }
        return { kDefaultNeuralRuntime };
    }

    // Before Automatic, saved INIs held the bundled 310.8 path as the only
    // choice. That path means Automatic unless it was picked explicitly since.
    inline std::string NormalizeNeuralRuntimeSetting(std::string value, bool explicitChoice)
    {
        if (value.empty() || (!explicitChoice && FoldRuntimePath(value) == FoldRuntimePath(kDefaultNeuralRuntime))) {
            return std::string(kAutomaticNeuralRuntime);
        }
        return value;
    }

    // The file a saved value names. Automatic starts on 310.8 until the
    // backend knows the GPU, so availability checks always see a real file.
    inline std::filesystem::path ResolveNeuralRuntimeSetting(std::string_view value, const std::filesystem::path& pluginDirectory)
    {
        return ResolveRuntimePath(std::filesystem::u8path(IsAutomaticNeuralRuntime(value) ? kDefaultNeuralRuntime : value), pluginDirectory);
    }

    // Friendly names for bundled and commonly installed runtime filenames.
    // Unknown filenames stay visible; selection and saved paths are unchanged.
    inline std::string NeuralRuntimeLabel(std::string_view value)
    {
        if (IsAutomaticNeuralRuntime(value)) { return "Automatic"; }
        auto filename = [](std::string_view path) {
            const auto utf8 = std::filesystem::u8path(path).filename().u8string();
            return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        };
        const auto name = filename(value);
        const auto folded = FoldRuntimePath(name);
        if (folded == FoldRuntimePath(filename(kDefaultNeuralRuntime))) { return "NVIDIA 310.8"; }
        if (folded == FoldRuntimePath(filename(kLatestNeuralRuntime))) { return "NVIDIA 310.9.16"; }
        if (folded == FoldRuntimePath(filename(kRTX40NeuralRuntime))) { return "TRP 310.9.16 (RTX 40)"; }
        if (folded == FoldRuntimePath(filename(kCompatNeuralRuntime))) { return "Community 310.9.16 (FP16)"; }
        if (folded == "nvngx_dlssnr_int8.dll") { return "Community 310.8 (INT8 v2)"; }
        if (folded == "nvngx_dlssnr_int8_ada.dll") { return "Community 310.8 (INT8 v2, RTX 40 fix)"; }
        if (folded == "nvngx_dlssnr_pd_build21.dll") { return "PureDark 310.9.16 (Build 21)"; }
        return name;
    }

    // Automatic, then every nvngx_dlssnr*.dll in the NVIDIA folder as INI paths
    // relative to Data/SKSE/Plugins, NVIDIA's default name first. `current` is
    // appended when it names a file elsewhere, so a custom path stays selectable.
    inline std::vector<std::string> ListNeuralRuntimes(const std::filesystem::path& pluginDirectory, const std::string& current)
    {
        std::vector<std::string> names;
        std::error_code error;
        for (std::filesystem::directory_iterator it(pluginDirectory / std::filesystem::u8path(kNeuralRuntimeFolder), error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) { continue; }
            const auto utf8 = it->path().filename().u8string();
            const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            const auto folded = FoldRuntimePath(name);
            if (folded.starts_with("nvngx_dlssnr") && folded.ends_with(".dll")) { names.push_back(name); }
        }
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            const bool aDefault = FoldRuntimePath(a) == "nvngx_dlssnr.dll", bDefault = FoldRuntimePath(b) == "nvngx_dlssnr.dll";
            return aDefault != bDefault ? aDefault : FoldRuntimePath(a) < FoldRuntimePath(b);
        });
        std::vector<std::string> runtimes{ std::string(kAutomaticNeuralRuntime) };
        for (const auto& name : names) { runtimes.push_back(std::string(kNeuralRuntimeFolder) + "/" + name); }
        if (!current.empty() && std::none_of(runtimes.begin(), runtimes.end(),
                [&](const std::string& r) { return FoldRuntimePath(r) == FoldRuntimePath(current); })) {
            runtimes.push_back(current);
        }
        return runtimes;
    }
}
