#include "Localization.h"
#include "PluginPaths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

namespace TheosRenderPipeline::Localization
{
	namespace
	{
		void (*g_sink)(bool, std::string_view){};
		std::atomic<const Table*> g_active{ nullptr };
		std::mutex g_lifecycle;             // guards the containers below, not lookups
		std::vector<std::shared_ptr<const Table>> g_tables; // every table ever installed
		std::shared_ptr<const Table> g_installed;
		std::string g_error;
		std::vector<Info> g_catalog;
		bool g_catalogLoaded{ false };

		void Write(bool a_error, std::string_view a_message)
		{
			if (g_sink) {
				g_sink(a_error, a_message);
			}
		}
		void Warn(std::string_view a_message)
		{
			Write(false, a_message);
		}

		bool EqualsNoCase(std::string_view a_left, std::string_view a_right)
		{
			if (a_left.size() != a_right.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_left.size(); ++i) {
				const auto l = static_cast<unsigned char>(a_left[i]);
				const auto r = static_cast<unsigned char>(a_right[i]);
				if (std::tolower(l) != std::tolower(r)) {
					return false;
				}
			}
			return true;
		}

		std::string_view TrimView(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return a_text.substr(first, last - first + 1);
		}

		// UTF-8 -> code points, skipping malformed bytes so a mis-encoded
		// dictionary still loads.
		void AppendCodepoints(std::string_view a_text, std::vector<char32_t>& a_out)
		{
			std::size_t i = 0;
			while (i < a_text.size()) {
				const auto lead = static_cast<unsigned char>(a_text[i]);
				std::size_t length = 0;
				char32_t value = 0;
				if (lead < 0x80) {
					length = 1;
					value = lead;
				} else if ((lead & 0xE0) == 0xC0) {
					length = 2;
					value = lead & 0x1F;
				} else if ((lead & 0xF0) == 0xE0) {
					length = 3;
					value = lead & 0x0F;
				} else if ((lead & 0xF8) == 0xF0) {
					length = 4;
					value = lead & 0x07;
				} else {
					++i;
					continue;
				}
				if (i + length > a_text.size()) {
					break;
				}
				bool valid = true;
				for (std::size_t k = 1; k < length; ++k) {
					const auto next = static_cast<unsigned char>(a_text[i + k]);
					if ((next & 0xC0) != 0x80) {
						valid = false;
						break;
					}
					value = (value << 6) | (next & 0x3F);
				}
				i += valid ? length : 1;
				if (valid && value > 0x7F) {
					a_out.push_back(value);
				}
			}
		}

		std::string Utf8FromWide(std::wstring_view a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int size = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
				nullptr, 0, nullptr, nullptr);
			if (size <= 0) {
				return {};
			}
			std::string out(static_cast<std::size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()), out.data(), size,
				nullptr, nullptr);
			return out;
		}
	} // namespace

	std::size_t Hash::operator()(std::string_view a_text) const noexcept
	{
		std::size_t hash = 1469598103934665603ull;
		for (const auto byte : a_text) {
			hash ^= static_cast<unsigned char>(byte);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	// ---- Table ------------------------------------------------------------

	const char* Table::Find(std::string_view a_text, std::string_view a_context) const noexcept
	{
		if (!a_context.empty()) {
			if (const auto context = contexts_.find(a_context); context != contexts_.end()) {
				if (const auto entry = context->second.values.find(a_text); entry != context->second.values.end()) {
					return entry->second.c_str();
				}
			}
		}
		if (const auto entry = strings_.values.find(a_text); entry != strings_.values.end()) {
			return entry->second.c_str();
		}
		return nullptr;
	}

	bool Table::Has(std::string_view a_text, std::string_view a_context) const noexcept
	{
		return Find(a_text, a_context) != nullptr;
	}

	std::vector<std::string> Table::Keys() const
	{
		std::vector<std::string> keys;
		keys.reserve(strings_.values.size());
		for (const auto& entry : strings_.values) {
			keys.push_back(entry.first);
		}
		std::sort(keys.begin(), keys.end());
		return keys;
	}

	// ---- parser -----------------------------------------------------------

	// The printf conversions in a_text, in order ("%d|%.1f|%s"), without %%. Text
	// with std::format fields is formatted by FormatText, which already falls
	// back to English on a mismatch, so its % signs are literal and not checked.
	std::string PrintfConversions(std::string_view a_text)
	{
		std::string result;
		if (a_text.find('{') != std::string_view::npos) {
			return result;
		}
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] != '%') {
				continue;
			}
			std::size_t j = i + 1;
			if (j < a_text.size() && a_text[j] == '%') {
				i = j;
				continue;
			}
			while (j < a_text.size() && std::string_view("-+#0123456789.*hlLqjzt").find(a_text[j]) != std::string_view::npos) {
				++j;
			}
			if (j < a_text.size() && std::string_view("diouxXeEfFgGaAcspn").find(a_text[j]) != std::string_view::npos) {
				result.append(a_text.substr(i, j - i + 1)).push_back('|');
				i = j;
			}
		}
		return result;
	}

	struct Parser
	{
		Table& table;
		std::string_view origin;
		std::vector<std::string>* warnings;
		std::size_t line{};

		void Warn(std::string_view a_message)
		{
			std::string text = origin.empty() ? std::string{} : std::string(origin) + ":";
			text += std::to_string(line) + ": " + std::string(a_message);
			if (warnings) {
				warnings->push_back(text);
			}
			Localization::Warn(text);
		}

		// Split "key = value" at the first '=' outside quotes.
		static bool SplitEntry(std::string_view a_line, std::string_view& a_key, std::string_view& a_value)
		{
			bool quoted = false;
			for (std::size_t i = 0; i < a_line.size(); ++i) {
				const char c = a_line[i];
				if (c == '\\' && quoted) {
					++i;
					continue;
				}
				if (c == '"') {
					quoted = !quoted;
					continue;
				}
				if (c != '=' || quoted) {
					continue;
				}
				a_key = TrimView(a_line.substr(0, i));
				a_value = TrimView(a_line.substr(i + 1));
				return true;
			}
			return false;
		}

		// Quoted text keeps leading/trailing spaces and reads \n, \t, \, \", \=.
		static bool Decode(std::string_view a_text, std::string& a_out, bool& a_wasQuoted)
		{
			a_wasQuoted = a_text.size() >= 2 && a_text.front() == '"';
			if (!a_wasQuoted) {
				a_out.assign(a_text);
				return true;
			}
			if (a_text.back() != '"') {
				return false;
			}
			a_out.clear();
			for (std::size_t i = 1; i + 1 < a_text.size(); ++i) {
				if (a_text[i] != '\\') {
					a_out.push_back(a_text[i]);
					continue;
				}
				if (++i + 1 > a_text.size()) {
					return false;
				}
				switch (a_text[i]) {
				case 'n':
					a_out.push_back('\n');
					break;
				case 't':
					a_out.push_back('\t');
					break;
				case '\\':
					a_out.push_back('\\');
					break;
				case '"':
					a_out.push_back('"');
					break;
				default:
					a_out.push_back(a_text[i]);
					break;
				}
			}
			return true;
		}

		void LanguageEntry(std::string_view a_key, std::string_view a_value)
		{
			std::string key(a_key), value;
			bool quoted{};
			if (!Decode(a_value, value, quoted)) {
				Warn("unterminated quote");
				return;
			}
			std::string lower = key;
			for (auto& c : lower) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (lower == "code") {
				table.info_.code = value;
			} else if (lower == "name") {
				table.info_.name = value;
			} else if (lower == "authors" || lower == "author") {
				table.info_.authors = value;
			} else if (lower == "version") {
				table.info_.version = value;
			} else if (lower == "fonts" || lower == "font") {
				std::size_t start = 0;
				while (start <= value.size()) {
					const auto end = value.find_first_of(";", start);
					const auto piece = TrimView(std::string_view(value).substr(start,
						end == std::string::npos ? std::string::npos : end - start));
					if (!piece.empty()) {
						table.info_.fonts.emplace_back(piece);
					}
					if (end == std::string::npos) {
						break;
					}
					start = end + 1;
				}
			} else {
				Warn("unknown [Language] key");
			}
		}

		void StringEntry(Table::Map& a_map, std::string_view a_key, std::string_view a_value)
		{
			std::string key, value;
			bool keyQuoted{}, valueQuoted{};
			if (!Decode(a_key, key, keyQuoted)) {
				Warn("unterminated quote in key");
				return;
			}
			if (!Decode(a_value, value, valueQuoted)) {
				Warn("unterminated quote in translation");
				return;
			}
			if (key.empty()) {
				Warn("empty key");
				return;
			}
			if (value.empty() && !valueQuoted) {
				// An unquoted empty translation means "not translated yet" and keeps
				// the English text; a quoted empty string ("") is deliberate.
				++table.info_.untranslated;
				return;
			}
			// Many labels are printf formats (ImGui::Text, SetTooltip, slider
			// formats), where a missing or extra %s reads the wrong argument and
			// can crash the game. Keep the English text unless the translation has
			// the same conversions in the same order.
			if (PrintfConversions(key) != PrintfConversions(value)) {
				Warn("translation's % conversions differ from the English; kept English");
				++table.info_.untranslated;
				return;
			}
			const auto [slot, inserted] = a_map.values.insert_or_assign(key, value);
			static_cast<void>(slot);
			if (!inserted) {
				++table.info_.duplicates;
			}
		}

		void Run(std::string_view a_text)
		{
			std::string_view section{};
			std::size_t position = 0;
			if (a_text.size() >= 3 && static_cast<unsigned char>(a_text[0]) == 0xEF &&
				static_cast<unsigned char>(a_text[1]) == 0xBB && static_cast<unsigned char>(a_text[2]) == 0xBF) {
				position = 3; // UTF-8 BOM
			}
			while (position <= a_text.size()) {
				const auto end = a_text.find('\n', position);
				auto entry = a_text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
				position = end == std::string_view::npos ? a_text.size() + 1 : end + 1;
				++line;
				entry = TrimView(entry);
				if (entry.empty() || entry.front() == ';' || entry.front() == '#') {
					continue;
				}
				if (entry.front() == '[') {
					if (entry.back() != ']') {
						Warn("unterminated section header");
						continue;
					}
					section = TrimView(entry.substr(1, entry.size() - 2));
					continue;
				}
				std::string_view key, value;
				if (!SplitEntry(entry, key, value)) {
					Warn("expected key = value");
					continue;
				}
				if (EqualsNoCase(section, "Language")) {
					LanguageEntry(key, value);
					continue;
				}
				if (section.empty()) {
					Warn("key outside of a section");
					continue;
				}
				if (EqualsNoCase(section, "Strings")) {
					StringEntry(table.strings_, key, value);
					continue;
				}
				constexpr std::string_view prefix = "Strings.";
				if (section.size() > prefix.size() && EqualsNoCase(section.substr(0, prefix.size()), prefix)) {
					StringEntry(table.contexts_[std::string(section.substr(prefix.size()))], key, value);
					continue;
				}
				Warn("unknown section");
			}
		}
	};

	std::shared_ptr<const Table> Parse(std::string_view a_text, std::string_view a_origin,
		std::vector<std::string>* a_warnings)
	{
		auto table = std::shared_ptr<Table>(new Table{});
		table->info_.path = std::filesystem::path(std::string(a_origin));
		Parser parser{ *table, a_origin, a_warnings };
		parser.Run(a_text);
		table->info_.entries = table->strings_.values.size();
		if (table->info_.code.empty()) {
			table->info_.code = table->info_.path.stem().string();
		}
		if (table->info_.name.empty()) {
			table->info_.name = table->info_.code;
		}
		// Glyphs for the merged system font: every translation plus the display name.
		std::vector<char32_t> codepoints;
		codepoints.reserve(1024);
		AppendCodepoints(table->info_.name, codepoints);
		for (const auto& entry : table->strings_.values) {
			AppendCodepoints(entry.second, codepoints);
		}
		for (const auto& context : table->contexts_) {
			for (const auto& entry : context.second.values) {
				AppendCodepoints(entry.second, codepoints);
			}
		}
		std::sort(codepoints.begin(), codepoints.end());
		codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
		table->codepoints_ = std::move(codepoints);
		return table;
	}

	std::shared_ptr<const Table> Load(const std::filesystem::path& a_file, std::vector<std::string>* a_warnings)
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(a_file, error)) {
			if (a_warnings) {
				a_warnings->push_back("missing dictionary: " + a_file.string());
			}
			return nullptr;
		}
		std::ifstream input(a_file, std::ios::binary);
		if (!input) {
			if (a_warnings) {
				a_warnings->push_back("unreadable dictionary: " + a_file.string());
			}
			return nullptr;
		}
		const std::string text{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		return Parse(text, a_file.string(), a_warnings);
	}

	std::filesystem::path Directory()
	{
		// Dictionaries live next to the shipped runtimes inside the game's
		// virtual Data tree, so resolve them from the executable rather than
		// the process working directory (Mod Organizer can start us anywhere).
		const auto plugins = PluginPaths::Directory();
		if (!plugins.empty()) {
			return plugins / L"TheosRenderPipeline" / L"Lang";
		}
		return std::filesystem::path(L"Data/SKSE/Plugins/TheosRenderPipeline/Lang");
	}

	namespace
	{
		std::vector<std::filesystem::path> DictionaryFiles()
		{
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(Directory(), error)) {
				if (!entry.is_regular_file(error)) {
					continue;
				}
				if (!EqualsNoCase(entry.path().extension().string(), ".ini")) {
					continue;
				}
				files.push_back(entry.path());
			}
			std::sort(files.begin(), files.end());
			return files;
		}

		std::string Primary(std::string_view a_code)
		{
			const auto dash = a_code.find('-');
			return std::string(a_code.substr(0, dash == std::string_view::npos ? a_code.size() : dash));
		}
	} // namespace

	std::vector<Info> Catalog()
	{
		std::lock_guard lock(g_lifecycle);
		if (g_catalogLoaded) {
			return g_catalog;
		}
		g_catalog.clear();
		for (const auto& file : DictionaryFiles()) {
			if (const auto table = Load(file); table) {
				if (table->Count() == 0) {
					continue;
				}
				g_catalog.push_back(table->Description());
			}
		}
		std::sort(g_catalog.begin(), g_catalog.end(), [](const Info& a_left, const Info& a_right) {
			return a_left.code < a_right.code;
		});
		g_catalogLoaded = true;
		return g_catalog;
	}

	void Install(std::shared_ptr<const Table> a_table)
	{
		std::lock_guard lock(g_lifecycle);
		if (a_table) {
			g_tables.push_back(a_table);
		}
		g_installed = std::move(a_table);
		g_catalogLoaded = false;
		g_active.store(g_installed.get(), std::memory_order_release);
	}

	std::shared_ptr<const Table> Active()
	{
		std::lock_guard lock(g_lifecycle);
		return g_installed;
	}

	bool Activate(std::string_view a_codeOrAuto)
	{
		const std::string request(TrimView(a_codeOrAuto));
		std::string code = request;
		const bool automatic = EqualsNoCase(request, "auto");
		if (automatic) {
			code = AutoCode();
		}
		if (code.empty() || EqualsNoCase(code, "en") || EqualsNoCase(code, "english")) {
			Install(nullptr);
			{
				std::lock_guard lock(g_lifecycle);
				g_error.clear();
			}
			return true;
		}

		const auto files = DictionaryFiles();
		// 1. An exact file name: Language=zh-CN reads Lang\zh-CN.ini.
		for (const auto& file : files) {
			if (EqualsNoCase(file.stem().string(), code)) {
				if (const auto table = Load(file); table && table->Count() > 0) {
					Install(table);
					std::lock_guard lock(g_lifecycle);
					g_error.clear();
					return true;
				}
			}
		}
		// 2. A [Language] Code, then 3. the primary language subtag.
		std::shared_ptr<const Table> fallback;
		for (const auto& file : files) {
			const auto table = Load(file);
			if (!table || table->Count() == 0) {
				continue;
			}
			if (EqualsNoCase(table->Description().code, code)) {
				Install(table);
				std::lock_guard lock(g_lifecycle);
				g_error.clear();
				return true;
			}
			if (!fallback && EqualsNoCase(Primary(table->Description().code), Primary(code))) {
				fallback = table;
			}
		}
		if (fallback) {
			Install(fallback);
			std::lock_guard lock(g_lifecycle);
			g_error.clear();
			return true;
		}

		if (automatic || EqualsNoCase(Primary(code), "en")) {
			// Nothing matches; English is the intended fallback.
			Install(nullptr);
			{
				std::lock_guard lock(g_lifecycle);
				g_error.clear();
			}
			if (automatic) {
				Warn("no dictionary for the Windows UI language '" + code + "'; using English");
			}
			return true;
		}
		{
			std::lock_guard lock(g_lifecycle);
			g_error = "dictionary not found for '" + std::string(a_codeOrAuto) + "'";
			Write(true, g_error);
		}
		return false;
	}

	bool Reload()
	{
		const std::string code(ActiveCode());
		return Activate(code.empty() ? std::string_view("en") : std::string_view(code));
	}

	std::string_view ActiveCode()
	{
		const auto table = Active();
		return table ? std::string_view(table->Description().code) : std::string_view{};
	}

	std::string_view ActiveName()
	{
		const auto table = Active();
		return table ? std::string_view(table->Description().name) : std::string_view(TR("English"));
	}

	bool Translated()
	{
		return g_active.load(std::memory_order_acquire) != nullptr;
	}

	std::string_view LastError()
	{
		std::lock_guard lock(g_lifecycle);
		return g_error;
	}

	std::string AutoCode()
	{
		wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
		if (::GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) <= 0) {
			return {};
		}
		return Utf8FromWide(name);
	}

	void SetLogSink(void (*a_sink)(bool, std::string_view))
	{
		g_sink = a_sink;
	}

	// ---- lookups ------------------------------------------------------------

	const char* Translate(const char* a_english) noexcept
	{
		return Translate(a_english, nullptr);
	}

	const char* Translate(const char* a_english, const char* a_context) noexcept
	{
		if (!a_english) {
			return "";
		}
		const auto* table = g_active.load(std::memory_order_acquire);
		if (!table) {
			return a_english;
		}
		if (const auto* text = table->Find(a_english, a_context ? std::string_view(a_context) : std::string_view{})) {
			return text;
		}
		return a_english;
	}

	std::string TranslateText(std::string_view a_english)
	{
		const std::string key(a_english);
		return std::string(Translate(key.c_str()));
	}

	std::string TranslateText(std::string_view a_english, std::string_view a_context)
	{
		const std::string key(a_english);
		const std::string context(a_context);
		return std::string(Translate(key.c_str(), context.c_str()));
	}
} // namespace TheosRenderPipeline::Localization
