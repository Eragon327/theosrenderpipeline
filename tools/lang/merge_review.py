#!/usr/bin/env python3
"""Merge the reviewer JSON files into the shipped zh-CN dictionary.

Each review file is a JSON array of {"file", "english", "chinese", "context"}
records.  Existing keys are rewritten in place; new keys are appended to the
matching [Strings] or [Strings.<context>] section.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import localize  # noqa: E402

REVIEW = localize.ROOT / "tools" / "lang" / "review"
DICT = localize.DICT


def parse_sections(text):
    """Return [(header_line_or_None, [ (english_key, raw_line) ])]."""
    sections = []
    current = (None, [])
    sections.append(current)
    for line in text.split(localize.NL):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            current = (line, [])
            sections.append(current)
            continue
        current[1].append((stripped, line))
    return sections



def key_of(stripped):
    """Sort key for a section line: the unquoted English key, or the raw text."""
    position = -1
    quoted = False
    for offset, ch in enumerate(stripped):
        if ch == localize.QUOTE:
            quoted = not quoted
        elif ch == "=" and not quoted:
            position = offset
            break
    if position < 0:
        return stripped
    key = stripped[:position].strip()
    if len(key) >= 2 and key[0] == localize.QUOTE and key[-1] == localize.QUOTE:
        key = key[1:-1]
    return key


def sort_sections(lines):
    """Sort the entries of every section, keeping its leading comments in place."""
    out = []
    index = 0
    total = len(lines)
    while index < total:
        line = lines[index]
        stripped = line.strip()
        out.append(line)
        index += 1
        if not (stripped.startswith("[") and stripped.endswith("]")):
            continue
        body = []
        while index < total:
            candidate = lines[index]
            text = candidate.strip()
            if text.startswith("[") and text.endswith("]"):
                break
            body.append(candidate)
            index += 1
        lead = 0
        while lead < len(body) and not body[lead].strip():
            lead += 1
        while lead < len(body) and (body[lead].strip().startswith(";") or body[lead].strip().startswith("#")):
            lead += 1
        while lead < len(body) and not body[lead].strip():
            lead += 1
        entries = [item for item in body[lead:] if item.strip()]
        entries.sort(key=lambda item: key_of(item.strip()))
        out.extend(body[:lead])
        out.extend(entries)
        # Keep exactly one blank line before the next section header.
        if index < total:
            while out and not out[-1].strip():
                out.pop()
            out.append("")
    return out


def normalize_sections(lines):
    """One blank line before every section header, none at the end."""
    out = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            while out and not out[-1].strip():
                out.pop()
            if out:
                out.append("")
            out.append(line)
            continue
        out.append(line)
    while out and not out[-1].strip():
        out.pop()
    return out

def main():
    entries = []
    for path in sorted(REVIEW.glob("*.json")):
        import json
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception as error:  # noqa: BLE001
            print("SKIP %s: %s" % (path.name, error))
            continue
        for item in data:
            english = item.get("english", "")
            chinese = item.get("chinese", "")
            context = item.get("context", "") or ""
            if not english.strip():
                print("SKIP empty key in " + path.name)
                continue
            entries.append((english, chinese, context, path.name))
    if not entries:
        print("no review entries")
        return 0
    # A label may appear in more than one review file; keep the last wording.
    unique = {}
    for english, chinese, context, source in entries:
        unique[(english, context)] = (english, chinese, context, source)
    entries = list(unique.values())

    text = DICT.read_text(encoding="utf-8")
    lines = text.split(localize.NL)
    # map key -> [(line index)] per section
    sections = []
    current_name = ""
    current_start = 0
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            sections.append((current_name, current_start, index))
            current_name = stripped[1:-1].strip().lower()
            current_start = index + 1
    sections.append((current_name, current_start, len(lines)))

    def section_bounds(name):
        for section_name, begin, end in sections:
            if section_name == name:
                return begin, end
        return None

    updated = 0
    added = 0
    for english, chinese, context, source in entries:
        wanted = "strings." + context.lower() if context else "strings"
        key_text = localize.dict_text(english, False)
        value_text = localize.dict_text(chinese, False)
        new_line = key_text + " = " + value_text
        bounds = section_bounds(wanted)
        if bounds is None:
            lines.append("")
            lines.append("[Strings." + context + "]")
            lines.append(new_line)
            updated_line_count = len(lines)
            sections.append((wanted, updated_line_count - 1, updated_line_count))
            added += 1
            continue
        begin, end = bounds
        found = -1
        for index in range(begin, end):
            stripped = lines[index].strip()
            if stripped.startswith(";") or stripped.startswith("#") or not stripped:
                continue
            position = -1
            quoted = False
            for offset, ch in enumerate(stripped):
                if ch == localize.QUOTE:
                    quoted = not quoted
                elif ch == "=" and not quoted:
                    position = offset
                    break
            if position < 0:
                continue
            existing = stripped[:position].strip()
            if existing == key_text:
                found = index
                break
        if found >= 0:
            if lines[found].strip() != new_line:
                lines[found] = new_line
                updated += 1
        else:
            lines.insert(begin, new_line)
            added += 1
            sections = []
            current_name = ""
            current_start = 0
            for index, line in enumerate(lines):
                stripped = line.strip()
                if stripped.startswith("[") and stripped.endswith("]"):
                    sections.append((current_name, current_start, index))
                    current_name = stripped[1:-1].strip().lower()
                    current_start = index + 1
            sections.append((current_name, current_start, len(lines)))

    lines = normalize_sections(sort_sections(lines))
    DICT.write_text(localize.NL.join(lines), encoding="utf-8")
    print("review entries: %d -> updated %d, added %d" % (len(entries), updated, added))
    return 0


if __name__ == "__main__":
    sys.exit(main())
