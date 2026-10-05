"""gather.py: collect HyperAIStudio's LOCTEXT and NSLOCTEXT strings and keep the translation files in step.

    python Tools/Localization/gather.py            update Resources/Localization/*.json from the source
    python Tools/Localization/gather.py check      exit 1 if any translation file is out of step
    python Tools/Localization/gather.py add <culture>   start a translation file, e.g. ja or zh-Hans

Each <culture>.json holds one entry per string: namespace, key, source (the English text) and text (the
translation, empty until translated). The editor loads the entries whose source still matches, so a changed
English string falls back to English until it is translated again. Nothing here runs the engine.

The prompt library (Resources/Prompts) is gathered too, under the namespace HyperAIStudioPrompts: each category's
title ("category.<id>"), and each preset's title, summary and placeholder labels and hints ("<id>.title",
"<id>.summary", "<id>.ph.<name>.label", "<id>.ph.<name>.hint"). Their translations are written beside the presets,
in Tools/Prompts/<culture>/<category>.json as flat {"<key>": "<text>"} objects, and copied in from there; for a
culture that has such a folder, check also fails while any prompt string has no translation. Prompt bodies are
written for agents and stay English.
"""
import json
import re
import sys
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
SOURCE = PLUGIN / "Source"
OUT = PLUGIN / "Resources" / "Localization"
PROMPTS = PLUGIN / "Resources" / "Prompts"
PROMPT_TRANSLATIONS = PLUGIN / "Tools" / "Prompts"
PROMPT_NAMESPACE = "HyperAIStudioPrompts"

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
    # The prompt library's strings join the same set, so check sees them out of step like any other.
    for key, source, name in gather_prompts():
        identity = (PROMPT_NAMESPACE, key)
        if identity in entries and entries[identity]["source"] != source:
            conflicts.append("%s/%s: %r vs %r (%s)" % (PROMPT_NAMESPACE, key, entries[identity]["source"], source, name))
            continue
        entries.setdefault(identity, {"namespace": PROMPT_NAMESPACE, "key": key, "source": source, "files": set()})
        entries[identity]["files"].add("Resources/Prompts/" + name)
    return [entries[identity] for identity in sorted(entries)], conflicts


def gather_prompts():
    """(key, English, file) for every category title and every preset's title, summary and placeholder label and hint."""
    found = []
    categories = PROMPTS / "categories.json"
    if categories.exists():
        for category in json.loads(categories.read_text(encoding="utf-8")).get("categories", []):
            if category.get("id") and category.get("title"):
                found.append(("category." + category["id"], category["title"], categories.name))
    for path in sorted(PROMPTS.glob("*.json")):
        if path.name == categories.name:
            continue
        for preset in json.loads(path.read_text(encoding="utf-8")).get("presets", []):
            preset_id = preset.get("id")
            if not preset_id or preset.get("hidden"):
                continue
            for field in ("title", "summary"):
                if preset.get(field):
                    found.append(("%s.%s" % (preset_id, field), preset[field], path.name))
            for placeholder in preset.get("placeholders", []):
                for field in ("label", "hint"):
                    if placeholder.get("name") and placeholder.get(field):
                        found.append(("%s.ph.%s.%s" % (preset_id, placeholder["name"], field), placeholder[field], path.name))
    return found


def prompt_translations(culture):
    """The culture's prompt translations from Tools/Prompts/<culture>/*.json, or None when it has no such folder."""
    folder = PROMPT_TRANSLATIONS / culture
    if not folder.is_dir():
        return None
    texts = {}
    for path in sorted(folder.glob("*.json")):
        texts.update(json.loads(path.read_text(encoding="utf-8")))
    return texts


def merge(culture, gathered, existing, prompt_texts=None):
    """New entries keep any translation whose English source is unchanged; others start empty. For a culture with
    prompt translations (prompt_texts), the prompt strings take theirs from there alone, so the two never drift."""
    known = {(entry["namespace"], entry["key"]): entry for entry in existing.get("entries", [])}
    merged = []
    for entry in gathered:
        previous = known.get((entry["namespace"], entry["key"]), {})
        text = previous.get("text", "") if previous.get("source") == entry["source"] else ""
        if entry["namespace"] == PROMPT_NAMESPACE and prompt_texts is not None:
            text = prompt_texts.get(entry["key"], "")
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
    untranslated = []
    for culture in cultures:
        path = OUT / (culture + ".json")
        existing = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        prompt_texts = prompt_translations(culture)
        merged = merge(culture, gathered, existing, prompt_texts)
        translated = sum(1 for entry in merged["entries"] if entry["text"])
        if merged != existing:
            if command == "check":
                stale.append(culture)
            else:
                write(path, merged)
        # A culture that translates the prompt library translates all of it: the picker shows nothing half-done.
        if prompt_texts is not None:
            for entry in merged["entries"]:
                if entry["namespace"] == PROMPT_NAMESPACE and not entry["text"]:
                    untranslated.append("%s %s" % (culture, entry["key"]))
        print("%s: %d of %d translated" % (culture, translated, len(merged["entries"])))
    print("%d strings in %d namespaces" % (len(gathered), len({entry["namespace"] for entry in gathered})))
    for missing in untranslated[:40]:
        print("prompt string without a translation:", missing)
    if len(untranslated) > 40:
        print("... and %d more prompt strings without a translation" % (len(untranslated) - 40))
    if command == "check" and (stale or conflicts or untranslated):
        print("out of step:", ", ".join(stale) or ("conflicts" if conflicts else "untranslated prompt strings"))
        sys.exit(1)


if __name__ == "__main__":
    main()
