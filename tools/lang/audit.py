"""Compare a menu dictionary with the menu sources.

    python tools/lang/audit.py [dictionary.ini] [--prune] [--missing FILE]

Lists the menu text the dictionary does not translate yet (missing) and the
entries no menu text uses any more (unused). --prune deletes the unused
entries; --missing writes the missing text as ready-to-fill dictionary lines.
Run from the repository root. The default dictionary is the shipped zh-CN.ini.
"""
import re
import sys
from pathlib import Path

LIT = r'"(?:[^"\\\n]|\\.)*"'
RUN = rf'{LIT}(?:\s*{LIT})*'
# TR("text"), TRF("text", ...), TRID("text") and TRC("text", "context").
CALL = re.compile(rf'\bTR(F|ID|C)?\(\s*({RUN})(?:\s*,\s*({LIT}))?')
PIECE = re.compile(r'"((?:[^"\\]|\\.)*)"')
ESCAPES = {'n': '\n', 't': '\t', '"': '"', '\\': '\\'}


def unescape(text):
    # \xNN escapes are UTF-8 bytes ("\xC2\xB7" is a middle dot).
    raw = re.sub(rb'\\x([0-9A-Fa-f]{2})', lambda m: bytes([int(m.group(1), 16)]), text.encode('utf-8'))
    return re.sub(r'\\(.)', lambda m: ESCAPES.get(m.group(1), m.group(1)), raw.decode('utf-8', 'replace'))


def source_keys():
    """(context, English) for every TR call in the menu sources."""
    keys = set()
    for path in sorted(Path('src').glob('Overlay*')):
        for m in CALL.finditer(path.read_text(encoding='utf-8')):
            english = unescape(''.join(PIECE.findall(m.group(2))))
            context = unescape(m.group(3)[1:-1]) if m.group(1) == 'C' and m.group(3) else ''
            keys.add((context, english))
    return keys


def source_texts():
    """Every string literal run in src/. Some menu text is translated through a
    variable (TR(names[i]), the status line's controller messages), so an entry
    whose English appears anywhere in the sources is not unused."""
    texts = set()
    for path in sorted(Path('src').rglob('*')):
        if path.suffix in ('.cpp', '.h'):
            for m in re.finditer(RUN, path.read_text(encoding='utf-8', errors='replace')):
                texts.add(unescape(''.join(PIECE.findall(m.group(0)))))
    return texts


def split(line):
    quoted = False
    i = 0
    while i < len(line):
        if line[i] == '\\' and quoted:
            i += 1
        elif line[i] == '"':
            quoted = not quoted
        elif line[i] == '=' and not quoted:
            return line[:i].strip(), line[i + 1:].strip()
        i += 1
    return None


def decode(text):
    return unescape(text[1:-1]) if len(text) >= 2 and text[0] == text[-1] == '"' else text


def quote(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\t', '\\t') + '"'


def main(argv):
    output = argv[argv.index('--missing') + 1] if '--missing' in argv else None
    args = [a for a in argv if not a.startswith('--') and a != output]
    path = Path(args[0] if args else 'package/SKSE/Plugins/TheosRenderPipeline/Lang/zh-CN.ini')
    lines = path.read_text(encoding='utf-8-sig').splitlines()
    used = source_keys()
    literals = source_texts()
    section, entries, keep = '', set(), []
    for line in lines:
        s = line.strip()
        if s.startswith('['):
            section = s[1:-1].strip()
        elif s and s[0] not in ';#' and section.startswith('Strings') and (parts := split(s)):
            key = (section[8:], decode(parts[0]))
            entries.add(key)
            if key not in used and ('', key[1]) not in used and key[1] not in literals:
                continue  # unused
        keep.append(line)
    plain = {english for _, english in entries}
    missing = sorted(english for context, english in used if english not in plain and (context, english) not in entries)
    unused = len(lines) - len(keep)
    print(f'{path}: {len(entries)} entries, {len(missing)} missing, {unused} unused')
    if '--prune' in argv and unused:
        newline = '\r\n' if b'\r\n' in path.read_bytes() else '\n'
        path.write_bytes((newline.join(keep) + newline).encode('utf-8'))
        print(f'pruned {unused} unused entries')
    if output:
        target = Path(output)
        target.write_text(''.join(f'{quote(english)} =\n' for english in missing), encoding='utf-8')
        print(f'wrote {target}')
    elif missing:
        for english in missing:
            print('  missing:', english.replace('\n', '\\n'))


if __name__ == '__main__':
    main(sys.argv[1:])
