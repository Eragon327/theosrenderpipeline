#include "Localization.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace TheosRenderPipeline::Localization;

namespace
{
void Require(bool a_condition, const char* a_message)
{
	if (!a_condition) {
		throw std::runtime_error(a_message);
	}
}

bool HasWarning(const std::vector<std::string>& a_warnings, std::string_view a_needle)
{
	for (const auto& warning : a_warnings) {
		if (warning.find(a_needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

bool HasCodepoint(const std::vector<char32_t>& a_codepoints, char32_t a_wanted)
{
	return std::find(a_codepoints.begin(), a_codepoints.end(), a_wanted) != a_codepoints.end();
}

// [Language] metadata, the simple [Strings] form and exact key matching.
void Metadata()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"(; comment at the top
# a second comment style
[Language]
Code = ja-JP
Name = 日本語
Version = 1
Authors = someone
Fonts = C:/Windows/Fonts/meiryo.ttc; msgothic.ttc
[Strings]
Apply = 適用
"Off" = オフ
)", "test.ini", &warnings);
	Require(table != nullptr, "a dictionary parses");
	const auto& info = table->Description();
	Require(info.code == "ja-JP", "[Language] Code is read");
	Require(info.name == "日本語", "[Language] Name is read");
	Require(info.version == "1", "[Language] Version is read");
	Require(info.authors == "someone", "[Language] Authors is read");
	Require(info.fonts.size() == 2 && info.fonts[0] == "C:/Windows/Fonts/meiryo.ttc" && info.fonts[1] == "msgothic.ttc",
		"[Language] Fonts splits on semicolons and trims each entry");
	Require(info.entries == 2 && table->Count() == 2, "both [Strings] entries are counted");
	Require(warnings.empty(), "a clean dictionary reports no warning");
	Require(std::string(table->Find("Apply")) == "適用", "a translation is found by its exact English key");
	Require(!table->Has("apply"), "keys are case sensitive, so 'apply' is a different label");
	Require(table->Find("missing") == nullptr, "a missing key has no translation");
	const auto keys = table->Keys();
	Require(keys.size() == 2 && keys[0] == "Apply" && keys[1] == "Off",
		"Keys() returns the decoded keys in order");
}

// Escapes, quoting, deliberate empty strings and duplicate keys.
void Escapes()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"([Strings]
"[ and ] are NR shortcuts while EnableNRHotkeys is on." = "[ 和 ] 是 NR 快捷键。"
"line" = "第一行\n第二行"
"\tindented" = "\t缩进"
"quoted \"word\"" = "带引号的 \"词\""
"back\\slash" = "反斜杠 \\"
"Deliberate" = ""
"Unfinished" =
"Repeated" = 一
"Repeated" = 二
)", "escape.ini", &warnings);
	Require(std::string(table->Find("[ and ] are NR shortcuts while EnableNRHotkeys is on.")) == "[ 和 ] 是 NR 快捷键。",
		"a quoted key may start with '['");
	Require(std::string(table->Find("line")) == "第一行\n第二行", "a quoted translation decodes \n");
	Require(std::string(table->Find("\tindented")) == "\t缩进", "a quoted key decodes \t");
	Require(std::string(table->Find("quoted \"word\"")) == "带引号的 \"词\"", "escaped quotes survive on both sides");
	Require(std::string(table->Find("back\\slash")) == "反斜杠 \\", "escaped backslashes survive on both sides");
	Require(table->Has("Deliberate") && std::string(table->Find("Deliberate")).empty(),
		"a quoted empty translation is deliberate and kept");
	Require(!table->Has("Unfinished"), "an unquoted empty translation falls back to English");
	Require(table->Description().untranslated == 1, "the untranslated count reports the bare empty value");
	Require(std::string(table->Find("Repeated")) == "二", "a repeated key keeps the last translation");
	Require(table->Description().duplicates == 1, "the duplicate count reports the repeat");
	Require(table->Description().entries == 7, "duplicates count once");
}

// Broken lines are reported and skipped, the rest of the file still loads.
void Warnings()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"([Strings]
Apply = 应用
this line has no separator
[CookBook]
Apply = 应用
[Cooking
)", "broken.ini", &warnings);
	Require(table->Count() == 1, "the valid entry still loads");
	Require(HasWarning(warnings, "broken.ini:3:") && HasWarning(warnings, "expected key = value"),
		"the missing separator is reported with its line number");
	Require(HasWarning(warnings, "broken.ini:5:") && HasWarning(warnings, "unknown section"),
		"an unknown section is reported and ignored");
	Require(HasWarning(warnings, "broken.ini:6:") && HasWarning(warnings, "unterminated section header"),
		"an unterminated section header is reported");
}

// A translation used as a printf format must take the same arguments, or ImGui
// reads the wrong ones; such an entry keeps the English text.
void Conversions()
{
	std::vector<std::string> warnings;
	const auto table = Parse(R"ini([Strings]
"Pass %d: %.1f%%" = "第 %d 遍：%.1f%%"
"Limited to %.0f%% so the menu fits." = "限制为 %s，以便菜单适合。"
"NR Pass %d: %s" = "NR %s"
"Uses about 19% fewer model pixels." = "约少 19% 的模型像素。"
"{} passes" = "{} 遍 (100% 输入)"
)ini", "formats.ini", &warnings);
	Require(table->Has("Pass %d: %.1f%%"), "matching conversions are kept");
	Require(!table->Has("Limited to %.0f%% so the menu fits."), "a different conversion keeps English");
	Require(!table->Has("NR Pass %d: %s"), "a missing conversion keeps English");
	Require(HasWarning(warnings, "formats.ini:3:") && HasWarning(warnings, "formats.ini:4:"),
		"each rejected entry is reported with its line");
	Require(table->Has("Uses about 19% fewer model pixels."), "a percent sign in prose is not a conversion");
	Require(table->Has("{} passes"), "std::format text is not checked for % conversions");
	Require(warnings.size() == 2, "only the mismatched entries warn");
}

// [Strings.<context>] wins over [Strings] for the labels that need a meaning.
void Contexts()
{
	const auto table = Parse(R"([Strings]
Normal = 正常
Off = 关闭
[Strings.texture]
Normal = 法线
)", "context.ini");
	Install(table);
	Require(std::string(Translate("Normal")) == "正常", "the shared key reads the global translation");
	Require(std::string(Translate("Normal", "texture")) == "法线", "a context selects its own translation");
	Require(std::string(Translate("Normal", "unknown")) == "正常", "a missing context falls back to the global one");
	Require(std::string(Translate("Off", "texture")) == "关闭", "a context without the key falls back to the global one");
	Install(nullptr);
	Require(std::string(Translate("Normal", "texture")) == "Normal", "English is the source text once unloaded");
}

// The translated format string keeps the arguments.
void Formatting()
{
	const auto table = Parse(R"([Strings]
"{} change{}" = "{} 项更改"
"Reached {} FPS" = "达到 {} FPS"
)", "format.ini");
	Install(table);
	Require(Format("{} change{}", 3, "s") == "3 项更改", "a translated format drops the English plural argument");
	Require(Format("Reached {} FPS", 59.9) == "达到 59.9 FPS", "numeric arguments survive translation");
	Install(nullptr);
	Require(Format("{} change{}", 1, "") == "1 change", "English formatting still works without a dictionary");
}

// Glyph coverage for the merged system font.
void Glyphs()
{
	const auto table = Parse(R"([Language]
Name = 简体中文
[Strings]
Apply = 应用
)", "glyph.ini");
	const auto& codepoints = table->Codepoints();
	Require(HasCodepoint(codepoints, 0x5E94), "a translated character reaches the glyph list");
	Require(!HasCodepoint(codepoints, 'A'), "ASCII stays with the embedded font");
	Require(std::is_sorted(codepoints.begin(), codepoints.end()), "the glyph list is sorted");
	Require(std::adjacent_find(codepoints.begin(), codepoints.end()) == codepoints.end(), "the glyph list has no repeats");
	// The parser skips malformed UTF-8 instead of failing the whole file.
	const auto broken = Parse("[Strings]\nApply = \xFF\xFE\n", "broken-utf8.ini");
	Require(broken->Count() == 1, "a mis-encoded translation still loads");
}

// Reading dictionaries from the packaged folder, and activating them.
void Files()
{
	const auto directory = Directory();
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	std::filesystem::create_directories(directory, error);
	{
		std::ofstream file(directory / "zh-CN.ini", std::ios::binary);
		// UTF-8 BOM and CRLF line endings, like a file saved by Notepad.
		file << "\xEF\xBB\xBF[Language]\r\nCode = zh-CN\r\nName = 简体中文\r\n[Strings]\r\nApply = 应用\r\n";
	}
	Require(Activate("zh-CN"), "a dictionary is activated by its file name");
	Require(Translated() && ActiveCode() == "zh-CN", "the active code reports the dictionary");
	Require(ActiveName() == "简体中文", "the active name comes from [Language] Name");
	Require(std::string(Translate("Apply")) == "应用", "translations apply after activation");

	// A pointer handed out earlier still reads its own translation after a reload.
	const char* applied = Translate("Apply");
	Require(Activate("zh-TW"), "'zh-TW' matches the 'zh' dictionary");
	Require(std::string(Translate("Apply")) == "应用", "the primary language subtag matches");
	Require(std::string(applied) == "应用", "a previous pointer stays valid across activation");

	Require(!Activate("de-DE"), "a missing dictionary reports failure");
	Require(Translated() && std::string(Translate("Apply")) == "应用", "a failed activation keeps the previous dictionary");
	Require(!LastError().empty(), "the failure is described");
	Require(Activate("en") && !Translated() && ActiveCode().empty(), "'en' selects the English source text");
	Require(std::string(Translate("Apply")) == "Apply", "English is the source text");

	const auto catalog = Catalog();
	Require(catalog.size() == 1 && catalog[0].code == "zh-CN" && catalog[0].name == "简体中文",
		"the catalogue lists the packaged dictionary");
	Require(Activate("auto"), "'auto' always succeeds");
	Require(Reload() || !Translated(), "reloading the active dictionary is safe");
	Require(!AutoCode().empty(), "the Windows UI locale is available");
	std::filesystem::remove_all(directory, error);
}

// The dictionary that ships in the package: it must parse cleanly and cover the
// overlay, otherwise a user would see English (or a warning) on first launch.
void Packaged(const std::filesystem::path& a_path)
{
	Require(std::filesystem::is_regular_file(a_path), "the packaged dictionary exists");
	std::vector<std::string> warnings;
	const auto table = Load(a_path, &warnings);
	Require(table != nullptr, "the packaged dictionary loads");
	Require(warnings.empty(), "the packaged dictionary parses without warnings");
	const auto& info = table->Description();
	Require(info.code == "zh-CN", "the packaged code is zh-CN");
	Require(!info.name.empty(), "the packaged name is set");
	Require(info.duplicates == 0, "the packaged dictionary has no duplicate keys");
	Require(info.entries > 500, "the packaged dictionary covers the overlay");
	Require(info.fonts.size() >= 1, "the packaged dictionary asks for a CJK font");
	const auto& codepoints = table->Codepoints();
	Require(codepoints.size() > 300, "the packaged dictionary has a real glyph list");
	Require(HasCodepoint(codepoints, 0x5E94) && HasCodepoint(codepoints, 0x7528),
		"common Chinese characters are in the glyph list");
	Require(std::is_sorted(codepoints.begin(), codepoints.end()), "the packaged glyph list is sorted");
	Require(std::adjacent_find(codepoints.begin(), codepoints.end()) == codepoints.end(),
		"the packaged glyph list has no repeats");
	Require(codepoints.back() <= 0xFFFF, "every packaged glyph fits the 16-bit atlas");
	std::printf("packaged dictionary: %zu strings (%.1f%% of the keys have a translation), %zu glyphs\n",
		info.entries,
		info.entries + info.untranslated == 0 ? 100.0
											 : 100.0 * static_cast<double>(info.entries)
			/ static_cast<double>(info.entries + info.untranslated),
		codepoints.size());
}

} // namespace

int main(int argc, char** argv)
{
	try {
		SetLogSink([](bool a_error, std::string_view a_message) {
			static_cast<void>(a_error);
			static_cast<void>(a_message);
		});
		Metadata();
		Escapes();
		Warnings();
		Conversions();
		Contexts();
		Formatting();
		Glyphs();
		Files();
		if (argc > 1) {
			Packaged(argv[1]);
		}
		SetLogSink(nullptr);
		std::puts("PASS dictionary parsing, escapes, contexts, formatting, glyphs, activation and package");
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
