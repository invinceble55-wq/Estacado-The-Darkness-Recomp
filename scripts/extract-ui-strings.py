"""Lists every English interface text the launcher, the shared settings panel
and the F1 overlay can show (7b): the literals inside Tr(...) /
Translate(...) calls and the wide literals of the settings schema. Adjacent
literals are joined as the compiler joins them.

  python scripts/extract-ui-strings.py            -> one text per line (JSON strings)
  python scripts/extract-ui-strings.py --missing config/ui_languages/ru.toml
      -> the texts that file does not translate yet
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CALL_SOURCES = [
    ROOT / "runtime/settings_launcher.cpp",
    ROOT / "external/ReXGlue/src/ui/settings_panel.cpp",
    ROOT / "external/ReXGlue/src/ui/overlay/host_settings_overlay.cpp",
    ROOT / "external/ReXGlue/include/rex/ui/settings_detection.h",
]
# Texts that reach Tr() through tables: the player presets' names
# (pc_settings_utility_model.cpp kKnownPresets) and the mouse binding labels
# (settings_panel.cpp BindingLabel).
EXTRA = [
    "Enhanced (recommended)", "Performance (for weaker PCs)", "Original (console look)",
    "Custom", "(none)", "Left mouse", "Right mouse", "Middle mouse", "Mouse 4", "Mouse 5",
    "Wheel up", "Wheel down", "degrees",
]
RETURN_SOURCES = [
    ROOT / "runtime/settings_launcher.cpp",
    ROOT / "runtime/runtime_game_setup.cpp",
]
SCHEMA_SOURCES = [
    ROOT / "runtime/pc_settings_schema.cpp",
    ROOT / "runtime/pc_settings_ui.cpp",
]
ESCAPES = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}
LITERAL = re.compile(r'(L|u8)?"((?:[^"\\\n]|\\.)*)"')


def decode(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == "u":
            out.append(chr(int(body[i + 2:i + 6], 16)))
            i += 6
        elif n == "x":
            m = re.match(r"[0-9a-fA-F]+", body[i + 2:])
            out.append(chr(int(m.group(0), 16)))
            i += 2 + len(m.group(0))
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    return "".join(out)


def literals(text, wide_only=False):
    """Joined literal groups in text: [(start, string, wide)]."""
    groups = []
    pos = 0
    while True:
        m = LITERAL.search(text, pos)
        if not m:
            break
        parts, wide, start, end = [decode(m.group(2))], m.group(1) == "L", m.start(), m.end()
        while True:
            gap = re.match(r'\s*(?://[^\n]*\s*)*', text[end:])
            n = LITERAL.match(text, end + gap.end())
            if not n:
                break
            parts.append(decode(n.group(2)))
            end = n.end()
        pos = end
        if wide_only and not wide:
            continue
        groups.append((start, "".join(parts), wide))
    return groups


def strip_comments(text):
    # Line comments outside literals (keeps the code's literals intact).
    out = []
    for line in text.split("\n"):
        in_str, i = False, 0
        while i < len(line):
            c = line[i]
            if c == "\\" and in_str:
                i += 2
                continue
            if c == '"':
                in_str = not in_str
            elif not in_str and line.startswith("//", i):
                line = line[:i]
                break
            i += 1
        out.append(line)
    return "\n".join(out)


def call_strings(path):
    text = strip_comments(path.read_text(encoding="utf-8"))
    found = []
    for m in re.finditer(r"\b(?:Tr|tr|Translate|choose|button|FinishJob)\(", text):
        depth, i = 1, m.end()
        in_str = False
        while i < len(text) and depth:
            c = text[i]
            if in_str:
                if c == "\\":
                    i += 1
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            i += 1
        found += [s for _, s, _ in literals(text[m.end():i - 1])]
    return found


def return_strings(path):
    """Messages a function returns as literals (shown through Tr(app, message))."""
    text = strip_comments(path.read_text(encoding="utf-8"))
    found = []
    for m in re.finditer(r"(?:return|[?:])\s*(?=\")", text):
        groups = literals(text[m.end():m.end() + 2000])
        if groups and groups[0][0] == 0:
            found.append(groups[0][1])
    return [s for s in found if " " in s and s[:1].isupper()]


def schema_strings(path):
    text = strip_comments(path.read_text(encoding="utf-8"))
    return [s for _, s, _ in literals(text, wide_only=True)]


# Technical names, and texts shown only with the (shelved) Arabic interface.
NO_ENTRY = {
    " (default.xex)", "VSync", "FXAA", "FXAA Extreme", "SMAA", "AMD FSR 1", "TAA", "NVIDIA DLAA",
    "Language (اللغة)", "العربية (Arabic)",
    "Language of the game's text and speech and of these settings. Automatic follows the Windows "
    "display language, like the console's dashboard language (English when the game has no such "
    "language). Arabic shows these settings in Arabic; the game's speech stays English, and its "
    "text becomes Arabic when an Arabic language pack is installed.",
}


def keep(text):
    # Technical names and values need no entry (see arabic_ui_translations.py).
    if text.startswith("##") or text in NO_ENTRY:
        return False
    if not re.search(r"[A-Za-z]{2}", text):
        return False
    if re.fullmatch(r"[a-z0-9_.%]+", text):
        return False
    return True


def all_strings():
    seen, out = set(), []
    for s in EXTRA:
        seen.add(s)
        out.append(s)
    for path in CALL_SOURCES:
        for s in call_strings(path):
            if keep(s) and s not in seen:
                seen.add(s)
                out.append(s)
    for path in RETURN_SOURCES:
        for s in return_strings(path):
            if keep(s) and s not in seen:
                seen.add(s)
                out.append(s)
    for path in SCHEMA_SOURCES:
        for s in schema_strings(path):
            if keep(s) and s not in seen:
                seen.add(s)
                out.append(s)
    return out


def main():
    strings = all_strings()
    if len(sys.argv) >= 3 and sys.argv[1] == "--missing":
        import tomllib
        table = tomllib.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")).get("strings", {})
        strings = [s for s in strings if s not in table]
    for s in strings:
        print(json.dumps(s, ensure_ascii=False))
    print(f"# {len(strings)} texts", file=sys.stderr)


if __name__ == "__main__":
    main()
