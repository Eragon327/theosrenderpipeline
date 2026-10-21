#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TheosRenderPipeline::Compatibility
{
	// Finds ImGui_ImplDX11_RenderDrawData in a build that has no exact profile.
	// The supported mods compile it with MSVC's large-frame prologue,
	//   push rbp; push r13; lea rbp,[rsp+disp32]; mov eax,imm32
	// 17 bytes in four instructions without RIP-relative operands, so a detour can
	// relocate them. Within a short distance it grows its buffers by
	// TotalVtxCount + 5000 and TotalIdxCount + 10000. A module qualifies only when
	// exactly one entry carries both; every profiled build's RVA is found this way.
	inline constexpr std::size_t kRenderDrawDataPrologueSize = 17;
	inline constexpr std::size_t kRenderDrawDataScanWindow = 0x500;

	[[nodiscard]] inline bool MatchesRenderDrawDataPrologue(std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.size() < kRenderDrawDataPrologueSize) {
			return false;
		}
		const auto* p = bytes.data();
		return p[0] == 0x40 && p[1] == 0x55 && p[2] == 0x41 && p[3] == 0x55 &&
		       p[4] == 0x48 && p[5] == 0x8D && p[6] == 0xAC && p[7] == 0x24 && p[10] == 0xFF && p[11] == 0xFF &&
		       p[12] == 0xB8 && p[15] == 0x00 && p[16] == 0x00;
	}

	[[nodiscard]] inline bool ContainsDword(std::span<const std::uint8_t> bytes, std::uint32_t value) noexcept
	{
		const std::array<std::uint8_t, 4> needle{ static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
			static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24) };
		for (std::size_t i = 0; i + needle.size() <= bytes.size(); ++i) {
			if (bytes[i] == needle[0] && bytes[i + 1] == needle[1] && bytes[i + 2] == needle[2] && bytes[i + 3] == needle[3]) {
				return true;
			}
		}
		return false;
	}

	// Offsets into code of every candidate entry.
	[[nodiscard]] inline std::vector<std::size_t> FindRenderDrawData(std::span<const std::uint8_t> code)
	{
		std::vector<std::size_t> found;
		for (std::size_t i = 0; i + kRenderDrawDataPrologueSize <= code.size(); ++i) {
			if (code[i] != 0x40 || !MatchesRenderDrawDataPrologue(code.subspan(i))) {
				continue;
			}
			const auto body = code.subspan(i, (std::min)(kRenderDrawDataScanWindow, code.size() - i));
			if (ContainsDword(body, 5000) && ContainsDword(body, 10000)) {
				found.push_back(i);
			}
		}
		return found;
	}
}
