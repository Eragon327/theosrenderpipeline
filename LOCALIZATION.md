# Localization

The overlay's labels are written in English in the source and looked up in an
external dictionary before they are drawn. A dictionary adds or corrects a
language without rebuilding the plugin; with no dictionary loaded, or with no
entry for a label, the English text appears unchanged. A partial dictionary is
therefore always usable.

Dictionaries are read from

```
Data\SKSE\Plugins\TheosRenderPipeline\Lang\<code>.ini
```

relative to Skyrim's `Data` folder, which MO2 presents as its virtual file
system. One file per language; the package ships `zh-CN.ini`.

## Contents

- [Choose the language](#choose-the-language)
- [Dictionary layout](#dictionary-layout)
- [Values, quoting and escapes](#values-quoting-and-escapes)
- [Contexts for shared English text](#contexts-for-shared-english-text)
- [Text format](#text-format)
- [Build encoding](#build-encoding)
- [When a dictionary does not load](#when-a-dictionary-does-not-load)
- [Fonts and glyphs](#fonts-and-glyphs)
- [Add a language](#add-a-language)
- [Developer workflow](#developer-workflow)
- [Dictionary tooling](#dictionary-tooling)
- [Checks](#checks)

## Choose the language

`[Overlay]` in `Data/SKSE/Plugins/TheosRenderPipeline.ini`:

```ini
[Overlay]
; Overlay language. auto follows the Windows UI language; en (or an empty
; value) keeps the built-in English labels; any other value names a dictionary
; in TheosRenderPipeline\Lang, e.g. zh-CN. Dictionaries load without a rebuild.
Language=auto
```

| Value | Result |
| --- | --- |
| `auto` | The Windows UI language, as a tag such as `zh-CN`. English when no dictionary matches; the log names the language that had none. |
| `en`, `English`, empty | The built-in English labels; no dictionary is active. |
| `zh-CN`, `zh` | A dictionary, matched in this order: the file name, then `[Language]` Code, then the primary language subtag. |

Matching ignores case, so `ZH-cn` works. The subtag rule means `zh-TW` uses
the `zh-CN` dictionary when it is the only `zh` one; an exact match is always
preferred. A value that matches no dictionary fails: the reason goes to the
log, no dictionary is installed, and the labels keep the language they had —
English at startup.

The overlay offers the same choice with the language button next to Zoom in its
title bar: Automatic (Windows), English, and every dictionary in the folder, each
listed under its `[Language]` `Name` whatever language the menu is in. The button
is greyed while no dictionary is installed, and shows an ochre `!` with the reason
when the requested dictionary could not be loaded. A pick applies at once; the labels and
their glyphs change on the next frame. **Save as default** writes the choice to
`Data/SKSE/Plugins/TheosRenderPipeline.User.ini`, which keeps only what differs
from the packaged file, so updates replace the packaged defaults without losing
the choice; without it the pick lasts for the session. The text beside the combo
names the active language and its string count, and a dictionary that could not
be loaded is reported there in red.

The setting is read before the overlay builds its font atlas, so the first frame
already draws the selected language with its glyphs.

## Dictionary layout

```ini
; Comments start with ; or #.
[Language]
Code = zh-CN
Name = 简体中文
Authors = TheosRenderPipeline translators
Version = 0.5.1
Fonts = msyh.ttc; simhei.ttf

[Strings]
Apply = 应用
" (full)" = "（完整）"

[Strings.OverlayImagePanel]
Normal = 法线
```

`[Language]` describes the dictionary and names the fonts it needs. The
language list shows this metadata.

| Key | Meaning | Default |
| --- | --- | --- |
| `Code` | Language tag, matched by `Language=` and shown by the language list. | The file name without `.ini`. |
| `Name` | Native display name, such as `简体中文`. | The code. |
| `Authors`, `Author` | Free text; credits for the translation. | Empty. |
| `Version` | Free text, usually the plugin version the dictionary was written for. | Empty. |
| `Fonts`, `Font` | Semicolon-separated system fonts for this language. Each entry is an absolute path or a file name under `%WINDIR%\Fonts`. | Empty; the built-in fallbacks apply. |

An unknown `[Language]` key is reported as a warning and ignored.

`[Strings]` maps the English text, written exactly as the source spells it, to
its translation: one `key = value` per line. Keys are case sensitive, so
`On` and `on` are different labels and the dictionary usually holds both.
Whitespace inside a key is part of the key, which is why partial sentences such
as `" (full)"` are quoted.

## Values, quoting and escapes

| Form | Meaning |
| --- | --- |
| `Key = text` | The value is trimmed at both ends. |
| `Key = ""` | Deliberately empty: the label is drawn blank. |
| `Key =` | Not translated yet: the English text is shown and the entry counts as untranslated. |
| `"Key" = "text"` | Quoted on either side: spaces are kept and escapes are decoded. |

Quote a key that starts or ends with a space, contains `=`, or starts with
`[`; a line that starts with `[` is otherwise read as a section header, and the
entry splits at the first `=` outside quotes. Quote a value that starts or ends
with a space, or needs a line break: escapes only apply between quotes.

Escapes are decoded in quoted text only:

| Sequence | Result |
| --- | --- |
| `\\` | One backslash. |
| `\n` | A line break. |
| `\t` | A tab. |
| `\"` | A double quote. |
| `\` + anything else | That character, so `\=` writes `=`. |

A repeated key inside one section keeps the last value and is counted as a
duplicate. The same key in `[Strings]` and in `[Strings.<context>]` is not a
duplicate: the two sections answer different lookups.

## Contexts for shared English text

Some English words mean different things in different panels: `Normal` is a
shading model on the Image tab, an image category and an object-mask group.
Those call sites pass a context, and `[Strings.<context>]` translates that one
use separately. The context is the panel source name, such as
`OverlayImagePanel` or `OverlayAdvancedPanel`.

```ini
[Strings]
Normal = 正常
Off = 关闭

[Strings.OverlayImagePanel]
Normal = 法线
```

A lookup with a context reads its own section first and `[Strings]` second, so
`Off` above is still found from the Image tab. A context section only
overrides; it never replaces the shared entries.

Section names are matched without regard to case, but the context after
`Strings.` and the lookup itself are case sensitive: `[Strings.overlayimagepanel]`
does not answer a lookup for `OverlayImagePanel`.

## Text format

- UTF-8. A byte order mark is skipped. Malformed byte sequences are dropped, so
  a mis-encoded file still loads its readable entries.
- LF and CRLF both work, including a file saved by Notepad with a BOM.
- One entry per line. `;` or `#` starts a comment only at the beginning of a
  line; there are no end-of-line comments.
- `[Language]`, `[Strings]` and `[Strings.<context>]` are matched without
  regard to case. Any other section is reported as unknown and skipped.
- A line without `=` outside quotes, an unterminated section header and an
  unterminated quote are each reported with their file and line number and
  skipped; the rest of the file still loads.

## Build encoding

A translated format string reaches `std::vformat` as UTF-8 at run time, and
`std::vformat` checks the bytes of a `char` format string against the execution
character set. A translation unit built without `/utf-8` uses the system code
page there, so every non-ASCII translation is rejected with
`Invalid encoded character in format string.`

Every target that draws or exercises the overlay, the plugin, its tests and
`TRPOverlayPreview`, therefore compiles with `/utf-8`; see `CMakeLists.txt`.
`Localization::Format` also falls back to the English text when formatting
throws, so a build that loses the flag degrades instead of taking the render
loop down.

## When a dictionary does not load

- A missing file, or a file with no `[Strings]` entries, is treated as absent.
  The earlier language stays active, the log records
  `dictionary not found for '<value>'` as an error, and the activation reports
  false. English is the state at startup, so a mistyped `Language` value leaves
  the overlay in English.
- A structurally broken file loads what it can. Each problem is logged as
  `<file>:<line>: <message>`, for example `zh-CN.ini:12: expected key = value`.
- A file without `[Language]` is known by its file name: `Code` becomes the
  file stem and `Name` becomes the code.
- A dictionary that defines no strings is left out of the language list, so a
  file with every line broken does not appear as a choice.
- Entries the plugin never asks for are harmless; keys the dictionary omits
  keep their English text.

## Fonts and glyphs

The overlay draws with an embedded Roboto that covers Latin, and merges one
system font into the same atlas for everything else, which keeps Latin text at
the embedded font's metrics. The merge tries the active dictionary's
`[Language]` `Fonts` entries in the order written, then the fonts Windows ships
for the languages this plugin runs with: `msyh.ttc`, `msyh.ttf`,
`simhei.ttf`, `YuGothM.ttc`, `meiryo.ttc`, `malgun.ttf` and
`arialuni.ttf`. The first candidate that loads is used; the rest cost only a
failed open. An entry is an absolute path when it contains a separator,
otherwise a file name under `%WINDIR%\Fonts`.

The glyphs requested are the non-ASCII code points the active dictionary's
`[Language]` `Name` and its translations use, so a language costs only the
glyphs it needs. This build's `ImWchar` is 16 bit, so a dictionary with a
character above U+FFFF is logged and that glyph is skipped. Switching language
rebuilds the atlas at the start of the next frame, because the labels of the
frame being drawn still refer to the old glyphs.

When no candidate loads, the log warns and unsupported characters draw as `?`;
the same happens for a character the loaded fonts do not contain. The log also
names the active language after each change, for example
`[Localization] 简体中文 (zh-CN): 1051 strings, 742 glyphs` for the shipped
dictionary, or `[Localization] English (built-in labels)`.

The shipped dictionary asks for `msyh.ttc` and `simhei.ttf`, both installed
with Windows 10 and 11. A language with different needs should list its fonts
first rather than rely on the fallbacks.

## Add a language

1. Copy `package/SKSE/Plugins/TheosRenderPipeline/Lang/zh-CN.ini` to
   `Data/SKSE/Plugins/TheosRenderPipeline/Lang/<code>.ini`, for example
   `ja-JP.ini`. The file name does not have to equal `Code`, but keeping them
   the same makes `Language=` obvious.
2. Change `Code`, `Name`, `Authors`, `Version` and `Fonts` in
   `[Language]`. For Japanese that is typically `meiryo.ttc; msgothic.ttc`.
3. Translate the values in `[Strings]` and, where the dictionary has them, in
   `[Strings.<context>]`. Leave the keys exactly as they are: they identify the
   English source text, not the translation.
4. Untranslated entries can stay as `Key =`, which shows English until the
   translation is filled in, or with `""` when the label should be blank.
5. Save the file as UTF-8. A BOM is optional; the runtime skips it.
6. Start the game and pick the language with the button next to Zoom, or
   set `[Overlay] Language` to the new code. Check every tab for text that is
   still English, clipped, or drawn as `?`, and fix the dictionary rather than
   the plugin.

A dictionary copied into the folder while the game runs is listed after the
next language change, which rescans the folder; restart the game to see it
immediately.

Ship a translation as its own mod folder with the same relative path, so MO2
merges only the `Lang` folder. A translation package should not replace
`TheosRenderPipeline.ini`, which would also override the reader's other
settings.

## Developer workflow

Every user-visible label is written in English in the source and wrapped where
it is used, so the source stays readable and the dictionary keeps a stable key.

| Form | Use |
| --- | --- |
| `TR("text")` | A plain label. |
| `TRC("text", "context")` | The same English text means different things in different panels. |
| `TRF("fmt {}", args...)` | A `std::format` string; translate it and keep the braces. |
| `TRFC("context", "fmt {}", args...)` | Both of the above. |

`TRF` formats the translated string with `std::vformat`, because a translated
format string cannot be checked at compile time. A dictionary must therefore
repeat the English placeholders exactly; a mismatched list throws
`std::format_error` at that call.

Strings handed to `std::snprintf` or an ImGui format argument are wrapped in
`TR`, not `TRF`, and the translation must keep each conversion specifier:

```cpp
std::snprintf(view.upscaleDetail, sizeof(view.upscaleDetail), TR("%.0f%% | Preset %s"), value, preset);
```

Text that is not shown to the player stays in English and unwrapped: INI keys,
file names, log-only lines and technical tokens such as `DLSS`, `XeSS` or
`NGX` preset letters.

A `constexpr` table keeps its English literals, because `TR` runs at runtime.
Wrap the literal where it is drawn instead, and let the same literal stay
untranslated where it is only logged:

```cpp
// src/FrameGen/NeuralObjectMask.h
inline constexpr const char* kObjectGroups[]{"First-person arms / weapons", "Player body / equipment",
    "Other humanoids / equipment", "Creatures"};

// src/OverlayNeuralPanel.cpp — the label the player reads
ImGui::SliderFloat(TR(NR::kObjectGroups[i]), &masks.strength[i], 0.0f, 1.0f, "%.2f");
```

`src/DLSSPreset.h` follows the same rule: the `constexpr` `entries` table holds
the English labels and `Label()` returns `TR(entry->label)`.

After adding a wrapped string, add its English text to each dictionary, as a
`Key =` line while it is untranslated; the label keeps showing English until
the value is filled in, so the order of the two steps does not matter. When the
string came through the dictionary tooling, `check` reports a key the dictionary
lacks as `MISSING`.

## Dictionary tooling

`tools/lang/localize.py` keeps the sources and the shipped dictionary in step.
Two workflows use it. When no translated copy exists yet, `extract` and
`dictionary` build the dictionary from a translated revision of the sources.
Once a dictionary exists, `wrap` is the usual path: it matches the English
literals of the working tree against the dictionary's keys and wraps the ones it
finds, so a new upstream release costs only the labels the dictionary lacks.

```powershell
python tools/lang/localize.py wrap         # wrap literals the dictionary knows
python tools/lang/localize.py audit        # keys the sources ask for, keys unused, text left in English
python tools/lang/localize.py extract      # pair the literals of HEAD^ and HEAD
python tools/lang/localize.py dictionary   # write the shipped dictionary from those pairs
python tools/lang/localize.py rewrite      # wrap the HEAD^ English literals in TR
python tools/lang/localize.py check        # compare dictionary, pairs and sources
python tools/lang/localize.py render       # translations the sources no longer produce
```

| Command | Work |
| --- | --- |
| `extract` | Aligns the two revisions line by line, treats unchanged lines as anchors and pairs the literals of each changed hunk. Writes `pairs.json` and `report.txt`. |
| `dictionary` | Groups the pairs into one entry per (English, context). A key translated differently in different files becomes a `[Strings.<file stem>]` entry. Writes the shipped `zh-CN.ini` and prints `CONFLICT` lines for a key that is still ambiguous. |
| `rewrite` | Wraps the `HEAD^` English literals in `TR`, `TRC` or `TRF` and writes the result into the working tree. `--dry-run` only lists the files. |
| `check` | Reports `MISSING`, `MISMATCH` and `CJK LEFT` per pair, writes `check-report.txt` and lists dictionary keys the sources never use. Exits non-zero when it finds a problem. |
| `render` | Lists translated strings the current sources no longer produce, for translations that were dropped or reworded. Writes `render-report.txt`. |
| `wrap` | Matches the working tree's English literals against the dictionary keys and wraps them in `TR`, `TRC`, `TRF` or `TRFC`. Skips text that is already wrapped, `constexpr` tables, `static_assert` messages and log calls; unmatched candidates go to `wrap-missing.json` for a translator. |
| `audit` | Cross-checks the repository: keys the sources wrap that the dictionary lacks, entries no source asks for, and prose still left in English. Writes `audit-report.txt`. |

`report.txt` holds the lines a person has to look at: unpaired English or
Chinese literals, added or removed text, edits without CJK characters, and files
whose code changed enough that the pairing is suspect.

`tools/lang/review/*.json` holds the reviewed wording as
`{"file", "english", "chinese", "context"}` records, one file per review pass.
`tools/lang/merge_review.py` merges them into the dictionary, rewrites a key
that already exists and appends the rest to `[Strings]` or
`[Strings.<context>]`, then sorts each section by its English key so the diff
stays readable. A reviewed value wins over the automatic pairing in `check`.

## Checks

`TRPLocalizationTests` (CMake target `TRPLocalizationTests`, CTest name
`Localization`) covers dictionary metadata, quoting and escapes, deliberate
empty values, duplicates, broken lines and their line numbers, context
overrides, translated formatting, glyph collection and activation from a
temporary `Lang` folder.

The target exists when the standalone checks are enabled:

```powershell
cmake --preset arp-nvidia "-DTRP_BUILD_COMPATIBILITY_TESTS=ON"
cmake --build out/build/nvidia --config Release --target TRPLocalizationTests
out/build/nvidia/Release/TRPLocalizationTests.exe
```

Or run it through CTest:
`ctest --test-dir out/build/nvidia -C Release -R Localization --output-on-failure`.

The test deletes and recreates `Data/SKSE/Plugins/TheosRenderPipeline/Lang` in
its working directory, so run it from the build folder that CTest uses rather
than from a game install.
