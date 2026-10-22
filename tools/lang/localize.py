#!/usr/bin/env python3
"""Localization tooling for TheosRenderPipeline.

The English UI text lives in the parent of the localization commit; the
translated text lives in that commit itself.  This tool pairs the two, writes
the shipped dictionary from the pairs, and rewrites the sources so they call
the Localization runtime instead of carrying literals.
"""

import argparse
import difflib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASE_REV = "HEAD^"
TIP_REV = "HEAD"
PAIRS = ROOT / "tools" / "lang" / "pairs.json"
REPORT = ROOT / "tools" / "lang" / "report.txt"
DICT = ROOT / "package" / "SKSE" / "Plugins" / "TheosRenderPipeline" / "Lang" / "zh-CN.ini"
BS = chr(92)
QUOTE = chr(34)
NL = chr(10)


def git_show(rev, path):
    done = subprocess.run(["git", "show", rev + ":" + path], cwd=str(ROOT), capture_output=True)
    if done.returncode != 0:
        return None
    return done.stdout.decode("utf-8")


def changed_files():
    done = subprocess.run(["git", "diff", "--name-only", BASE_REV, TIP_REV], cwd=str(ROOT),
                          capture_output=True, text=True, encoding="utf-8")
    names = []
    for line in done.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith("src/") or line.startswith("include/"):
            names.append(line)
    return names


def scan_raw(text):
    """Split C++ source into a skeleton and the string literals it holds.

    Returns (skeleton, literals) where literals are (start, end, inner, raw)
    tuples in source order and the skeleton has every literal replaced by
    '<LIT>' so two versions of one file can be aligned literal by literal.
    """
    literals = []
    pieces = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find(NL, i)
            if j < 0:
                j = n
            pieces.append(text[i:j])
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            pieces.append(text[i:j])
            i = j
            continue
        if c == "R" and text.startswith('R"', i):
            k = i + 2
            delim = ""
            while k < n and text[k] not in "(" + NL and (k - (i + 2)) < 16:
                delim += text[k]
                k += 1
            if k < n and text[k] == "(":
                close = ")" + delim + QUOTE
                j = text.find(close, k + 1)
                if j < 0:
                    raise ValueError("unterminated raw string")
                literals.append((i, j + len(close), text[k + 1:j], True))
                pieces.append("<LIT>")
                i = j + len(close)
                continue
        if c == QUOTE:
            j = i + 1
            while j < n:
                if text[j] == BS:
                    j += 2
                    continue
                if text[j] == QUOTE:
                    break
                j += 1
            literals.append((i, j + 1, text[i + 1:j], False))
            pieces.append("<LIT>")
            i = j + 1
            continue
        if c == chr(39):
            j = i + 1
            while j < n:
                if text[j] == BS:
                    j += 2
                    continue
                if text[j] == chr(39):
                    break
                j += 1
            pieces.append("<CHR>")
            i = j + 1
            continue
        pieces.append(c)
        i += 1
    return "".join(pieces), literals


def merge_runs(text, literals):
    """Join adjacent literals ("a" "b") into one writable run."""
    runs = []
    for start, end, inner, raw in literals:
        if runs:
            previous = runs[-1]
            if text[previous[1]:start].strip() == "":
                runs[-1] = (previous[0], end, previous[2] + inner, previous[3] or raw, previous[4] + 1)
                continue
        runs.append((start, end, inner, raw, 1))
    return runs


def scan(text):
    """Skeleton plus adjacent-literal runs, aligned index by index."""
    skeleton, literals = scan_raw(text)
    return skeleton, merge_runs(text, literals)


def has_cjk(text):
    for ch in text:
        code = ord(ch)
        if 0x3040 <= code <= 0x30FF or 0x3400 <= code <= 0x4DBF or 0x4E00 <= code <= 0x9FFF \
                or 0xF900 <= code <= 0xFAFF or 0xFF01 <= code <= 0xFF60:
            return True
    return False


def line_map(text):
    """Line starts, so a character offset can be reported as a line number."""
    lines = text.split(NL)
    offsets = []
    position = 0
    for line in lines:
        offsets.append(position)
        position += len(line) + 1
    offsets.append(position)
    return lines, offsets


def runs_within(runs, begin, end):
    return [index for index, run in enumerate(runs) if run[0] >= begin and run[1] <= end]


def pair_literals(name, text_e, runs_en, text_z, runs_zh, report):
    """Pair literals hunk by hunk, using unchanged lines as anchors.

    Line-level anchoring keeps a reflowed translation from shifting every
    later pair; hunks whose literal counts disagree are flagged as suspect.
    """
    lines_e, offset_e = line_map(text_e)
    lines_z, offset_z = line_map(text_z)
    matcher = difflib.SequenceMatcher(None, lines_e, lines_z, autojunk=False)
    pairs = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        left = runs_within(runs_en, offset_e[i1], offset_e[i2])
        right = runs_within(runs_zh, offset_z[j1], offset_z[j2])
        if tag == "delete":
            for index in left:
                report.append("REMOVED ENGLISH %s:%d: %r" % (name, i1 + 1, runs_en[index][2]))
            continue
        if tag == "insert":
            for index in right:
                report.append("ADDED CHINESE %s:%d: %r" % (name, j1 + 1, runs_zh[index][2]))
            continue
        shared = min(len(left), len(right))
        suspect = len(left) != len(right)
        for step in range(shared):
            pairs.append((left[step], right[step], suspect, i1 + 1))
        for index in left[shared:]:
            report.append("UNPAIRED ENGLISH %s:%d: %r" % (name, i1 + 1, runs_en[index][2]))
        for index in right[shared:]:
            report.append("UNPAIRED CHINESE %s:%d: %r" % (name, j1 + 1, runs_zh[index][2]))
    return pairs


def cmd_extract(args):
    files = changed_files()
    report = []
    all_pairs = []
    for name in files:
        english = git_show(BASE_REV, name)
        chinese = git_show(TIP_REV, name)
        if english is None or chinese is None:
            report.append("SKIP (missing revision): " + name)
            continue
        skel_en, runs_en = scan(english)
        skel_zh, runs_zh = scan(chinese)
        if skel_en != skel_zh:
            report.append("SKELETON DIFFERS: " + name)
        pairs = pair_literals(name, english, runs_en, chinese, runs_zh, report)
        for index, other, suspect, line in pairs:
            inner_en = runs_en[index][2]
            inner_zh = runs_zh[other][2]
            if inner_en == inner_zh or not inner_en.strip():
                continue
            if not has_cjk(inner_zh):
                report.append("NON-CJK EDIT %s: %r -> %r" % (name, inner_en, inner_zh))
            all_pairs.append({
                "file": name,
                "english": inner_en,
                "chinese": inner_zh,
                "raw": runs_en[index][3],
                "pieces": runs_en[index][4],
                "index": index,
                "suspect": suspect,
                "line": line,
            })
    PAIRS.write_text(json.dumps(all_pairs, ensure_ascii=False, indent=1), encoding="utf-8")
    REPORT.write_text(NL.join(report) + NL, encoding="utf-8")
    unique_english = {p["english"] for p in all_pairs}
    print("files: %d, pairs: %d, unique english keys: %d" % (len(files), len(all_pairs), len(unique_english)))
    print("report lines: %d -> %s" % (len(report), REPORT))
    return 0


def unique(values):
    seen = []
    for value in values:
        if value not in seen:
            seen.append(value)
    return seen


def plan_entries(pairs):
    """One dictionary entry per (english, context) covering every pair."""
    by_english = {}
    order = []
    for pair in pairs:
        if pair["english"] not in by_english:
            by_english[pair["english"]] = []
            order.append(pair["english"])
        by_english[pair["english"]].append(pair)
    entries = []
    conflicts = {}
    for english in order:
        group = by_english[english]
        values = unique([p["chinese"] for p in group])
        if len(values) == 1:
            entries.append({"english": english, "chinese": values[0], "context": "", "raw": group[0]["raw"]})
            conflicts[english] = []
            continue
        conflicts[english] = values
        per_file = {}
        for pair in group:
            context = Path(pair["file"]).stem
            per_file.setdefault(context, []).append(pair)
        for context, items in per_file.items():
            inner = unique([p["chinese"] for p in items])
            if len(inner) > 1:
                conflicts[english].append("AMBIGUOUS IN " + context)
            entries.append({"english": english, "chinese": inner[0], "context": context, "raw": items[0]["raw"]})
    return entries, conflicts


def dict_text(value, raw):
    """Encode a literal body for the dictionary file.

    Both the key and the value go through the same escaping so a label that
    spans lines stays on one dictionary line as \\n.  The C++ parser decodes it
    back into the newline the source carries.
    """
    value = value.replace(BS, BS + BS).replace(QUOTE, BS + QUOTE)
    value = value.replace(NL, BS + "n").replace(chr(9), BS + "t")
    value = value.replace(chr(13), "")
    return QUOTE + value + QUOTE


def cmd_dictionary(args):
    pairs = json.loads(PAIRS.read_text(encoding="utf-8"))
    entries, conflicts = plan_entries(pairs)
    plain = [e for e in entries if not e["context"]]
    scoped = [e for e in entries if e["context"]]
    plain.sort(key=lambda e: e["english"].lower())
    scoped.sort(key=lambda e: (e["context"].lower(), e["english"].lower()))
    lines = [
        "; TheosRenderPipeline dictionary: Simplified Chinese (zh-CN).",
        ";",
        "; Edit the right-hand side only.  Keys are the English text used in the",
        "; source; a quoted empty string keeps the line blank on purpose, while an",
        "; unquoted empty value means \"not translated yet\" and shows English.",
        ";",
        "; Sections of the form [Strings.<context>] override a key whose English",
        "; text means different things in different panels.",
        "",
        "[Language]",
        "Code = zh-CN",
        "Name = 简体中文",
        "Authors = TheosRenderPipeline translators",
        "Version = 0.5.1",
        "Fonts = msyh.ttc; simhei.ttf",
        "",
        "[Strings]",
    ]
    for entry in plain:
        lines.append(dict_text(entry["english"], entry["raw"]) + " = " + dict_text(entry["chinese"], entry["raw"]))
    context = None
    for entry in scoped:
        if entry["context"] != context:
            context = entry["context"]
            lines.append("")
            lines.append("[Strings." + context + "]")
        lines.append(dict_text(entry["english"], entry["raw"]) + " = " + dict_text(entry["chinese"], entry["raw"]))
    DICT.parent.mkdir(parents=True, exist_ok=True)
    DICT.write_text(NL.join(lines) + NL, encoding="utf-8")
    print("dictionary entries: %d (scoped: %d) -> %s" % (len(entries), len(scoped), DICT))
    troubled = {k: v for k, v in conflicts.items() if v}
    for english, notes in sorted(troubled.items()):
        print("CONFLICT %r -> %s" % (english, "; ".join(notes)))
    return 0


def unquote_dict(text):
    if len(text) < 2 or text[0] != QUOTE or text[-1] != QUOTE:
        return text
    out = []
    index = 1
    while index < len(text) - 1:
        ch = text[index]
        if ch == BS and index + 1 < len(text) - 1:
            index += 1
            code = text[index]
            out.append({"n": NL, "t": chr(9), BS: BS, QUOTE: QUOTE}.get(code, code))
            index += 1
            continue
        out.append(ch)
        index += 1
    return "".join(out)


def load_dictionary(path):
    """Minimal reader for the dictionary format, mirroring the C++ parser."""
    values = {}
    section = "strings"
    for number, raw_line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        if line.startswith("["):
            end = line.find("]")
            if end < 0:
                continue
            section = line[1:end].strip().lower()
            continue
        if section != "strings" and not section.startswith("strings."):
            continue
        position = -1
        quoted = False
        for index, ch in enumerate(line):
            if ch == QUOTE:
                quoted = not quoted
            elif ch == "=" and not quoted:
                position = index
                break
        if position < 0:
            continue
        key = unquote_dict(line[:position].strip())
        value = unquote_dict(line[position + 1:].strip())
        context = section[len("strings."):] if section.startswith("strings.") else ""
        values[(key, context)] = value
    return values


def lookup(values, english, context):
    if context and (english, context) in values:
        return values[(english, context)]
    return values.get((english, ""))

def format_callee(text, start):
    """Return the span of a std::format/fmt::format callee right before start."""
    index = start
    while index > 0 and text[index - 1] in " \t\r\n":
        index -= 1
    if index == 0 or text[index - 1] != "(":
        return None
    index -= 1
    while index > 0 and text[index - 1] in " \t\r\n":
        index -= 1
    begin = index
    while begin > 0 and (text[begin - 1].isalnum() or text[begin - 1] in "_:"):
        begin -= 1
    name = text[begin:index]
    if name in ("std::format", "fmt::format"):
        return (begin, index)
    return None


def cmd_rewrite(args):
    pairs = json.loads(PAIRS.read_text(encoding="utf-8"))
    entries, conflicts = plan_entries(pairs)
    values = {}
    for entry in entries:
        values[(entry["english"], entry["context"])] = entry["chinese"]
    by_file = {}
    for pair in pairs:
        by_file.setdefault(pair["file"], {})[pair["index"]] = pair
    wrapped = 0
    formatted = 0
    for name, wanted in sorted(by_file.items()):
        english = git_show(BASE_REV, name)
        if english is None:
            print("missing english source: " + name)
            continue
        skel, literals = scan(english)
        edits = []
        for index, item in enumerate(literals):
            start, end, inner, raw, pieces = item
            pair = wanted.get(index)
            if pair is None:
                continue
            stem = Path(name).stem
            context = stem if (pair["english"], stem) in values else ""
            literal = english[start:end]
            callee = format_callee(english, start)
            if callee is not None:
                edits.append((callee[0], callee[1], "TRF"))
                formatted += 1
                continue
            if context:
                edits.append((start, end, "TRC(" + literal + ", " + QUOTE + context + QUOTE + ")"))
            else:
                edits.append((start, end, "TR(" + literal + ")"))
            wrapped += 1
        edits.sort()
        out = []
        cursor = 0
        for start, end, replacement in edits:
            out.append(english[cursor:start])
            out.append(replacement)
            cursor = end
        out.append(english[cursor:])
        text = "".join(out)
        target = ROOT / name
        if args.dry_run:
            print("would rewrite " + name + " (" + str(len(edits)) + " edits)")
            continue
        target.write_text(text, encoding="utf-8", newline="")
    print("wrapped literals: %d, format calls: %d" % (wrapped, formatted))
    return 0


def placeholders(text):
    """Format fields a translation must not invent: std::format braces and printf."""
    braces = re.findall(r"\{[^{}]*\}", text)
    percents = re.findall(r"%(?:[-+ #0]*\d*(?:\.\d+)?(?:ll|l|h|z)?[diouxXeEfgGscp])", text)
    return len(braces), len(percents)


def placeholder_problem(english, translated):
    """A translation may drop a field, never add one it has no argument for."""
    if translated is None:
        return None
    want_braces, want_percents = placeholders(english)
    got_braces, got_percents = placeholders(translated)
    if got_braces > want_braces:
        return "braces %d > %d" % (got_braces, want_braces)
    if got_percents > want_percents:
        return "printf %d > %d" % (got_percents, want_percents)
    return None


def reviewed_values():
    """Translations a reviewer pinned in tools/lang/review/*.json.

    The auto-aligned pairs are only a first guess; once a reviewer has decided
    what a string means, the review files win over the pairing heuristics.
    """
    out = {}
    directory = ROOT / "tools" / "lang" / "review"
    for path in sorted(directory.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:  # noqa: BLE001 - a broken review file is not fatal
            continue
        for item in data:
            key = item.get("english", "")
            if not key.strip():
                continue
            out[(key, (item.get("context") or "").lower())] = item.get("chinese", "")
    return out


def expected_value(pair, reviewed):
    """The translation the dictionary should hold for one extracted pair."""
    key = decode_literal(pair["english"], pair["raw"])
    for context in (Path(pair["file"]).stem.lower(), ""):
        if (key, context) in reviewed:
            return key, context, reviewed[(key, context)]
    return key, None, decode_literal(pair["chinese"], pair["raw"])


# Keys translated at the display site through TR(variable) - the literal itself
# lives in a shared constexpr table, so the scan above never sees it.  Keep the
# list short and specific; anything else that looks unused is a stale entry.
DISPLAY_ONLY = {
    "Creatures",
    "First-person arms / weapons",
    "Other humanoids / equipment",
    "Player body / equipment",
}


def cmd_check(args):
    values = load_dictionary(DICT)
    reviewed = reviewed_values()
    pairs = json.loads(PAIRS.read_text(encoding="utf-8"))
    problems = []
    for pair in pairs:
        key, pinned, expected = expected_value(pair, reviewed)
        stem = Path(pair["file"]).stem.lower()
        context = stem if (key, stem) in values else ""
        found = lookup(values, key, context)
        if found is None:
            problems.append("MISSING  " + pair["file"] + ": " + repr(pair["english"])[:80])
        elif placeholder_problem(key, found) is not None:
            problems.append("PLACEHOLDER " + pair["file"] + ": " + repr(pair["english"])[:50]
                            + " -> " + repr(found)[:40] + " (" + placeholder_problem(key, found) + ")")
        elif found != expected:
            problems.append("MISMATCH " + pair["file"] + ": " + repr(pair["english"])[:60] + " -> "
                            + repr(found)[:40] + (" wanted " + repr(expected)[:40]
                                                  + (" (review)" if pinned is not None else "")))
    files = changed_files()
    used = set()
    for name in files:
        path = ROOT / name
        if not path.exists():
            problems.append("MISSING FILE " + name)
            continue
        text = path.read_text(encoding="utf-8")
        skel, literals = scan(text)
        for start, end, inner, raw, count in literals:
            if has_cjk(inner):
                problems.append("CJK LEFT " + name + ": " + repr(inner)[:70])
            used.add(decode_literal(inner, raw))
    unused = [key for key in values if key[1] == "" and key[0] not in used and key[0] not in DISPLAY_ONLY]
    check_report = ROOT / "tools" / "lang" / "check-report.txt"
    check_report.write_text(NL.join(problems) + NL, encoding="utf-8")
    print("checked %d files, %d pairs" % (len(files), len(pairs)))
    print("problems: %d -> %s" % (len(problems), check_report))
    print("dictionary keys never used in source: %d" % len(unused))
    for key in unused[:20]:
        print("  UNUSED " + repr(key[0])[:80])
    return 1 if problems else 0


def decode_literal(inner, raw):
    """Turn a C++ literal body into the text the program sees."""
    if raw:
        return inner
    simple = {"n": NL, "t": chr(9), "r": chr(13), BS: BS, QUOTE: QUOTE,
              "a": chr(7), "b": chr(8), "f": chr(12), "v": chr(11), "0": chr(0)}
    out = []
    index = 0
    while index < len(inner):
        ch = inner[index]
        if ch != BS or index + 1 >= len(inner):
            out.append(ch)
            index += 1
            continue
        code = inner[index + 1]
        if code == "x":
            # Consecutive \xNN bytes form one UTF-8 sequence, exactly what the
            # compiler hands the runtime.
            buffer = bytearray()
            j = index
            while j + 3 < len(inner) + 1 and inner[j] == BS and inner[j + 1] == "x":
                digits = inner[j + 2:j + 4]
                if len(digits) == 2 and all(c in "0123456789abcdefABCDEF" for c in digits):
                    buffer.append(int(digits, 16))
                    j += 4
                else:
                    break
            if buffer:
                out.append(buffer.decode("utf-8", "replace"))
                index = j
                continue
        if code in ("u", "U"):
            limit = {"u": 4, "U": 8}[code]
            j = index + 2
            digits = ""
            while j < len(inner) and inner[j] in "0123456789abcdefABCDEF" and len(digits) < limit:
                digits += inner[j]
                j += 1
            if digits:
                value = int(digits, 16)
                if value <= 0x10FFFF:
                    out.append(chr(value))
                    index = j
                    continue
        out.append(simple.get(code, code))
        index += 2
    return "".join(out)


def wrapper_context(text, start, end):
    """Kind and context of the TR wrapper around one literal run, if any."""
    prefix = text[:start].rstrip()
    for name in ("TRFC(", "TRC(", "TRF(", "TR("):
        if prefix.endswith(name):
            context = ""
            if name in ("TRC(", "TRFC("):
                tail = text[end:].lstrip()
                if tail.startswith(","):
                    tail = tail[1:].lstrip()
                    if tail.startswith(QUOTE):
                        close = tail.find(QUOTE, 1)
                        if close > 0:
                            context = tail[1:close]
            return name[:-1], context
    return None, None


def cmd_render(args):
    """Compare the Chinese commit with what the rebuilt sources can still show.

    Two things are worth knowing after a review round:

    * text the Chinese commit shipped that no extracted pair covers and that no
      dictionary entry reproduces - the only case that is a real gap;
    * translations a reviewer reworded.  Those differ from the Chinese commit on
      purpose, so they are counted, not listed.
    """
    values = load_dictionary(DICT)
    pairs = json.loads(PAIRS.read_text(encoding="utf-8"))
    covered = {}
    for pair in pairs:
        covered.setdefault(pair["file"], set()).add(pair["index"])
    values_seen = set(values.values())
    reworded = 0
    for pair in pairs:
        expected = decode_literal(pair["chinese"], pair["raw"])
        key = decode_literal(pair["english"], pair["raw"])
        stem = Path(pair["file"]).stem.lower()
        found = lookup(values, key, stem if (key, stem) in values else "")
        if found is not None and found != expected:
            reworded += 1
    report = []
    total = 0
    for name in changed_files():
        chinese = git_show(TIP_REV, name)
        if chinese is None:
            continue
        _, runs_zh = scan(chinese)
        covered_indexes = covered.get(name, set())
        missing = []
        for index, (start, end, inner, raw, pieces) in enumerate(runs_zh):
            if index in covered_indexes:
                continue
            value = decode_literal(inner, raw)
            if not has_cjk(value):
                continue
            if value in values_seen:
                continue
            missing.append(value)
        if missing:
            total += len(missing)
            report.append("== " + name + " (%d)" % len(missing))
            for value in sorted(missing)[:40]:
                report.append("   " + repr(value)[:110])
    header = ["HEAD-only strings with no dictionary entry: %d" % total,
              "reworded translations (cleared by review): %d" % reworded]
    out = ROOT / "tools" / "lang" / "render-report.txt"
    out.write_text(NL.join(header + report) + NL, encoding="utf-8")
    print("HEAD-only strings with no dictionary entry: %d" % total)
    print("reworded translations (cleared by review): %d" % reworded)
    print("report -> %s" % out)
    return 0


# Callees whose text stays English: the plugin log and the fatal start-up dialog
# are read by whoever debugs a report, not by players.
LOG_CALLEES = (
    "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsnprintf",
    "puts", "fputs", "OutputDebugStringA", "OutputDebugStringW",
)


def call_name(text, start):
    """Name of the call whose argument starts at this offset, or ""."""
    index = start
    while index > 0 and text[index - 1] in " \t\r\n":
        index -= 1
    if index == 0 or text[index - 1] != "(":
        return ""
    index -= 1
    while index > 0 and text[index - 1] in " \t\r\n":
        index -= 1
    end = index
    while index > 0 and (text[index - 1].isalnum() or text[index - 1] in "_:"):
        index -= 1
    return text[index:end]


def english_only(name):
    if not name:
        return False
    if name.startswith("logger::") or name.startswith("SKSE::log::"):
        return True
    if name.startswith("LOG"):
        return True
    if name in LOG_CALLEES:
        return True
    return name.endswith("report_and_fail")


def in_constexpr(text, start):
    """A literal inside a constant expression cannot call TR: the wrapper is not
    a constant expression, and the C++ compiler rejects it outright."""
    index = start
    while index > 0 and text[index - 1] != ";":
        index -= 1
    window = text[index:start]
    if len(window) > 600:
        window = window[-600:]
    return "constexpr" in window or "static_assert" in window


def obvious_text(body):
    """Cheap screen for strings no dictionary should hold."""
    if not body or body.strip() != body:
        return False
    if body.startswith("#"):
        return False
    if len(body) < 2:
        return False
    return any(ch.isalpha() for ch in body)


def cmd_wrap(args):
    values = load_dictionary(DICT)
    if not values:
        print("dictionary is empty: " + str(DICT))
        return 1
    files = []
    if args.files:
        files = [Path(name) for name in args.files]
    else:
        for pattern in ("*.cpp", "*.h"):
            files.extend(sorted((ROOT / "src").rglob(pattern)))
        files = [path.relative_to(ROOT) for path in files]
    totals = {"files": 0, "tr": 0, "trc": 0, "trf": 0, "wrapped": 0, "log": 0}
    missing = []
    changed = []
    for name in files:
        path = ROOT / name
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        skel, literals = scan(text)
        stem = Path(name).stem
        edits = []
        file_missing = []
        for start, end, inner, raw, count in literals:
            kind, _ = wrapper_context(text, start, end)
            if kind:
                totals["wrapped"] += 1
                continue
            if count != 1:
                continue
            body = decode_literal(inner, raw)
            if raw and NL in body:
                continue
            name_of_call = call_name(text, start)
            if english_only(name_of_call):
                totals["log"] += 1
                continue
            if not obvious_text(body):
                continue
            if in_constexpr(text, start):
                continue
            scoped = (body, stem.lower()) in values
            if not scoped and (body, "") not in values:
                line = text.count(NL, 0, start) + 1
                file_missing.append({"file": str(name), "line": line, "text": body})
                continue
            literal = text[start:end]
            callee = format_callee(text, start)
            if callee is not None:
                edits.append((callee[0], callee[1], "TRFC" if scoped else "TRF"))
                totals["trf"] += 1
                continue
            if scoped:
                edits.append((start, end, "TRC(" + literal + ", " + QUOTE + stem + QUOTE + ")"))
                totals["trc"] += 1
            else:
                edits.append((start, end, "TR(" + literal + ")"))
                totals["tr"] += 1
        missing.extend(file_missing)
        if not edits:
            continue
        edits.sort()
        out = []
        cursor = 0
        for begin, finish, replacement in edits:
            if begin < cursor:
                continue
            out.append(text[cursor:begin])
            out.append(replacement)
            cursor = finish
        out.append(text[cursor:])
        text = "".join(out)
        if "Localization.h" not in text:
            anchor = re.search("^#include .*$", text, re.M)
            if anchor:
                text = text[:anchor.end()] + NL + '#include "Localization.h"' + text[anchor.end():]
        changed.append(str(name))
        totals["files"] += 1
        if not args.dry_run:
            path.write_text(text, encoding="utf-8", newline="")
    out = ROOT / "tools" / "lang" / "wrap-missing.json"
    out.write_text(json.dumps(missing, ensure_ascii=False, indent=1) + NL, encoding="utf-8")
    print("files rewritten: %d, TR: %d, TRC: %d, TRF: %d, already wrapped: %d, log/dialog kept: %d"
          % (totals["files"], totals["tr"], totals["trc"], totals["trf"], totals["wrapped"], totals["log"]))
    print("untranslated candidates: %d -> %s" % (len(missing), out))
    for name in changed:
        print("  " + name)
    return 0



def plumb_literal(text, start, body):
    """Plumbing rather than prose: includes, paths, ids and format fields."""
    line_start = text.rfind(NL, 0, start) + 1
    if text[line_start:start].lstrip().startswith("#"):
        return True
    if "/" in body or BS in body or ".h" == body[-2:] or ".cpp" in body:
        return True
    if " " not in body or "::" in body or "{" in body or "%" in body:
        return True
    return False


def cmd_audit(args):
    """Cross-check the active dictionaries against what the sources ask for."""
    values = load_dictionary(DICT)
    if not values:
        print("dictionary is empty: " + str(DICT))
        return 1
    missing = []
    unused = []
    unwrapped = []
    used = set()
    constant_pool = set()
    files = sorted((ROOT / "src").rglob("*.cpp")) + sorted((ROOT / "src").rglob("*.h"))
    for path in files:
        rel = str(path.relative_to(ROOT)).replace(BS, "/")
        text = path.read_text(encoding="utf-8")
        skel, literals = scan(text)
        for start, end, inner, raw, count in literals:
            body = decode_literal(inner, raw)
            kind, context = wrapper_context(text, start, end)
            line = text.count(NL, 0, start) + 1
            if kind:
                key = (body, (context or "").lower())
                used.add(key)
                if key not in values and lookup(values, body, context) is None:
                    missing.append((rel, line, body, context))
                continue
            if in_constexpr(text, start):
                constant_pool.add(body)
                continue
            if raw and NL in body:
                continue
            if english_only(call_name(text, start)):
                continue
            if not obvious_text(body):
                continue
            if plumb_literal(text, start, body):
                continue
            unwrapped.append((rel, line, body))
    for (key, context) in values:
        if not context:
            if (key, "") not in used and key not in constant_pool:
                unused.append((key, context))
        elif (key, context) not in used and (key, "") not in used and key not in constant_pool:
            unused.append((key, context))
    out = ROOT / "tools" / "lang" / "audit-report.txt"
    report = []
    report.append("dictionary: %s (%d entries)" % (DICT.name, len(values)))
    report.append("missing entries used by the sources: %d" % len(missing))
    for rel, line, body, context in missing[:200]:
        report.append("  MISSING %s:%d %r context=%r" % (rel, line, body, context))
    report.append("dictionary entries no source asks for: %d" % len(unused))
    for key, context in unused[:400]:
        report.append("  UNUSED context=%r %r" % (context, key))
    report.append("unwrapped text the sources still carry: %d" % len(unwrapped))
    for rel, line, body in unwrapped[:400]:
        report.append("  UNWRAPPED %s:%d %r" % (rel, line, body))
    out.write_text(NL.join(report) + NL, encoding="utf-8")
    print("missing: %d, unused: %d, unwrapped: %d -> %s" % (len(missing), len(unused), len(unwrapped), out))
    return 0



def main(argv):
    parser = argparse.ArgumentParser(description="TheosRenderPipeline dictionary tooling")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("extract", help="pair the English and Chinese literals")
    commands.add_parser("dictionary", help="write the shipped zh-CN dictionary")
    rewrite = commands.add_parser("rewrite", help="wrap the English literals in TR/TRC/TRF")
    rewrite.add_argument("--dry-run", action="store_true")
    commands.add_parser("audit", help="cross-check dictionaries against the sources")
    commands.add_parser("check", help="verify dictionary and sources agree")
    commands.add_parser("render", help="list translated strings the sources no longer produce")
    wrap = commands.add_parser("wrap", help="wrap literals the dictionary already knows")
    wrap.add_argument("--dry-run", action="store_true")
    wrap.add_argument("--files", nargs="*")
    args = parser.parse_args(argv)
    table = {
        "extract": cmd_extract,
        "dictionary": cmd_dictionary,
        "rewrite": cmd_rewrite,
        "audit": cmd_audit,
        "check": cmd_check,
        "render": cmd_render,
        "wrap": cmd_wrap,
    }
    return table[args.command](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
