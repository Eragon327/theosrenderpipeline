#include "FrameGen/NeuralRenderingRuntimeContract.h"
#include "FrameGen/NeuralRuntimeFiles.h"
#include "FrameGen/NeuralRuntimeCompatibility.h"
#include "FrameGen/SourceDLSSGSettings.h"
#include "RuntimePathSettings.h"
#include <map>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <process.h>

using namespace TheosRenderPipeline;
using NeuralRendering::RuntimeBuild;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); } }

int main()
{
    using NeuralRendering::MatchRuntime;
    using NeuralRendering::kRuntimeSize;
    // Known builds keep their exact mapping.
    Require(MatchRuntime(kRuntimeSize, NeuralRendering::kLegacyRuntimeSha256, "300.0.0.0", false) == RuntimeBuild::Legacy, "legacy by hash");
    Require(MatchRuntime(kRuntimeSize, NeuralRendering::kNexusRuntimeSha256, "310.8.0.0", true) == RuntimeBuild::Nexus3108, "Nexus 310.8 by hash");
    Require(MatchRuntime(kRuntimeSize, NeuralRendering::kBuild14RuntimeSha256, "310.4.0.0", true) == RuntimeBuild::Build14, "Build14 by hash");
    // Any other runtime loads by version: a community int8 build (different size and hash) or a newer NVIDIA one.
    Require(MatchRuntime(309671536, "F82812BF0AAFD77F7F95ABC61B0AFA36967EF9D0A773137B85E1D978201C19F9", "310.8.0.0", true) == RuntimeBuild::Version310,
        "community 310.8 build loads by version");
    Require(MatchRuntime(170000000, "ABC", "311.2.0.0", true) == RuntimeBuild::Version310, "newer NVIDIA version loads");
    Require(NeuralRendering::UsesReconstructionContract(RuntimeBuild::Version310), "310.x uses the current contract");
    // Older or unreadable versions are refused, and the legacy-only path never takes a 310 build.
    Require(MatchRuntime(170000000, "ABC", "300.1.0.0", true) == RuntimeBuild::Unknown, "pre-310 unknown build refused");
    Require(MatchRuntime(170000000, "ABC", "unavailable", true) == RuntimeBuild::Unknown, "unreadable version refused");
    Require(MatchRuntime(309671536, "F82812BF", "310.8.0.0", false) == RuntimeBuild::Unknown, "legacy-only callers keep exact identity");
    Require(NeuralRendering::MajorVersion("310.8.0.0") == 310 && NeuralRendering::MajorVersion("") == 0 &&
        NeuralRendering::MajorVersion("x310") == 0, "major version parsing");

    // Community int8 v2 is refused only on RTX 40 (SM 8.9); everything else loads by version.
    {
        using NeuralRendering::FindIncompatibleRuntime;
        constexpr std::string_view v2 = "F82812BF0AAFD77F7F95ABC61B0AFA36967EF9D0A773137B85E1D978201C19F9";
        constexpr std::string_view adaFixed = "4A5710990E8D16AD66B06128B4FA5E7910F5B98EF823193082314C36277089D0";
        Require(FindIncompatibleRuntime(v2, 89) != nullptr, "int8 v2 is refused on RTX 40");
        for (const int capability : { 75, 86, 120, 0 }) {
            Require(FindIncompatibleRuntime(v2, capability) == nullptr, "int8 v2 still loads on RTX 20/30/50 and unknown GPUs");
        }
        Require(FindIncompatibleRuntime(adaFixed, 89) == nullptr, "a fixed build loads on RTX 40");
        Require(FindIncompatibleRuntime(NeuralRendering::kNexusRuntimeSha256, 89) == nullptr, "NVIDIA's runtime loads on RTX 40");
    }
    // The folder listing: NVIDIA's name first, other nvngx_dlssnr*.dll after, nothing else.
    const auto root = std::filesystem::temp_directory_path() / std::format(L"TRPNeuralRuntimeFiles-{}", _getpid());
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto folder = root / L"TheosRenderPipeline" / L"NVIDIA";
    std::filesystem::create_directories(folder);
    for (const auto* name : {L"nvngx_dlssnr_int8.dll", L"nvngx_dlssnr.dll", L"NVNGX_DLSSNR_v3.DLL", L"nvngx_dlss.dll", L"nvngx_dlssnr.txt"}) {
        std::ofstream(folder / name) << "x";
    }
    std::filesystem::create_directories(folder / L"nvngx_dlssnr_folder.dll");
    const auto listed = SourceDLSSG::ListNeuralRuntimes(root, "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll");
    Require(listed == std::vector<std::string>{"Automatic", "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll",
        "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll", "TheosRenderPipeline/NVIDIA/NVNGX_DLSSNR_v3.DLL"},
        "Automatic, then only nvngx_dlssnr*.dll files, default first");
    Require(SourceDLSSG::ListNeuralRuntimes(root, R"(TheosRenderPipeline\NVIDIA\NVNGX_DLSSNR.DLL)").size() == 4,
        "a differently written path to a listed file is not duplicated");
    Require(SourceDLSSG::ListNeuralRuntimes(root, "automatic").size() == 4, "Automatic is not listed twice as a custom path");
    const auto custom = SourceDLSSG::ListNeuralRuntimes(root, "D:/Other/nvngx_dlssnr.dll");
    Require(custom.size() == 5 && custom.back() == "D:/Other/nvngx_dlssnr.dll", "a custom path stays selectable");
    Require(SourceDLSSG::ListNeuralRuntimes(root / L"missing", "") == std::vector<std::string>{"Automatic"},
        "a missing folder still offers Automatic");
    std::filesystem::remove_all(root, error);

    // Automatic: the newest model each GPU generation runs, 310.8 always last.
    {
        using namespace SourceDLSSG;
        using List = std::vector<std::string_view>;
        Require(AutomaticNeuralRuntimes(120) == List{kLatestNeuralRuntime, kCompatNeuralRuntime, kDefaultNeuralRuntime},
            "RTX 50 prefers NVIDIA's 310.9.16, then the compat build");
        Require(AutomaticNeuralRuntimes(89) == List{kCompatNeuralRuntime, kDefaultNeuralRuntime},
            "RTX 40 never tries NVIDIA's RTX 50-only 310.9.16");
        for (const int capability : {0, 75, 86, 90}) {
            Require(AutomaticNeuralRuntimes(capability) == List{kDefaultNeuralRuntime},
                "RTX 20/30, unverified and unknown GPUs keep 310.8");
        }
        Require(IsAutomaticNeuralRuntime("automatic") && !IsAutomaticNeuralRuntime(kDefaultNeuralRuntime), "Automatic is a name, not a file");
        const auto plugins = std::filesystem::path(L"C:/Game/Data/SKSE/Plugins");
        Require(ResolveNeuralRuntimeSetting("Automatic", plugins) == (plugins / L"TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll").lexically_normal(),
            "Automatic resolves to 310.8 until the GPU is known");
        Require(ResolveNeuralRuntimeSetting("D:/x/nvngx_dlssnr_new.dll", plugins) == std::filesystem::path(L"D:/x/nvngx_dlssnr_new.dll"),
            "absolute paths are kept");
        Require(NeuralRuntimeLabel("Automatic") == "Automatic" && NeuralRuntimeLabel(kDefaultNeuralRuntime) == "NVIDIA 310.8" &&
            NeuralRuntimeLabel(R"(C:\G\NVIDIA\NVNGX_DLSSNR_310.9.16.DLL)") == "NVIDIA 310.9.16 (RTX 50)" &&
            NeuralRuntimeLabel(kCompatNeuralRuntime) == "310.9.16 compat (RTX 20-40)" &&
            NeuralRuntimeLabel("TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll") == "nvngx_dlssnr_int8.dll", "menu labels");

        // Saved settings: the old default means Automatic unless it was picked since.
        struct Ini
        {
            std::map<std::string, std::string> values;
            const char* GetValue(const char*, const char* key, const char* fallback) const
            {
                const auto it = values.find(key); return it == values.end() ? fallback : it->second.c_str();
            }
            bool GetBoolValue(const char* section, const char* key, bool fallback) const
            {
                const auto* value = GetValue(section, key, nullptr); return value ? std::string_view(value) == "true" : fallback;
            }
            long GetLongValue(const char*, const char*, long fallback) const { return fallback; }
            double GetDoubleValue(const char*, const char*, double fallback) const { return fallback; }
            void SetValue(const char*, const char* key, const char* value) { values[key] = value; }
            void SetBoolValue(const char*, const char* key, bool value) { values[key] = value ? "true" : "false"; }
            void SetLongValue(const char*, const char*, long) {}
            void SetDoubleValue(const char*, const char*, double) {}
            void Delete(const char*, const char*) {}
        };
        auto load = [](std::map<std::string, std::string> values) { return LoadPreferences(Ini{std::move(values)}).neuralRuntime; };
        Require(load({}) == "Automatic", "a missing value is Automatic");
        Require(load({{"NeuralRenderingRuntimePath", "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll"}}) == "Automatic",
            "an INI from 0.4.3 or older moves to Automatic");
        Require(load({{"NeuralRenderingRuntimePath", "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll"}}) ==
            "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll", "a chosen community runtime stays chosen");
        Ini saved;
        Preferences explicit310;
        explicit310.neuralRuntime = std::string(kDefaultNeuralRuntime);
        StorePreferences(saved, explicit310);
        Require(saved.values["NeuralRenderingRuntimeExplicit"] == "true" && LoadPreferences(saved).neuralRuntime == kDefaultNeuralRuntime,
            "310.8 picked after Automatic exists stays 310.8");
        StorePreferences(saved, Preferences{});
        Require(saved.values["NeuralRenderingRuntimePath"] == "Automatic" && saved.values["NeuralRenderingRuntimeExplicit"] == "false",
            "Automatic is saved as Automatic");
        RuntimePathSettings paths;
        paths.Load(Ini{{{"NeuralRenderingRuntimePath", "TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll"}}});
        Require(paths.neural == "Automatic" &&
            paths.Resolve(plugins).neural == (plugins / L"TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll").lexically_normal().string(),
            "startup paths read the same migration and resolve Automatic to a real file");
    }
    std::puts("PASS NR runtime selection: known builds, version 310+ admission, refusals, folder listing, Automatic choice, labels and settings migration");
}
