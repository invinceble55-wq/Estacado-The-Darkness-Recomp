"""Writes config/ui_languages/<code>.toml from scripts/ui_languages/<code>_strings.py
(7b). The TOML file is what the launcher and the F1 overlay read; a new
language needs only such a file (ui_language.h).

  python scripts/generate-ui-language.py ru
"""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def toml_string(text):
    out = ['"']
    for c in text:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif ord(c) < 0x20:
            out.append("\\u%04X" % ord(c))
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def main():
    code = sys.argv[1]
    source = ROOT / "scripts" / "ui_languages" / f"{code}_strings.py"
    spec = importlib.util.spec_from_file_location(f"{code}_strings", source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    lines = [
        f"# Interface language for the launcher and the F1 overlay ({code}).",
        f"# Generated from scripts/ui_languages/{code}_strings.py by",
        "# scripts/generate-ui-language.py; edit the source, or this file directly to try",
        "# a correction. Keys are the exact English texts; \"{}\" is a value the program",
        "# fills in. Untranslated texts stay English.",
        f"name = {toml_string(module.NAME)}",
        "windows = [" + ", ".join(toml_string(w) for w in module.WINDOWS) + "]",
        "",
        "[strings]",
    ]
    for english, translated in module.STRINGS.items():
        lines.append(f"{toml_string(english)} = {toml_string(translated)}")
    target = ROOT / "config" / "ui_languages" / f"{code}.toml"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    print(f"{target}: {len(module.STRINGS)} texts")


if __name__ == "__main__":
    main()
