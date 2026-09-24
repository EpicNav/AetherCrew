"""gather.py: collect HyperAIStudio's LOCTEXT and NSLOCTEXT strings and keep the translation files in step.

    python Tools/Localization/gather.py            update Resources/Localization/*.json from the source
    python Tools/Localization/gather.py check      exit 1 if any translation file is out of step
    python Tools/Localization/gather.py add <culture>   start a translation file, e.g. ja or zh-Hans

Each <culture>.json holds one entry per string: namespace, key, source (the English text) and text (the
translation, empty until translated). The editor loads the entries whose source still matches, so a changed
English string falls back to English until it is translated again. Nothing here runs the engine.
"""
import json
import re
import sys
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
SOURCE = PLUGIN / "Source"
OUT = PLUGIN / "Resources" / "Localization"

NAMESPACE_DEFINE = re.compile(r'^\s*#\s*define\s+LOCTEXT_NAMESPACE\s+"((?:[^"\\]|\\.)*)"', re.M)
NAMESPACE_UNDEF = re.compile(r'^\s*#\s*undef\s+LOCTEXT_NAMESPACE\b', re.M)
CALL = re.compile(r'\b(NSLOCTEXT|LOCTEXT)\s*\(')
STRING = re.compile(r'\s*(?:TEXT\s*\(\s*)?"((?:[^"\\]|\\.)*)"\s*\)?')
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0"}


def unescape(literal):
    out = []
    index = 0
    while index < len(literal):
        char = literal[index]
        if char == "\\" and index + 1 < len(literal):
            out.append(ESCAPES.get(literal[index + 1], literal[index + 1]))
            index += 2
            continue
        out.append(char)
        index += 1
    return "".join(out)


def read_strings(text, start, count):
    """Reads count comma-separated string arguments (each may be adjacent literals) from text[start:]."""
    values = []
    position = start
    for argument in range(count):
        parts = []
        while True:
            match = STRING.match(text, position)
            if not match:
                break
            parts.append(unescape(match.group(1)))
            position = match.end()
        if not parts:
            return None
        values.append("".join(parts))
        rest = text[position:].lstrip()
        position = len(text) - len(rest)
        if argument < count - 1:
            if not rest.startswith(","):
                return None
            position += 1
    return values


def gather():
    entries = {}
    conflicts = []
    for path in sorted(SOURCE.rglob("*")):
        # Test strings never reach a user.
        if path.suffix not in (".cpp", ".h") or "Intermediate" in path.parts or "Tests" in path.parts:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Namespace changes by position: every #define and #undef in order.
        marks = [(m.start(), unescape(m.group(1))) for m in NAMESPACE_DEFINE.finditer(text)]
        marks += [(m.start(), None) for m in NAMESPACE_UNDEF.finditer(text)]
        marks.sort()
        for match in CALL.finditer(text):
            line_start = text.rfind("\n", 0, match.start()) + 1
            line = text[line_start:match.start()]
            if line.lstrip().startswith("//") or line.lstrip().startswith("#define"):
                continue
            if match.group(1) == "NSLOCTEXT":
                values = read_strings(text, match.end(), 3)
                if not values:
                    continue
                namespace, key, source = values
            else:
                values = read_strings(text, match.end(), 2)
                namespace = None
                for position, name in marks:
                    if position < match.start():
                        namespace = name
                if not values or namespace is None:
                    continue
                key, source = values
            identity = (namespace, key)
            if identity in entries and entries[identity]["source"] != source:
                conflicts.append("%s/%s: %r vs %r (%s)" % (namespace, key, entries[identity]["source"], source, path.name))
                continue
            entries.setdefault(identity, {"namespace": namespace, "key": key, "source": source, "files": set()})
            entries[identity]["files"].add(path.relative_to(PLUGIN).as_posix())
    return [entries[identity] for identity in sorted(entries)], conflicts


def merge(culture, gathered, existing):
    """New entries keep any translation whose English source is unchanged; others start empty."""
    known = {(entry["namespace"], entry["key"]): entry for entry in existing.get("entries", [])}
    merged = []
    for entry in gathered:
        previous = known.get((entry["namespace"], entry["key"]), {})
        text = previous.get("text", "") if previous.get("source") == entry["source"] else ""
        merged.append({"namespace": entry["namespace"], "key": entry["key"], "source": entry["source"], "text": text})
    return {"culture": culture, "entries": merged}


def write(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=1) + "\n", encoding="utf-8", newline="\n")


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "update"
    gathered, conflicts = gather()
    for conflict in conflicts:
        print("conflict:", conflict)
    OUT.mkdir(parents=True, exist_ok=True)
    cultures = sorted(path.stem for path in OUT.glob("*.json"))
    if command == "add":
        cultures = sorted(set(cultures) | {sys.argv[2]})
    stale = []
    for culture in cultures:
        path = OUT / (culture + ".json")
        existing = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        merged = merge(culture, gathered, existing)
        translated = sum(1 for entry in merged["entries"] if entry["text"])
        if merged != existing:
            if command == "check":
                stale.append(culture)
            else:
                write(path, merged)
        print("%s: %d of %d translated" % (culture, translated, len(merged["entries"])))
    print("%d strings in %d namespaces" % (len(gathered), len({entry["namespace"] for entry in gathered})))
    if command == "check" and (stale or conflicts):
        print("out of step:", ", ".join(stale) or "conflicts")
        sys.exit(1)


if __name__ == "__main__":
    main()
