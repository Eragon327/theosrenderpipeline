#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// External dictionaries.
//
// Every user-visible label in the overlay is written in English in the source
// and passed through TR() / TRC() / TRF(). With no dictionary loaded those
// calls return the English text unchanged, so the plugin behaves exactly like
// an English-only build. A dictionary adds translations without a rebuild:
//
//   Data\SKSE\Plugins\TheosRenderPipeline\Lang\<code>.ini
//
// The directory hangs off the game executable's Data tree, not the process
// working directory, so mod managers can start Skyrim from anywhere.
//
// [Language] holds the metadata the language picker shows and the optional
// font list; [Strings] maps each English label to its translation. A key the
// dictionary does not define keeps the English text, so a partial dictionary
// is always usable. LOCALIZATION.md documents the file format.
namespace TheosRenderPipeline::Localization
{
	// What a dictionary declares about itself. Code and Name come from
	// [Language]; a file without them is known by its file stem.
	struct Info
	{
		std::string code;           // "zh-CN"
		std::string name;           // native display name, e.g. "简体中文"
		std::string authors;        // free text from [Language] Authors
		std::string version;        // free text from [Language] Version
		std::vector<std::string> fonts; // preferred font files: absolute, or a name under %WINDIR%\Fonts
		std::filesystem::path path;
		std::size_t entries{};      // translated keys
		std::size_t untranslated{}; // keys present with an empty translation; they fall back to English
		std::size_t duplicates{};   // repeated keys; the last one wins
	};

	// Case-insensitive-stable hashing is not needed: dictionary keys are matched
	// exactly, which keeps "On" and "on" distinct labels.
	struct Hash
	{
		using is_transparent = void;
		std::size_t operator()(std::string_view a_text) const noexcept;
		std::size_t operator()(const std::string& a_text) const noexcept { return (*this)(std::string_view(a_text)); }
	};

	// A parsed dictionary. Immutable: translation results stay valid for the
	// lifetime of the process, so reloading never invalidates a pointer a caller
	// still holds.
	class Table
	{
	  public:
		Table(const Table&) = delete;
		Table& operator=(const Table&) = delete;

		const Info& Description() const noexcept { return info_; }
		std::size_t Count() const noexcept { return strings_.values.size(); }
		// Every key defined in [Strings], sorted. The audit tool compares these
		// against the TR() call sites in src/.
		std::vector<std::string> Keys() const;
		bool Has(std::string_view a_text, std::string_view a_context = {}) const noexcept;
		// Code points the translations need beyond ASCII, sorted and unique.
		const std::vector<char32_t>& Codepoints() const noexcept { return codepoints_; }
		// The translation, or nullptr when the key is missing. a_context selects
		// [Strings.<context>] for the labels that share an English spelling but
		// not a meaning (see LOCALIZATION.md).
		const char* Find(std::string_view a_text, std::string_view a_context = {}) const noexcept;

		// Filled by Parse(); lookups read it. Public so the parser can build a
		// table without a second copy of every string.
		Table() = default;

		struct Map
		{
			std::unordered_map<std::string, std::string, Hash, std::equal_to<>> values;
		};

		Info info_;
		Map strings_;
		std::unordered_map<std::string, Map, Hash, std::equal_to<>> contexts_;
		std::vector<char32_t> codepoints_;
	};

	// The folder dictionaries are read from, relative to Skyrim's working
	// directory (inside MO2's virtual Data tree).
	std::filesystem::path Directory();

	// Every readable dictionary in Directory(), sorted by code.
	std::vector<Info> Catalog();

	// Parse dictionary text. A structurally broken file still yields a table with
	// the lines that were valid; warnings name the offending line numbers.
	std::shared_ptr<const Table> Parse(std::string_view a_text, std::string_view a_origin,
		std::vector<std::string>* a_warnings = nullptr);
	std::shared_ptr<const Table> Load(const std::filesystem::path& a_file,
		std::vector<std::string>* a_warnings = nullptr);

	// Make a table active. A null table means English, the source text.
	void Install(std::shared_ptr<const Table> a_table);
	std::shared_ptr<const Table> Active();

	// Activate by code: "" or "en" selects English, "auto" asks Windows for the
	// UI language, anything else matches a dictionary by [Language] Code, then by
	// file stem, then by primary language ("zh" matches "zh-CN"). A missing
	// dictionary leaves the current selection in place and reports false.
	bool Activate(std::string_view a_codeOrAuto);
	bool Reload(); // re-read the active dictionary file

	std::string_view ActiveCode(); // "" while English
	std::string_view ActiveName(); // "English", or the dictionary's [Language] Name
	bool Translated();             // a non-English dictionary is active
	std::string_view LastError();  // why the last Activate failed; empty on success
	// The Windows UI locale as a BCP-47 tag ("zh-CN"), or "" when unavailable.
	std::string AutoCode();

	// Route dictionary diagnostics (parse warnings, missing files) to the plugin
	// log. Without a sink they are dropped, which keeps the unit tests quiet.
	void SetLogSink(void (*a_sink)(bool a_error, std::string_view a_message));

	// ---- text lookups ---------------------------------------------------
	// Returns a_english unchanged when no dictionary is active or the key is absent.
	const char* Translate(const char* a_english) noexcept;
	const char* Translate(const char* a_english, const char* a_context) noexcept;
	std::string TranslateText(std::string_view a_english);
	std::string TranslateText(std::string_view a_english, std::string_view a_context);

	// std::vformat checks a char format string against the execution character
	// set, so a UTF-8 dictionary entry only formats in a translation unit built
	// with /utf-8. Every target that draws the overlay sets that flag (see
	// CMakeLists.txt); the fallback below makes a future mismatch degrade to the
	// English text instead of throwing out of the render loop.
	template <class... Args>
	std::string FormatText(const char* a_translated, const char* a_english, Args&&... a_args)
	{
		try {
			return std::vformat(a_translated, std::make_format_args(a_args...));
		} catch (const std::exception&) {
			if (a_translated != a_english) {
				try {
					return std::vformat(a_english, std::make_format_args(a_args...));
				} catch (const std::exception&) {
				}
			}
			return a_english;
		}
	}

	// Translate a format string and format it. Replaces std::format, whose
	// compile-time format-string parameter cannot take a translated string.
	template <class... Args>
	std::string Format(const char* a_english, Args&&... a_args)
	{
		return FormatText(Translate(a_english), a_english, a_args...);
	}
	template <class... Args>
	std::string FormatContext(const char* a_context, const char* a_english, Args&&... a_args)
	{
		return FormatText(Translate(a_english, a_context), a_english, a_args...);
	}
} // namespace TheosRenderPipeline::Localization

// Mark a user-visible English label for translation. TRC adds a context for
// labels that read the same in English but mean different things. TRF is TR
// plus formatting, for the std::format call sites.
#define TR(text) ::TheosRenderPipeline::Localization::Translate(text)
#define TRC(text, context) ::TheosRenderPipeline::Localization::Translate(text, context)
#define TRF(format, ...) ::TheosRenderPipeline::Localization::Format(format, __VA_ARGS__)
#define TRFC(context, format, ...) ::TheosRenderPipeline::Localization::FormatContext(context, format, __VA_ARGS__)
// A translated label whose ImGui ID stays the English text ("<translation>###<English>"),
// for widgets that keep state by ID (tabs, collapsing headers), so switching
// language keeps the selected tab and the open sections. Use .c_str() in the call.
#define TRID(text) (std::string(::TheosRenderPipeline::Localization::Translate(text)) + "###" text)
