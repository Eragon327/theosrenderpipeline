#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace TheosRenderPipeline::NeuralRendering
{
	// NVIDIA's NR runtime 310.9.16 ships its kernels for RTX 50 only. Universal
	// runs it on RTX 40 with TRP's kernel pack (tools/nr-runtime): Ada code
	// compiled from NVIDIA's own PTX, next to NVIDIA's original code. The DLL on
	// disk stays NVIDIA's signed file; a loaded copy is patched in memory, on
	// RTX 40 only, before NGX initializes it. Standard leaves it unmodified.
	inline constexpr std::string_view kAdaKernelRuntimeSha256 =
		"2BC19BC09A7B89DD44F73E2AC0108FCC586E2154BDD0830F242F4F040E28789F";
	inline constexpr std::string_view kAdaKernelPackSha256 =
		"ACD2B6DD5526A71FFD5B34F78A9B7230DFDD57D5F79C581D10A876EB130AF542";
	// Installed next to the runtime by Universal.
	inline constexpr std::string_view kAdaKernelPackName = "nvngx_dlssnr_310.9.16_rtx40.trpk";
	inline constexpr int kAdaComputeCapability = 89;

	// False in Standard, which never patches the runtime.
	bool AdaKernelsSupported();

	struct AdaKernelResult
	{
		bool applied{};
		bool unsafe{}; // Unconfirmed rollback: do not enter this runtime or fall back.
		std::string status;
	};
	// a_module: a runtime with kAdaKernelRuntimeSha256, loaded and not yet
	// initialized. Every write is checked against NVIDIA's original bytes and
	// applied before any NGX entry. The caller serializes loading/initialization;
	// newlyLoaded must be false if the image was resident before its LoadLibrary.
	// Only a fresh module or our recorded installation is accepted. Installed
	// modules and read-only packs stay resident for the process lifetime.
	AdaKernelResult ApplyAdaKernels(HMODULE a_module, const std::filesystem::path& a_pack, bool newlyLoaded);
}
