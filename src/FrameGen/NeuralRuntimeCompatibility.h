#pragma once

#include <Windows.h>
#include <cstring>
#include <string_view>

namespace TheosRenderPipeline::NeuralRendering
{
	// Runtimes that load and run without error but render black on one GPU
	// family. Every other runtime still loads by version; a fixed build has a
	// different hash and is unaffected.
	struct IncompatibleRuntime
	{
		std::string_view sha256;
		int computeCapability; // major * 10 + minor
		const char* description;
	};
	inline constexpr IncompatibleRuntime kIncompatibleRuntimes[]{
		// Community int8 v2 rebuilt the SM75/SM86/SM120 kernels for int8 weights
		// but kept NVIDIA's FP8-era SM89 kernels, which render black on RTX 40.
		{ "F82812BF0AAFD77F7F95ABC61B0AFA36967EF9D0A773137B85E1D978201C19F9", 89,
			"community int8 v2, which has no working RTX 40 kernels" },
	};

	inline const IncompatibleRuntime* FindIncompatibleRuntime(std::string_view sha256, int computeCapability)
	{
		for (const auto& entry : kIncompatibleRuntimes) {
			if (entry.computeCapability == computeCapability && entry.sha256 == sha256) { return &entry; }
		}
		return nullptr;
	}

	// The CUDA compute capability of the adapter with this LUID (89 for RTX 40),
	// or 0 when the CUDA driver or adapter is unavailable. Kernels are chosen by it.
	inline int ComputeCapability(const LUID& luid)
	{
		using Init = int(__stdcall*)(unsigned);
		using Count = int(__stdcall*)(int*);
		using Get = int(__stdcall*)(int*, int);
		using Capability = int(__stdcall*)(int*, int*, int);
		using Luid = int(__stdcall*)(char*, unsigned*, int);
		const auto cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!cuda) { return 0; }
		const auto init = reinterpret_cast<Init>(GetProcAddress(cuda, "cuInit"));
		const auto count = reinterpret_cast<Count>(GetProcAddress(cuda, "cuDeviceGetCount"));
		const auto get = reinterpret_cast<Get>(GetProcAddress(cuda, "cuDeviceGet"));
		const auto capability = reinterpret_cast<Capability>(GetProcAddress(cuda, "cuDeviceComputeCapability"));
		const auto deviceLuid = reinterpret_cast<Luid>(GetProcAddress(cuda, "cuDeviceGetLuid"));
		int result = 0, devices = 0;
		if (init && count && get && capability && deviceLuid && init(0) == 0 && count(&devices) == 0) {
			for (int ordinal = 0; ordinal < devices && !result; ++ordinal) {
				int device{}, major{}, minor{};
				char bytes[8]{};
				unsigned mask{};
				if (get(&device, ordinal) || deviceLuid(bytes, &mask, device) || std::memcmp(bytes, &luid, sizeof(bytes))) { continue; }
				if (capability(&major, &minor, device) == 0) { result = major * 10 + minor; }
			}
		}
		FreeLibrary(cuda);
		return result;
	}
}
