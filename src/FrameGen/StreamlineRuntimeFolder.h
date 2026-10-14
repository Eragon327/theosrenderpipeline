#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace TheosRenderPipeline::SourceDLSSG
{
	// The RTX 20 backport prepares the exact DLSS-G 310.9.1 programs; RTX 30 is
	// qualified for 310.9.16 too. Newer DLSS-G (Preset C) ships in the configured
	// folder; the 310.9.1 Streamline set ships beside it, because NGX loads
	// DLSS-G from sl.dlss_g's own folder.
	inline constexpr std::wstring_view kBackportFolderSuffix = L"-RTX20-30";

	// RTX 20 always uses the 310.9.1 set; RTX 30 only when RTX30LegacyDLSSG asks for it.
	constexpr bool UsesBackportStreamline(bool turingRoute, bool ampereRoute, bool rtx30Legacy)
	{
		return turingRoute || (ampereRoute && rtx30Legacy);
	}

	inline std::filesystem::path BackportStreamlineDirectory(const std::filesystem::path& configured)
	{
		auto folder = configured.has_filename() ? configured : configured.parent_path();
		return folder.parent_path() / (folder.filename().wstring() + std::wstring(kBackportFolderSuffix));
	}
}
