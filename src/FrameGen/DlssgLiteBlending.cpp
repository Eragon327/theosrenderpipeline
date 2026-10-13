#include <PCH.h>
#include "DlssgLiteBlending.h"
#include "DlssgPreset.h"
#include "../PluginPaths.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <xbyak/xbyak.h>
#include <atomic>
#include <memory>

namespace TheosRenderPipeline::SourceDLSSG::LiteBlending
{
	namespace
	{
		std::atomic<bool> gEnabled{};
		std::atomic<std::uint32_t> gRequests{};
		Snapshot gState{};

		void AddParameter(int feature, NVSDK_NGX_Parameter* parameters)
		{
			if (feature != NVSDK_NGX_Feature_FrameGeneration || !parameters || !gEnabled.load(std::memory_order_relaxed)) { return; }
			NVSDK_NGX_Parameter_SetI(parameters, kParameter.data(), 1);
			if (gRequests.fetch_add(1, std::memory_order_relaxed) == 0) {
				spdlog::info("[SourceDLSSG] Lite blending requested at frame generation creation ({}=1)", kParameter);
			}
		}

		// The export checks that its return address is inside NVIDIA's NGX
		// module, so this is entered by jump and leaves by jump: the caller's
		// return address stays in place. Arguments are saved around the C++ call,
		// then the displaced prologue runs before resuming the original body.
		class Stub final : public Xbyak::CodeGenerator
		{
		public:
			Stub(std::span<const std::uint8_t> prologue, std::uintptr_t resume) : CodeGenerator(160)
			{
				Xbyak::Label destination;
				push(rcx); push(rdx); push(r8); push(r9);
				sub(rsp, 0x28);
				mov(ecx, edx);
				mov(rdx, r8);
				mov(rax, reinterpret_cast<std::uintptr_t>(&AddParameter));
				call(rax);
				add(rsp, 0x28);
				pop(r9); pop(r8); pop(rdx); pop(rcx);
				for (const auto value : prologue) { db(value); }
				jmp(ptr[rip + destination]);
				L(destination);
				dq(resume);
				ready();
			}
		};

		bool RuntimeNamesParameter(HMODULE module)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
			const auto* section = IMAGE_FIRST_SECTION(nt);
			for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
				if (std::memcmp(section->Name, ".rdata", 6) != 0) { continue; }
				const auto* start = reinterpret_cast<const std::uint8_t*>(module) + section->VirtualAddress;
				return NamesParameter({ start, section->Misc.VirtualSize });
			}
			return false;
		}
	}

	bool Install(HMODULE provider)
	{
		if (gState.hook == HookState::Installed) { return true; }
		if (!provider) { gState.hook = HookState::Failed; return false; }
		gState.runtimeSupports = RuntimeNamesParameter(provider);
		gState.driverPreset = ReadDriverPreset();
		if (gState.driverPreset) {
			spdlog::info("[SourceDLSSG] NVIDIA driver profile sets Frame Generation preset {} (0x{:X}); DLSS-G prefers it over TRP's preset",
				DriverPresetName(gState.driverPreset), gState.driverPreset);
		}
		auto* site = reinterpret_cast<std::uint8_t*>(GetProcAddress(provider, "NVSDK_NGX_D3D12_CreateFeature"));
		std::array<std::uint8_t, kPrologueBytes> prologue{};
		if (!site) { gState.hook = HookState::Failed; return false; }
		std::memcpy(prologue.data(), site, prologue.size());
		if (!MatchesCreateFeaturePrologue(prologue)) {
			gState.hook = HookState::UnsupportedPrologue;
			spdlog::warn("[SourceDLSSG] Lite blending unavailable: unexpected CreateFeature prologue in {}", PluginPaths::ModulePath(provider).string());
			return false;
		}
		// Resident for the process, like the retained DLSS-G module.
		static std::unique_ptr<Stub> stub;
		stub = std::make_unique<Stub>(prologue, reinterpret_cast<std::uintptr_t>(site + prologue.size()));
		// mov rax, imm64; jmp rax; nop. rax is volatile at function entry.
		std::array<std::uint8_t, kPrologueBytes> patch{ 0x48, 0xB8 };
		const auto target = reinterpret_cast<std::uintptr_t>(stub->getCode());
		std::memcpy(patch.data() + 2, &target, sizeof(target));
		patch[10] = 0xFF; patch[11] = 0xE0; patch[12] = 0x90;
		DWORD old{};
		if (!VirtualProtect(site, patch.size(), PAGE_EXECUTE_READWRITE, &old)) { gState.hook = HookState::Failed; return false; }
		std::memcpy(site, patch.data(), patch.size());
		DWORD ignored{};
		VirtualProtect(site, patch.size(), old, &ignored);
		FlushInstructionCache(GetCurrentProcess(), site, patch.size());
		gState.hook = std::memcmp(site, patch.data(), patch.size()) == 0 ? HookState::Installed : HookState::Failed;
		spdlog::info("[SourceDLSSG] Lite blending hook {} on {} (build names {}: {})",
			gState.hook == HookState::Installed ? "installed" : "failed", PluginPaths::ModulePath(provider).string(), kParameter, gState.runtimeSupports);
		return gState.hook == HookState::Installed;
	}

	void Configure(bool enabled) { gEnabled.store(enabled, std::memory_order_relaxed); }

	Snapshot State()
	{
		auto state = gState;
		state.requests = gRequests.load(std::memory_order_relaxed);
		return state;
	}

	std::uint32_t ReadDriverPreset()
	{
		// NvAPI DRS through nvapi_QueryInterface; the public SDK is not a TRP dependency.
		constexpr std::uint32_t kFrameGenerationPreset = 0x10E41DF1;
		struct Setting
		{
			std::uint32_t version;
			wchar_t name[2048];
			std::uint32_t id, type, location, isCurrentPredefined, isPredefinedValid;
			union Value { std::uint32_t u32; std::uint8_t binary[4100]; wchar_t text[2048]; } predefined, current;
		};
		struct Application
		{
			std::uint32_t version, isPredefined;
			wchar_t appName[2048], friendlyName[2048], launcher[2048], fileInFolder[2048];
			std::uint32_t flags;
			wchar_t commandLine[2048];
		};
		const auto nvapi = GetModuleHandleW(L"nvapi64.dll") ? GetModuleHandleW(L"nvapi64.dll") : LoadLibraryW(L"nvapi64.dll");
		const auto query = nvapi ? reinterpret_cast<void* (*)(std::uint32_t)>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
		if (!query) { return 0; }
		using Init = int (*)();
		using Create = int (*)(void**);
		using Load = int (*)(void*);
		using FindApp = int (*)(void*, const wchar_t*, void**, Application*);
		using Base = int (*)(void*, void**);
		using Get = int (*)(void*, void*, std::uint32_t, Setting*);
		using Destroy = int (*)(void*);
		const auto init = reinterpret_cast<Init>(query(0x0150E828));
		const auto create = reinterpret_cast<Create>(query(0x0694D52E));
		const auto load = reinterpret_cast<Load>(query(0x375DBD6B));
		const auto findApp = reinterpret_cast<FindApp>(query(0xEEE566B2));
		const auto base = reinterpret_cast<Base>(query(0xDA8466A0));
		const auto get = reinterpret_cast<Get>(query(0x73BF8338));
		const auto destroy = reinterpret_cast<Destroy>(query(0xDAD9CFF8));
		if (!init || !create || !load || !findApp || !base || !get || !destroy || init() != 0) { return 0; }
		void* session{};
		if (create(&session) != 0) { return 0; }
		std::uint32_t value{};
		if (load(session) == 0) {
			auto setting = std::make_unique<Setting>();
			auto application = std::make_unique<Application>();
			auto read = [&](void* profile) {
				*setting = {};
				setting->version = sizeof(Setting) | (1u << 16);
				return profile && get(session, profile, kFrameGenerationPreset, setting.get()) == 0 && setting->type == 0;
			};
			const auto exe = PluginPaths::ModulePath(GetModuleHandleW(nullptr)).filename();
			void* profile{};
			application->version = sizeof(Application) | (4u << 16);
			if (findApp(session, exe.c_str(), &profile, application.get()) == 0 && read(profile)) { value = setting->current.u32; }
			else if (base(session, &profile) == 0 && read(profile)) { value = setting->current.u32; }
		}
		destroy(session);
		return value;
	}
}
