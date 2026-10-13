#include "FrameGen/NeuralRenderingRuntimeContract.h"
#include "FrameGen/NeuralRuntimeFiles.h"
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
    Require(listed == std::vector<std::string>{"TheosRenderPipeline/NVIDIA/nvngx_dlssnr.dll", "TheosRenderPipeline/NVIDIA/nvngx_dlssnr_int8.dll",
        "TheosRenderPipeline/NVIDIA/NVNGX_DLSSNR_v3.DLL"}, "only nvngx_dlssnr*.dll files, default first");
    Require(SourceDLSSG::ListNeuralRuntimes(root, R"(TheosRenderPipeline\NVIDIA\NVNGX_DLSSNR.DLL)").size() == 3,
        "a differently written path to a listed file is not duplicated");
    const auto custom = SourceDLSSG::ListNeuralRuntimes(root, "D:/Other/nvngx_dlssnr.dll");
    Require(custom.size() == 4 && custom.back() == "D:/Other/nvngx_dlssnr.dll", "a custom path stays selectable");
    Require(SourceDLSSG::ListNeuralRuntimes(root / L"missing", "").empty(), "a missing folder lists nothing");
    std::filesystem::remove_all(root, error);
    std::puts("PASS NR runtime selection: known builds, version 310+ admission, refusals, folder listing");
}
