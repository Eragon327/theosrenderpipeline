# Language tooling

`localize.py` keeps the shipped overlay dictionary and the source literals in
step. How dictionaries look and how the runtime uses them is described in
[LOCALIZATION.md](../../LOCALIZATION.md); this file covers the scripts in this
folder.

Python 3 standard library only, run from the repository root.

There are two workflows. A dictionary that already exists is extended with
`wrap`, which matches the working tree's English literals against the
dictionary keys, so a new upstream release only costs the labels the dictionary
still lacks. Only when no dictionary exists yet does the tooling fall back to
pairing a translated revision (`HEAD`) with its English parent (`HEAD^`).

```powershell
python tools/lang/localize.py wrap         # wrap literals the dictionary knows
python tools/lang/localize.py audit        # dictionary against sources, both ways
python tools/lang/localize.py check        # compare dictionary, pairs and sources
python tools/lang/localize.py render       # translations the sources no longer produce
python tools/lang/localize.py extract      # pair the literals of HEAD^ and HEAD
python tools/lang/localize.py dictionary   # write the shipped dictionary from those pairs
python tools/lang/localize.py rewrite      # wrap the HEAD^ English literals in TR
```

## Commands

`wrap` is the working command. It reads each literal run of the working tree,
looks the decoded text up in the dictionary (the file-stem context first, then
the global entry) and wraps what it finds in `TR`, `TRC`, `TRF` or `TRFC`
(`std::format`/`fmt::format` call sites become `TRF`). It skips text that is
already wrapped, multi-piece runs such as `"a " "b"` (join them first), raw
multi-line strings, text inside `constexpr` or `static_assert` (C2131/C2061),
and calls whose name says the string is not shown to the player - `logger::`,
`SKSE::log::`, `printf` family and `*report_and_fail`. Candidates that match no
key are written to `wrap-missing.json` for a translator. Running it twice is
safe: the second run reports every hit as already wrapped.

**Wrap only the menu sources (`src/Overlay*`).** Backend status text, settings keys and log
lines stay English; the menu translates backend text where it shows it.

**Never wrap a string that is read back.** INI section names and keys
(`ini.GetValue("Settings", ...)`, `GetSection("Preset")`, `SetLongValue(...)`),
weather profile names, file names, enum spellings and anything compared against
data from disk must stay English - the dictionary only changes what the player
reads. A wrapped key silently points at a section that no longer exists once a
translation is active, and `audit` cannot see it, so after every `wrap` run grep
for `TR(` next to `GetValue`, `SetValue`, `SetLongValue`, `GetSection` and
`GetLongValue`, and re-check comparisons whose left side comes from disk
(`TR` on both sides is only correct when the value was assigned through `TR`).

`audit` cross-checks the repository in one pass: keys the sources wrap that the
dictionary lacks (`missing`), dictionary entries no source asks for
(`unused`), and prose still left in English (`unwrapped`, filtered to skip
includes, paths, format fields and single tokens). `unused` entries are usually
text an upstream release reworded; they are harmless but can be deleted.
Writes `audit-report.txt`.

`extract` aligns the two revisions line by line, uses unchanged lines as
anchors, and pairs the literals of each changed hunk, so a reflowed translation
does not shift every later pair. Adjacent literals (`"a" "b"`) are read as one
run. Writes `pairs.json` and `report.txt`.

`dictionary` groups the pairs into one entry per (English, context). A key
translated differently in different files becomes a `[Strings.<file stem>]`
entry; a key still ambiguous inside one file is printed as `CONFLICT`. Writes
`package/SKSE/Plugins/TheosRenderPipeline/Lang/zh-CN.ini`.

`rewrite` re-reads `HEAD^`, wraps each paired literal in `TR`, `TRC` or `TRF`
(`std::format`/`fmt::format` call sites become `TRF`), and writes the result
into the working tree, replacing any uncommitted edits in those files.
`--dry-run` lists the files instead. Do not run it again after a review round:
it rebuilds every file from `HEAD^` and would drop hand-written code that the
translation commit added.

`check` is the gate for the pairing workflow. It walks `pairs.json`, resolves
each key the way the wrapper does and reports:

* `MISSING` - the dictionary has no entry for a paired key;
* `PLACEHOLDER` - the translation has more `{}` or `%` fields than the English
  text, which would read arguments that are not there;
* `MISMATCH` - the value differs from the pairing (see below);
* `CJK LEFT` - a translated literal is still hard-coded in a source file;
* `MISSING FILE` - a file changed since `HEAD` disappeared.

`tools/lang/review/*.json` overrides the pairing: once a reviewer has decided
what a string means, that value wins over the automatically aligned one, so the
`MISMATCH` line means "the dictionary was edited after extraction" rather than
"the dictionary is wrong". Values ending up identical are silent. The command
also lists dictionary keys no source literal uses; keys translated at the
display site through `TR(<variable>)` are listed in `DISPLAY_ONLY` inside the
script so they do not show up as stale. Writes `check-report.txt` and exits
non-zero on a problem.

`render` compares the translation commit with the working tree: `HEAD`-only
strings that no pair covers and no dictionary value reproduces are real gaps,
while reworded translations are only counted, since a review round changes them
on purpose. Writes `render-report.txt`.

`merge_review` folds the reviewer files into the shipped dictionary - existing
keys are rewritten in place, missing ones are appended to their section, and the
sections are re-sorted so the file stays diff-friendly. Keys and values are
escaped, so a label that spans lines stays on one dictionary line as `\n`.

## Files

| File | Content |
| --- | --- |
| `pairs.json` | One record per paired literal: `file`, `english`, `chinese`, `raw`, `pieces`, `index`, `suspect`, `line`. |
| `report.txt` | The lines a person has to read: unpaired English or Chinese literals, added or removed text, edits without CJK characters, files whose code changed enough that the pairing is suspect, missing revisions. |
| `wrap-missing.json` | Literals `wrap` could not match: `file`, `line`, `text`. The translation shortlist for a new release. |
| `audit-report.txt` | Written by `audit`: missing, unused and unwrapped counts with the lines behind them. |
| `review/<pass>.json` | Reviewer wording: `file`, `english`, `chinese`, `context`. Consumed by `check` and `merge_review`. |
| `check-report.txt`, `render-report.txt` | Written by `check` and `render`. |

## Order of work

Carrying the dictionary to a new upstream release:

1. Port the plumbing (`src/Localization.*`, the overlay hooks, the INI key, the
   test and the CMake entries) onto the new tag.
2. `wrap`; every literal the dictionary already knows is wrapped in one pass.
3. `audit`; translate the `unwrapped` prose the player can see, read the panel
   files first, and remember that internal status and diagnostic strings stay in
   English.
4. Write the new wording into `review/<pass>.json`, run `merge_review`, then
   `wrap` again so the new keys are picked up.
5. `check` and `audit`; both must report nothing that matters, and
   `TRPLocalizationTests` must pass.

Building a dictionary from a translated revision, as 0.4.5 was:

1. Translate a copy of the sources in a commit, keeping the code structure and
   the literals' positions unchanged.
2. `extract`, then read `report.txt` for pairs the alignment could not make.
3. `dictionary` for the shipped file, then `rewrite` for the English sources.
4. `check`; fix the dictionary until it exits zero.
5. Review the dictionary bucket by bucket and write the corrections into
   `review/<pass>.json`, then `merge_review` and `check` again.
6. After later source edits, `render` shows translations that were lost.
