#!/usr/bin/env python3
# Games by Hyper 2026.
"""check_prompts.py: checks HyperAI Studio's prompt library against the plugin it ships in.

    python Tools/Prompts/check_prompts.py check    exit 1 on any error
    python Tools/Prompts/check_prompts.py stats    presets per category and difficulty, and the catalog tools no preset uses

    --prompts DIR        read the <category>.json preset files from DIR instead of Resources/Prompts
    --translations DIR   read their zh-Hans translations from DIR instead of Tools/Prompts/zh-Hans

The library is Resources/Prompts: categories.json and one <category>.json per category (schema
hyperai.prompt-presets.v1). Each preset's title, summary and placeholder labels and hints are translated in
Tools/Prompts/zh-Hans/<category>.json, a flat {"<id>.title": "...", "<id>.ph.<name>.label": "..."} object, and the
category titles in Tools/Prompts/zh-Hans/categories.json ("category.<id>"); gather.py copies them into
Resources/Localization/zh-Hans.json.

The rules are the C++ PromptPresets::Validate's (HyperAIStudio.PromptPresets.* tests), checked against the files:
packs and tools against Tools/Catalog/catalog_source.json (a tool must belong to one of the preset's packs or to a
foundation pack, and the prompt must name it), skills against Resources/Skills and Resources/AgentTeam/skills, agents
against Resources/AgentTeam/agents, category routes against EnsureDefaultTaskRoutes in HyperAIStudioSettings.h, and
each plan op kind against a TEXT("<kind>") somewhere in Source. Every category needs at least its "min" presets, and
the example preset in Resources/Prompts/README.md keeps the same rules. Standard library only; nothing here runs the
engine. Run it beside build_catalog.py check and gather.py check.
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
CATEGORY_SCHEMA = "hyperai.prompt-categories.v1"
PRESET_SCHEMA = "hyperai.prompt-presets.v1"
FOUNDATION = {"shared_foundation", "context_scene_project_search", "diagnostics", "pie_runtime"}
TOKENS = {"selected_actors", "selected_actor", "selected_assets", "selected_asset", "level", "content_folder", "project"}
DIFFICULTY = {"starter", "standard", "advanced"}
SELECTION = {"actors", "assets", "none"}
PRESET_FIELDS = {"id", "title", "summary", "packs", "skills", "agent", "tools", "ops", "difficulty", "tags", "context",
                 "placeholders", "prompt"}
CATEGORY_FIELDS = {"id", "title", "order", "icon", "min", "route"}
CONTEXT_FIELDS = {"selection", "classes", "min"}
PLACEHOLDER_FIELDS = {"name", "label", "default", "hint"}
ID_RE = re.compile(r"^[a-z0-9-]+\.[a-z0-9-]+$")
KEBAB_RE = re.compile(r"^[a-z0-9-]+$")
NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
BRACED_RE = re.compile(r"\{\{([^{}]*)\}\}")
UNCLOSED_RE = re.compile(r"\{\{([a-z][a-z0-9_]*)(?![a-z0-9_]|\}\})")
TEXT_RE = re.compile(r'TEXT\("((?:[^"\\]|\\.)*)"\)')
ROUTE_RE = re.compile(r'Add\(TEXT\("([^"]+)"\)')
MAX_TITLE, MAX_SUMMARY, MIN_PROMPT, MAX_PROMPT, MAX_PACKS, MAX_SKILLS = 60, 160, 80, 4000, 3, 2


def read_json(path, errors):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        errors.append("%s: not readable JSON: %s" % (path.name, error))
        return None


class Plugin:
    """What the prompts may name: packs and their tools, skills, agents, task routes and plan op kinds."""

    def __init__(self, root):
        catalog = json.loads((root / "Tools" / "Catalog" / "catalog_source.json").read_text(encoding="utf-8"))
        self.tools = {}
        self.pack_of = {}
        for pack in catalog["packs"]:
            names = [tool["name"] if isinstance(tool, dict) else tool for tool in pack.get("tools", [])]
            self.tools[pack["id"]] = names
            for name in names:
                self.pack_of[name] = pack["id"]
        self.skills = {path.name for folder in (root / "Resources" / "Skills", root / "Resources" / "AgentTeam" / "skills")
                       if folder.is_dir() for path in folder.iterdir() if path.is_dir()}
        self.agents = {path.stem for path in (root / "Resources" / "AgentTeam" / "agents").glob("*.md")} | {"team"}
        settings = (root / "Source" / "HyperAIStudio" / "Public" / "HyperAIStudioSettings.h").read_text(encoding="utf-8")
        body = settings[settings.find("bool EnsureDefaultTaskRoutes()"):]
        body = body[:body.find("return true;")]
        self.routes = set(ROUTE_RE.findall(body))
        self.root = root
        self._literals = None

    def literals(self):
        """Every TEXT("...") in the C++ source, for the op kinds the toolsets compare against."""
        if self._literals is None:
            self._literals = set()
            for path in (self.root / "Source").rglob("*.cpp"):
                self._literals.update(TEXT_RE.findall(path.read_text(encoding="utf-8", errors="replace")))
        return self._literals


def check_categories(plugin, translations, errors):
    data = read_json(PLUGIN / "Resources" / "Prompts" / "categories.json", errors)
    if data is None:
        return {}
    if data.get("schema") != CATEGORY_SCHEMA or not isinstance(data.get("categories"), list):
        errors.append("categories.json: needs \"schema\": \"%s\" and a categories array" % CATEGORY_SCHEMA)
        return {}
    zh = {}
    side = translations / "categories.json"
    if not side.exists():
        # A staging folder holds presets only; the category titles stay with the plugin.
        side = PLUGIN / "Tools" / "Prompts" / "zh-Hans" / "categories.json"
    if side.exists():
        zh = read_json(side, errors) or {}
    else:
        errors.append("categories.json: no zh-Hans translations at %s" % side)
    categories = {}
    for index, category in enumerate(data["categories"]):
        cid = category.get("id", "")
        at = "categories.json[%d] %s" % (index, cid)
        for key in category:
            if key not in CATEGORY_FIELDS:
                errors.append("%s: unknown field %s" % (at, key))
        if not KEBAB_RE.match(cid) or cid in categories:
            errors.append("%s: id must be unique lower-case words joined by hyphens" % at)
            continue
        if not (0 < len(category.get("title", "")) <= MAX_TITLE):
            errors.append("%s: title must be 1-%d characters" % (at, MAX_TITLE))
        if not isinstance(category.get("order", 0), int) or not isinstance(category.get("min", 0), int) or category.get("min", 0) < 0:
            errors.append("%s: order and min must be whole numbers, min not negative" % at)
        if "route" in category and category["route"] not in plugin.routes:
            errors.append("%s: route '%s' is not a default task route (%s)" % (at, category["route"], ", ".join(sorted(plugin.routes))))
        if not zh.get("category." + cid):
            errors.append("%s: zh-Hans missing category.%s" % (at, cid))
        categories[cid] = category
    return categories


def check_preset(plugin, preset, index, category, where, zh, ids, errors):
    pid = preset.get("id", "")
    at = "%s[%d] %s" % (where, index, pid)
    for key in preset:
        if key not in PRESET_FIELDS:
            errors.append("%s: unknown field %s" % (at, key))
    if not isinstance(pid, str) or not ID_RE.match(pid) or not pid.startswith(category + "."):
        errors.append("%s: id must be '<category>.<lower-case-words>' with this file's category" % at)
    if pid in ids:
        errors.append("%s: id is used twice (also in %s)" % (at, ids[pid]))
    ids.setdefault(pid, where)
    strings = {field: preset.get(field, "") for field in ("title", "summary", "prompt", "agent", "difficulty")}
    for field, value in strings.items():
        if not isinstance(value, str):
            errors.append("%s: %s must be a string" % (at, field))
            strings[field] = ""
    title, summary, prompt = strings["title"], strings["summary"], strings["prompt"]
    if not (0 < len(title) <= MAX_TITLE):
        errors.append("%s: title must be 1-%d characters" % (at, MAX_TITLE))
    if not (0 < len(summary) <= MAX_SUMMARY) or "\n" in summary:
        errors.append("%s: summary must be one line of 1-%d characters" % (at, MAX_SUMMARY))
    if not (MIN_PROMPT <= len(prompt) <= MAX_PROMPT):
        errors.append("%s: prompt must be %d-%d characters" % (at, MIN_PROMPT, MAX_PROMPT))
    if strings["difficulty"] not in DIFFICULTY:
        errors.append("%s: difficulty must be starter, standard or advanced" % at)
    lists = {}
    for field in ("packs", "skills", "tools", "ops", "tags"):
        value = preset.get(field, [])
        if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
            errors.append("%s: %s must be an array of strings" % (at, field))
            value = []
        lists[field] = value
    packs = lists["packs"]
    if not (1 <= len(packs) <= MAX_PACKS):
        errors.append("%s: packs must name 1-%d packs" % (at, MAX_PACKS))
    for pack in packs:
        if pack not in plugin.tools:
            errors.append("%s: pack %s is not in the catalog" % (at, pack))
    if len(lists["skills"]) > MAX_SKILLS:
        errors.append("%s: at most %d skills" % (at, MAX_SKILLS))
    for skill in lists["skills"]:
        if skill not in plugin.skills:
            errors.append("%s: skill %s is not shipped" % (at, skill))
    if strings["agent"] and strings["agent"] not in plugin.agents:
        errors.append("%s: agent %s is not one of the team's" % (at, strings["agent"]))
    for tool in lists["tools"]:
        owner = plugin.pack_of.get(tool)
        if owner is None:
            errors.append("%s: tool %s is not in the catalog" % (at, tool))
        elif owner not in packs and owner not in FOUNDATION:
            errors.append("%s: tool %s belongs to pack %s, which is not one of this preset's" % (at, tool, owner))
        if tool not in prompt:
            errors.append("%s: tool %s is listed but the prompt does not name it" % (at, tool))
    for kind in lists["ops"]:
        if kind not in plugin.literals():
            errors.append("%s: op kind %s appears as TEXT(\"%s\") nowhere in Source" % (at, kind, kind))
    context = preset.get("context")
    if context is not None:
        if not isinstance(context, dict):
            errors.append("%s: context must be an object" % at)
        else:
            for key in context:
                if key not in CONTEXT_FIELDS:
                    errors.append("%s: unknown field context.%s" % (at, key))
            if context.get("selection", "none") not in SELECTION:
                errors.append("%s: context.selection must be actors, assets or none" % at)
            if not isinstance(context.get("classes", []), list) or not isinstance(context.get("min", 0), int) or context.get("min", 0) < 0:
                errors.append("%s: context.classes must be an array and context.min a whole number" % at)
    names = []
    placeholders = preset.get("placeholders", [])
    if not isinstance(placeholders, list):
        errors.append("%s: placeholders must be an array" % at)
        placeholders = []
    for placeholder in placeholders:
        if not isinstance(placeholder, dict):
            errors.append("%s: each placeholder must be an object" % at)
            continue
        for key in placeholder:
            if key not in PLACEHOLDER_FIELDS:
                errors.append("%s: unknown field placeholders.%s" % (at, key))
        name = placeholder.get("name", "")
        if not isinstance(name, str) or not NAME_RE.match(name) or name in TOKENS or name in names:
            errors.append("%s: placeholder '%s' must be a new lower-case name" % (at, name))
        names.append(name)
        if not placeholder.get("label"):
            errors.append("%s: placeholder %s needs a label" % (at, name))
        if "{{%s}}" % name not in prompt:
            errors.append("%s: placeholder %s is declared but the prompt does not use it" % (at, name))
        if not zh.get("%s.ph.%s.label" % (pid, name)):
            errors.append("%s: zh-Hans missing %s.ph.%s.label" % (at, pid, name))
        if placeholder.get("hint") and not zh.get("%s.ph.%s.hint" % (pid, name)):
            errors.append("%s: zh-Hans missing %s.ph.%s.hint" % (at, pid, name))
    for used in BRACED_RE.findall(prompt):
        if used not in TOKENS and used not in names:
            errors.append("%s: {{%s}} is neither a context token nor a declared placeholder" % (at, used))
    for unclosed in UNCLOSED_RE.findall(prompt):
        errors.append("%s: {{%s has no closing }}" % (at, unclosed))
    for field in ("title", "summary"):
        if not zh.get("%s.%s" % (pid, field)):
            errors.append("%s: zh-Hans missing %s.%s" % (at, pid, field))
    return lists


def check(plugin, prompts, translations):
    errors, warnings = [], []
    categories = check_categories(plugin, translations, errors)
    library = {cid: [] for cid in categories}
    ids = {}
    for path in sorted(prompts.glob("*.json")):
        if path.name == "categories.json":
            continue
        if path.stem not in categories:
            errors.append("%s: %s is not a category in categories.json" % (path.name, path.stem))
            continue
        data = read_json(path, errors)
        if data is None:
            continue
        for key in data:
            if key not in ("schema", "category", "presets"):
                errors.append("%s: unknown field %s" % (path.name, key))
        if data.get("schema") != PRESET_SCHEMA or data.get("category") != path.stem or not isinstance(data.get("presets"), list):
            errors.append("%s: needs \"schema\": \"%s\", the file's name as its category, and a presets array" % (path.name, PRESET_SCHEMA))
            continue
        zh = {}
        side = translations / path.name
        if side.exists():
            zh = read_json(side, errors) or {}
        else:
            errors.append("%s: no zh-Hans translations at %s" % (path.name, side))
        expected = set()
        for index, preset in enumerate(data["presets"]):
            if not isinstance(preset, dict):
                errors.append("%s[%d]: each preset must be an object" % (path.name, index))
                continue
            lists = check_preset(plugin, preset, index, path.stem, path.name, zh, ids, errors)
            library[path.stem].append((preset, lists))
            pid = preset.get("id", "")
            expected |= {"%s.title" % pid, "%s.summary" % pid}
            for placeholder in preset.get("placeholders", []) if isinstance(preset.get("placeholders"), list) else []:
                if isinstance(placeholder, dict):
                    expected |= {"%s.ph.%s.%s" % (pid, placeholder.get("name"), field) for field in ("label", "hint") if placeholder.get(field)}
        for key in sorted(set(zh) - expected):
            warnings.append("%s: zh-Hans has %s, which no preset uses" % (side.name, key))
    for cid, category in categories.items():
        if len(library[cid]) < category.get("min", 0):
            errors.append("%s: %d presets, fewer than its min of %d" % (cid, len(library[cid]), category.get("min", 0)))
    check_readme_example(plugin, errors)
    return categories, library, errors, warnings


def check_readme_example(plugin, errors):
    """The README's example preset is copied as a template, so it keeps the same rules (its id is a shipped one's)."""
    readme = PLUGIN / "Resources" / "Prompts" / "README.md"
    match = re.search(r"```json\r?\n(.*?)\r?\n```", readme.read_text(encoding="utf-8"), re.S)
    if not match:
        errors.append("README.md: no ```json example preset")
        return
    try:
        preset = json.loads(match.group(1))
    except ValueError as error:
        errors.append("README.md: the example preset is not JSON: %s" % error)
        return
    category = str(preset.get("id", "")).split(".")[0]
    zh = read_json(PLUGIN / "Tools" / "Prompts" / "zh-Hans" / (category + ".json"), errors) or {}
    check_preset(plugin, preset, 0, category, "README.md example", zh, {}, errors)


def stats(plugin, categories, library):
    total = sum(len(presets) for presets in library.values())
    print("%-28s %5s %5s  %s" % ("category", "count", "min", "starter/standard/advanced"))
    for cid, category in categories.items():
        presets = library[cid]
        levels = Counter(preset.get("difficulty") for preset, _ in presets)
        mark = "" if len(presets) >= category.get("min", 0) else "  < min"
        print("%-28s %5d %5d  %d/%d/%d%s" % (cid, len(presets), category.get("min", 0), levels["starter"], levels["standard"], levels["advanced"], mark))
    print("total %d presets in %d categories" % (total, len(categories)))
    used_tools = Counter(tool for presets in library.values() for _, lists in presets for tool in lists["tools"])
    used_packs = Counter(pack for presets in library.values() for _, lists in presets for pack in lists["packs"])
    agents = Counter(preset.get("agent") or "(chat's own)" for presets in library.values() for preset, _ in presets)
    skills = Counter(skill for presets in library.values() for _, lists in presets for skill in lists["skills"])
    print("agents: " + ", ".join("%s %d" % pair for pair in agents.most_common()))
    print("skills: " + ", ".join("%s %d" % pair for pair in skills.most_common()))
    print("packs no preset names: " + (", ".join(sorted(set(plugin.tools) - set(used_packs))) or "none"))
    unused = sorted(tool for tools in plugin.tools.values() for tool in tools if tool not in used_tools)
    all_tools = sum(len(tools) for tools in plugin.tools.values())
    print("tool coverage: %d of %d catalog tools appear in a preset" % (all_tools - len(unused), all_tools))
    for pack, tools in sorted(plugin.tools.items()):
        missing = [tool for tool in tools if tool not in used_tools]
        if missing:
            print("  %s: %s" % (pack, ", ".join(missing)))


def main():
    args = sys.argv[1:]
    command = args.pop(0) if args and not args[0].startswith("--") else "check"
    prompts = PLUGIN / "Resources" / "Prompts"
    translations = PLUGIN / "Tools" / "Prompts" / "zh-Hans"
    while args:
        option = args.pop(0)
        if option == "--prompts" and args:
            prompts = Path(args.pop(0))
        elif option == "--translations" and args:
            translations = Path(args.pop(0))
        else:
            sys.exit(__doc__)
    if command not in ("check", "stats"):
        sys.exit(__doc__)
    plugin = Plugin(PLUGIN)
    categories, library, errors, warnings = check(plugin, prompts, translations)
    if command == "stats":
        stats(plugin, categories, library)
    for warning in warnings:
        print("warning:", warning)
    for error in errors:
        print("error:", error)
    print("%d presets, %d errors, %d warnings" % (sum(len(presets) for presets in library.values()), len(errors), len(warnings)))
    if command == "check" and errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
