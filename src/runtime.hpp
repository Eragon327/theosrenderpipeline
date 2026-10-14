#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <filesystem>
#include <cstdint>

namespace trp::ampere {
struct RuntimeStatus {
    bool prepared{}, bridgeInstalled{}, failed{}, createSeen{};
    std::uint32_t fatbins{}, resolverMask{}, requirementsCalls{}, capabilityCalls{}, createCalls{};
    const char* error{};
};
using Log = void (*)(const char*);
// The Turing diagnostic candidate stops synchronously on a real kernel-load
// failure, before NVIDIA can publish a partially initialized FG object.
using Fatal = void (*)(const char*);
// One host, one adapter, one provider. This owner and its published data remain
// alive until process exit. Call only at the controlled pre-slInit boundary.
// Separate modules are permitted only by the host's coordinated CS path.
bool Start(ID3D12Device* device, const std::filesystem::path& directory, Log log, bool allowSeparateModules = false, Fatal fatal = nullptr) noexcept;
// Developer test only: run the unchanged RTX 30 (SM86) preparation on a physical
// RTX 40 adapter, which can execute SM86 programs. Every provider write is the
// Ampere one; only the physical-adapter checks accept Ada. Call before Start.
void EmulateAmpereOnAda(bool enabled) noexcept;
RuntimeStatus Snapshot() noexcept;
bool Verify() noexcept;
void EnterStartupScope() noexcept;
void LeaveStartupScope() noexcept;
} // namespace trp::ampere
