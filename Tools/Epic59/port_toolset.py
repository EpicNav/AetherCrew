#!/usr/bin/env python3
# Games by Hyper 2026.
"""port_toolset.py: copy a UE 5.9 toolset's Source modules into HyperAIStudio and apply the UE 5.8 fixes.

    python Tools/Epic59/port_toolset.py CheatsToolset [ConsoleToolset ...]
    python Tools/Epic59/port_toolset.py --list                 # every 5.9 toolset and whether it is ported
    python Tools/Epic59/port_toolset.py --check CheatsToolset  # report only, write nothing

For each toolset folder under <ue59>/Engine/Plugins/Experimental/Toolsets/<Name> it:
  1. copies Source/<Module>/ to Source/Epic59/<Module>/ byte for byte (Epic's copyright headers stay),
  2. renames modules and reflected types whose names UE 5.8 (or HyperAI) already uses, by appending "59",
  3. applies the mechanical UE 5.8 fixes (see FIXES below and Source/Epic59/PORTING.md),
  4. adds the modules to HyperAIStudio.uplugin (Type Editor, Epic's loading phase, Win64),
  5. prints what still needs a hand edit: 5.9-only APIs and headers UE 5.8 does not have.

Edits keep each file's line endings. Re-running refuses to overwrite a ported module unless --force is
given, because hand edits live in those files; --force re-copies and re-applies the mechanical fixes only.

Paths: --ue59 (default $EPIC59_SRC), --ue58 (default $HYPERAI_BENCH_ENGINE or E:/EpicGames/UE_5.8). The
5.8 type and header index is cached in Intermediate/Epic59/ (delete it after an engine update).
Standard library only.
"""

import argparse
import json
import os
import re
import shutil
import sys
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[2]
EPIC59_DIR = PLUGIN / "Source" / "Epic59"
UPLUGIN = PLUGIN / "HyperAIStudio.uplugin"
CACHE = PLUGIN / "Intermediate" / "Epic59"
DEFAULT_UE59 = os.environ.get("EPIC59_SRC", "")
DEFAULT_UE58 = os.environ.get("HYPERAI_BENCH_ENGINE", r"E:\EpicGames\UE_5.8")
COMPAT_MODULES = ("AsyncFunctionResult", "ToolsetCore", "ToolsetRegistryEditor")
SUFFIX = "59"
MARK = "// UE 5.8:"
SOURCE_EXT = (".h", ".hpp", ".inl", ".cpp")

# Build.cs dependencies 5.9 has and 5.8 lacks: replacement (None drops the entry).
DEPENDENCY_MAP = {
    "ToolsetRegistryEngine": "ToolsetRegistryEditor",
    "AsyncFunctionResultEditor": None,
}

# Text that compiles on 5.9 only, or compiles but behaves differently on 5.8. (pattern, advice)
FLAGS = [
    (r"UToolsetRegistry::ExecuteTool\b", "5.8 returns UToolCallAsyncResultString*, not UAsyncFunctionResultString*; "
     "use auto* or the 5.8 type"),
    (r"\bUAsyncFunctionResult\w*\s*\*\s*\w+\s*=\s*UToolsetRegistry::", "assigns a 5.8 registry result to a 5.9 type"),
    (r"OnCompleted\.Add(Unique)?Dynamic\b", "5.8's OnCompleted passes UToolCallAsyncResult*; the handler must take that"),
    (r"\bUsesInstanceDispatch\b|\bUsesInstanceArgument\b|\bResolveToolDispatchTarget\b",
     "5.8's registry dispatches static tools only; non-static tools are rejected"),
    (r"\bGetToolsToHide_Implementation\b|\bIsToolsetHidden_Implementation\b",
     "compiles via IToolsetProvider, but 5.8's registry never calls it; hide with HyperAI presets or the block list"),
    (r"\bHandleScriptMessage\b|\bFScopedScriptExceptionCapture\b|Blueprint/ScriptExceptionCapture\.h",
     "5.9-only exception capture API; 5.8's FToolCallExceptionHandler has GetScriptException/CaptureErrorsIn only"),
    (r"\bFToolsetRegistry::(Get|Create)\b|\bFToolsetRegistryTestFactory\b",
     "5.9-only registry access; on 5.8 use UToolsetRegistrySubsystem::Get()"),
    (r"\bRegisterToolsetForPlugin\b|\bUnregisterToolsetsForPlugin\b|\bFToolCallContext\b\s+\w+\s*[,)]",
     "5.9-only FToolsetRegistry API"),
    (r"UToolsetLibrary::(ListObjectPropertyNames|StructToJson|IsClassAllowed|GetPluginContentPaths|"
     r"GetAssetContentRoots|ValidateAssetPath|TryValidateAssetPath|IsPathVisible|IsPathWritable|"
     r"ResolveAssetPath|ConvertToVirtualPath)\b", "UToolsetLibrary function added in 5.9; 5.8's class lacks it"),
    (r"ToolsetRegistry/AssetHelpers\.h", "5.9 ToolsetRegistryEditor helper not in the compat layer yet"),
    (r"ToolsetRegistry/(ToolsetJsonSchema|UndoCaptureParticipant|MarkdownAgentSkills|CodeBlock|"
     r"ToolsetManifestSeed|ToolsetRegistrySettings)\.h", "5.9-only ToolsetRegistry header not in the compat layer"),
    (r"ToolsetRegistry/Private/", "includes the registry's private test fixtures"),
    (r"\bUE59\b", "Epic's own 5.9 adaptation note: check it against 5.8"),
]


def log(msg):
    print(msg, flush=True)


# ------------------------------------------------------------------------------------------- text I/O

def read_text(path):
    """Text with its line endings intact (newline='' keeps \\r\\n)."""
    with open(path, encoding="utf-8-sig" if path.suffix != ".cs" else "utf-8", errors="surrogateescape",
              newline="") as f:
        return f.read()


def write_text(path, text):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    data = text.encode("utf-8", errors="surrogateescape")
    path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + data)


def eol_of(text):
    return "\r\n" if "\r\n" in text else "\n"


# ------------------------------------------------------------------------------------------- 5.8 index

TYPE_RE = re.compile(
    r"\b(UCLASS|USTRUCT|UENUM|UINTERFACE)\s*\((?:[^()]|\([^()]*\))*\)\s*"
    r"(?:class|struct|enum(?:\s+class)?)\s+(?:\w+_API\s+)?(\w+)")


def scan_types(text):
    return {m.group(2) for m in TYPE_RE.finditer(text)}


def build_ue58_index(ue58):
    CACHE.mkdir(parents=True, exist_ok=True)
    cache = CACHE / "ue58_index.json"
    if cache.exists():
        data = json.loads(cache.read_text(encoding="utf-8"))
        if data.get("engine") == str(ue58):
            return set(data["types"]), set(data["modules"]), set(data["headers"])
    log("Indexing UE 5.8 reflected types, modules and headers (cached in %s)." % cache.relative_to(PLUGIN))
    types, modules, headers = set(), set(), set()
    for top in ("Source", "Plugins", "Platforms"):
        root = Path(ue58) / "Engine" / top
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ("Intermediate", "Binaries", "ThirdParty", "Content")]
            for name in filenames:
                if name.endswith(".Build.cs"):
                    modules.add(name[:-len(".Build.cs")])
                elif name.endswith((".h", ".inl", ".hpp")):
                    full = Path(dirpath) / name
                    parts = full.as_posix().lower().split("/")
                    for depth in range(1, min(4, len(parts)) + 1):
                        headers.add("/".join(parts[-depth:]))
                    if name.endswith(".h"):
                        try:
                            text = full.read_text(encoding="utf-8", errors="replace")
                        except OSError:
                            continue
                        if "GENERATED_" in text:
                            types |= scan_types(text)
    cache.write_text(json.dumps({"engine": str(ue58), "types": sorted(types), "modules": sorted(modules),
                                 "headers": sorted(headers)}), encoding="utf-8")
    return types, modules, headers


def hyperai_names():
    """Modules and reflected types HyperAI already declares, outside the Epic59 ports."""
    types, modules = set(), set()
    for path in (PLUGIN / "Source").rglob("*"):
        if EPIC59_DIR in path.parents:
            continue
        if path.name.endswith(".Build.cs"):
            modules.add(path.name[:-len(".Build.cs")])
        elif path.suffix == ".h":
            types |= scan_types(path.read_text(encoding="utf-8", errors="replace"))
    return types, modules


def compat_headers():
    """Headers the compat modules and every ported module provide, as lowercase include suffixes."""
    found = set()
    for path in EPIC59_DIR.rglob("*.h"):
        parts = path.as_posix().lower().split("/")
        for depth in range(1, 4):
            found.add("/".join(parts[-depth:]))
    return found


# ------------------------------------------------------------------------------------------- helpers

def find_balanced(text, open_index):
    """Index just past the parenthesis or brace that closes the one at open_index."""
    opener = text[open_index]
    closer = {"(": ")", "{": "}"}[opener]
    depth = 0
    i = open_index
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def line_of(text, index):
    return text.count("\n", 0, index) + 1


class Port:
    def __init__(self, args):
        self.args = args
        self.ue59 = Path(args.ue59)
        self.ue58 = Path(args.ue58)
        self.toolsets_root = self.ue59 / "Engine" / "Plugins" / "Experimental" / "Toolsets"
        self.hand_edits = []   # (file, line, message)
        self.notes = []
        self.changed = 0

    # ------------------------------------------------------------------ discovery

    def toolset_dir(self, name):
        direct = self.toolsets_root / name
        if direct.is_dir():
            return direct
        matches = [p.parent for p in self.toolsets_root.rglob(name + ".uplugin")]
        if not matches:
            sys.exit("No toolset named %s under %s" % (name, self.toolsets_root))
        return matches[0]

    def descriptor(self, folder):
        uplugins = list(folder.glob("*.uplugin"))
        if not uplugins:
            sys.exit("No .uplugin in %s" % folder)
        return json.loads(read_text(uplugins[0]))

    # ------------------------------------------------------------------ main entry

    def run(self, names):
        types58, modules58, self.headers58 = build_ue58_index(self.ue58)
        hyper_types, hyper_modules = hyperai_names()
        taken_types = types58 | hyper_types
        taken_modules = modules58 | hyper_modules
        for name in names:
            self.port_one(name, taken_types, taken_modules)
        self.report()

    def port_one(self, name, taken_types, taken_modules):
        folder = self.toolset_dir(name)
        desc = self.descriptor(folder)
        log("\n== %s (%s)" % (name, folder))
        source_modules = sorted(p.parent for p in (folder / "Source").rglob("*.Build.cs"))
        module_names = [p.name for p in source_modules]
        module_renames = {m: m + SUFFIX for m in module_names if m in taken_modules and m not in COMPAT_MODULES}
        for old, new in module_renames.items():
            self.notes.append("%s: module %s exists in UE 5.8 or HyperAI; ported as %s" % (name, old, new))

        # Copy.
        copied = []
        for src in source_modules:
            dest = EPIC59_DIR / module_renames.get(src.name, src.name)
            if dest.exists() and not self.args.check:
                if not self.args.force:
                    sys.exit("%s already exists; pass --force to re-copy it (hand edits there are lost)"
                             % dest.relative_to(PLUGIN))
                if not self.args.check:
                    shutil.rmtree(dest)
            if not self.args.check:
                shutil.copytree(src, dest)
            copied.append((src, dest))

        # Type renames: reflected types this toolset declares that 5.8 or HyperAI already has.
        declared = set()
        for src, _ in copied:
            for h in src.rglob("*.h"):
                declared |= scan_types(read_text(h))
        type_renames = {}
        for t in sorted(declared):
            if t in taken_types:
                type_renames[t] = t + SUFFIX
                # An interface's I-class follows its U-class.
                if t.startswith("U") and ("I" + t[1:]) not in type_renames:
                    type_renames["I" + t[1:]] = "I" + t[1:] + SUFFIX
        for old, new in type_renames.items():
            if old in declared:
                self.notes.append("%s: type %s exists in UE 5.8 or HyperAI; renamed %s" % (name, old, new))

        if self.args.check:
            for src, dest in copied:
                for path in sorted(src.rglob("*")):
                    if path.suffix in SOURCE_EXT:
                        rel = Path(dest.name) / path.relative_to(src)
                        self.fix_source(rel, read_text(path), module_renames, type_renames, declared, dry=True)
            return

        for src, dest in copied:
            old_module = src.name
            new_module = dest.name
            if old_module != new_module:
                old_cs = dest / (old_module + ".Build.cs")
                old_cs.rename(dest / (new_module + ".Build.cs"))
            for path in sorted(dest.rglob("*")):
                if path.suffix in SOURCE_EXT:
                    text = read_text(path)
                    fixed = self.fix_source(path.relative_to(EPIC59_DIR), text, module_renames, type_renames,
                                            declared)
                    if fixed != text:
                        write_text(path, fixed)
                        self.changed += 1
                elif path.name.endswith(".Build.cs"):
                    text = read_text(path)
                    fixed = self.fix_build_cs(path.relative_to(EPIC59_DIR), text, module_renames)
                    if fixed != text:
                        write_text(path, fixed)
                        self.changed += 1

        self.update_uplugin(name, desc, module_renames)

    # ------------------------------------------------------------------ source fixes

    def fix_source(self, rel, text, module_renames, type_renames, declared, dry=False):
        eol = eol_of(text)

        # Renamed modules: API macro, IMPLEMENT_MODULE, toolset and script-package names in strings.
        for old, new in module_renames.items():
            text = re.sub(r"\b%s_API\b" % old.upper(), "%s_API" % new.upper(), text)
            text = re.sub(r"(IMPLEMENT_MODULE\s*\(\s*\w+\s*,\s*)%s(\s*\))" % old, r"\g<1>%s\2" % new, text)
            text = text.replace('TEXT("%s.' % old, 'TEXT("%s.' % new).replace("/Script/%s." % old, "/Script/%s." % new)
            for m in re.finditer(r'(?<!Category = )(?<!Category=)"%s"' % re.escape(old), text):
                self.hand(rel, text, m.start(), 'string "%s" names the renamed module %s' % (old, new))

        # Renamed types (word boundary, outside of #include lines so file names stay).
        for old, new in type_renames.items():
            text = re.sub(r'(?m)^(?!\s*#\s*include)(.*)$',
                          lambda m: re.sub(r"\b%s\b" % re.escape(old), new, m.group(1)), text) \
                if re.search(r"\b%s\b" % re.escape(old), text) else text
            short_old, short_new = old[1:], new[1:]
            # A qualified name ("Module.LogsToolset", "/Script/Module.LogsToolset") is what the registry sees.
            for m in re.finditer(r'"[\w/]*[./]%s\b[^"\n]*"' % re.escape(short_old), text):
                self.hand(rel, text, m.start(), "string mentions %s (renamed %s): check it" % (short_old, short_new))

        # 5.9's ToolsetDefinition.h -> compat header with the 5.9 additions.
        text = re.sub(r'#include\s+"ToolsetRegistry/ToolsetDefinition\.h"',
                      '#include "ToolsetRegistry/ToolsetDefinition59.h" %s 5.9 ToolsetDefinition additions' % MARK,
                      text)

        # UToolsetDefinition's 5.9 statics moved to UE::ToolsetRegistry; the compat header declares them there.
        new_text = re.sub(r"\bUToolsetDefinition::(IsFunctionAIRuntimeCallable|GetFunctionAccessMode|"
                          r"IsFunctionMarkedAsTool)\b", r"UE::ToolsetRegistry::\1", text)
        if new_text != text and "ToolsetDefinition59.h" not in new_text:
            new_text = self.add_include(new_text, "ToolsetRegistry/ToolsetDefinition59.h", eol)
        text = new_text

        # Direct toolset classes gain IToolsetProvider, 5.9's base interface (5.8's base lacks it).
        text = re.sub(r"(\bclass\s+(?:\w+_API\s+)?\w+\s*(?:final\s*)?:\s*public\s+UToolsetDefinition\b)"
                      r"(?!\s*,\s*public\s+IToolsetProvider)([^\r\n]*)",
                      r"\1, public IToolsetProvider\2 %s 5.9's UToolsetDefinition is an IToolsetProvider" % MARK, text)

        # 5.8 has no cooked-runtime toolsets: drop the opt-in.
        text = re.sub(r"(?m)^([ \t]*)(UToolsetRegistry::EnableCookedRuntimeSupport\s*\([^;]*\);)",
                      r"\1%s no cooked-runtime toolsets in 5.8. \2" % MARK, text)

        if rel.suffix == ".h":
            text = self.fix_ufunctions(rel, text)

        for pattern, advice in FLAGS:
            for m in re.finditer(pattern, text):
                if MARK in text[text.rfind("\n", 0, m.start()) + 1:text.find("\n", m.end())]:
                    continue
                self.hand(rel, text, m.start(), advice)

        for m in re.finditer(r'(?m)^\s*#\s*include\s+"([^"]+)"', text):
            inc = m.group(1)
            if inc.endswith(".generated.h") or "UE_INLINE" in inc:
                continue
            key = inc.lower().replace("\\", "/")
            if key in self.headers58 or key in self.port_headers():
                continue
            self.hand(rel, text, m.start(), 'include "%s" is not in UE 5.8 or the ports' % inc)
        return text

    def port_headers(self):
        if not hasattr(self, "_port_headers"):
            self._port_headers = compat_headers()
            for path in self.toolsets_root.rglob("*.h"):
                parts = path.as_posix().lower().split("/")
                for depth in range(1, 4):
                    self._port_headers.add("/".join(parts[-depth:]))
        return self._port_headers

    def add_include(self, text, header, eol):
        line = '#include "%s" %s 5.9 ToolsetDefinition additions' % (header, MARK)
        m = list(re.finditer(r'(?m)^#include\s+"[^"]+"[^\r\n]*$', text))
        m = [x for x in m if "generated" not in x.group(0) and "UE_INLINE" not in x.group(0)]
        if not m:
            return line + eol + text
        end = m[-1].end()
        return text[:end] + eol + line + text[end:]

    def fix_ufunctions(self, rel, text):
        """Within toolset classes: AIRuntimeCallable implies AICallable (5.8 knows only the latter), and
        an unmarked UFUNCTION gets AIIgnore (5.9 silently skips it; 5.8 logs an error)."""
        out = []
        pos = 0
        for cls in re.finditer(r"\bclass\s+(?:\w+_API\s+)?(\w+)\s*(?:final\s*)?:\s*public\s+(\w+)", text):
            base = cls.group(2)
            if base != "UToolsetDefinition" and base not in self.toolset_classes_seen:
                continue
            self.toolset_classes_seen.add(cls.group(1))
            brace = text.find("{", cls.end())
            if brace < 0:
                continue
            end = find_balanced(text, brace)
            out.append(text[pos:brace])
            out.append(self.fix_class_body(rel, text, brace, end))
            pos = end
        out.append(text[pos:])
        return "".join(out)

    toolset_classes_seen = set()

    def fix_class_body(self, rel, text, start, end):
        body = text[start:end]
        result = []
        pos = 0
        for m in re.finditer(r"\bUFUNCTION\s*\(", body):
            open_index = m.end() - 1
            close = find_balanced(body, open_index)
            spec = body[open_index + 1:close - 1]
            new_spec = spec
            has_meta = re.search(r"\bmeta\s*=\s*\(", spec)
            names = set(re.findall(r"\b(AICallable|AIRuntimeCallable|AIIgnore)\b", spec))
            if "AIRuntimeCallable" in names and "AICallable" not in names:
                new_spec = re.sub(r"\bAIRuntimeCallable\b", "AICallable, AIRuntimeCallable", spec, count=1)
            elif not names:
                if has_meta:
                    meta_open = has_meta.end()
                    new_spec = spec[:meta_open] + "AIIgnore, " + spec[meta_open:]
                else:
                    new_spec = (spec + ", " if spec.strip() else "") + "meta = (AIIgnore)"
            # A tool must be static on 5.8.
            after = body[close:close + 400]
            decl = after.split(";", 1)[0]
            if names & {"AICallable", "AIRuntimeCallable"} and not re.search(r"\bstatic\b", decl):
                self.hand(rel, text, start + m.start(), "non-static tool: 5.8's registry rejects it")
            result.append(body[pos:open_index + 1])
            result.append(new_spec)
            pos = close - 1
        result.append(body[pos:])
        return "".join(result)

    # ------------------------------------------------------------------ Build.cs

    def fix_build_cs(self, rel, text, module_renames):
        for old, new in module_renames.items():
            text = re.sub(r"\b(class|public)\s+%s\b" % old, r"\1 %s" % new, text)
            text = text.replace('"%s"' % old, '"%s"' % new)
        for old, new in DEPENDENCY_MAP.items():
            if new is None:
                text = re.sub(r'(?m)^[ \t]*"%s",?[^\r\n]*\r?\n' % old, "", text)
            elif '"%s"' % new in text:
                text = re.sub(r'(?m)^[ \t]*"%s",?[^\r\n]*\r?\n' % old, "", text)
            else:
                text = text.replace('"%s"' % old, '"%s" /* UE 5.8: was %s */' % (new, old))
        # Every module that uses the 5.8 registry also gets the compat layer (5.9-only headers, IToolsetProvider).
        if '"ToolsetRegistry"' in text and '"ToolsetRegistryEditor"' not in text:
            text = re.sub(r'(?m)^([ \t]*)"ToolsetRegistry",([^\r\n]*)(\r?)$',
                          r'\1"ToolsetRegistry",\2\3\n\1"ToolsetRegistryEditor", %s compat layer for 5.9 headers\3'
                          % MARK, text, count=1)
            if '"ToolsetRegistryEditor"' not in text:
                self.hand(rel, text, text.find('"ToolsetRegistry"'),
                          'add "ToolsetRegistryEditor" next to "ToolsetRegistry" by hand')
        return text

    # ------------------------------------------------------------------ uplugin

    def update_uplugin(self, name, desc, module_renames):
        text = read_text(UPLUGIN)
        eol = eol_of(text)
        existing = set(re.findall(r'"Name"\s*:\s*"([^"]+)"', text))
        start = text.find('"Modules"')
        close = text.find("\n\t],", start)
        if start < 0 or close < 0:
            self.notes.append("HyperAIStudio.uplugin: Modules array not found; add the modules by hand")
            return
        entries = []
        for module in desc.get("Modules", []):
            mod_name = module_renames.get(module["Name"], module["Name"])
            if mod_name in existing:
                continue
            phase = module.get("LoadingPhase", "Default")
            entry = [
                "\t\t{",
                '\t\t\t"Name": "%s",' % mod_name,
                '\t\t\t"Type": "Editor",',
                '\t\t\t"LoadingPhase": "%s",' % phase,
                '\t\t\t"PlatformAllowList": [',
                '\t\t\t\t"Win64"',
                "\t\t\t]",
                "\t\t}",
            ]
            entries.append(eol.join(entry))
            if module.get("Type") not in ("Editor", None):
                self.notes.append("%s: module %s is %s in 5.9; added as Editor (HyperAIStudio is an editor plugin)"
                                  % (name, mod_name, module.get("Type")))
        if entries:
            line_end = text.rfind("}", start, close + 1)
            text = text[:line_end + 1] + "," + eol + ("," + eol).join(entries) + text[line_end + 1:]
            write_text(UPLUGIN, text)
            self.changed += 1
        plugin_names = set(re.findall(r'"Name"\s*:\s*"([^"]+)"', text[text.find('"Plugins"'):]))
        for dep in desc.get("Plugins", []):
            if dep.get("Name") not in plugin_names and dep.get("Name") != "ToolsetRegistry":
                self.notes.append("%s: needs plugin %s; add it to HyperAIStudio.uplugin's Plugins (Enabled false "
                                  "plus a load guard if it is optional)" % (name, dep.get("Name")))

    # ------------------------------------------------------------------ reporting

    def hand(self, rel, text, index, message):
        self.hand_edits.append((str(rel).replace("\\", "/"), line_of(text, index), message))

    def report(self):
        if self.notes:
            log("\nNotes:")
            for note in self.notes:
                log("  " + note)
        seen = set()
        unique = [h for h in self.hand_edits if not (h in seen or seen.add(h))]
        log("\nNeeds a look (%d):" % len(unique))
        for path, line, message in unique:
            log("  %s:%d: %s" % (path, line, message))
        if not self.args.check:
            log("\n%d files changed. Mark each hand edit with a short '%s' comment." % (self.changed, MARK))


def list_toolsets(args):
    root = Path(args.ue59) / "Engine" / "Plugins" / "Experimental" / "Toolsets"
    for folder in sorted(p for p in root.iterdir() if p.is_dir()):
        modules = sorted(p.name[:-len(".Build.cs")] for p in folder.rglob("*.Build.cs"))
        ported = [m for m in modules if (EPIC59_DIR / m).exists() or (EPIC59_DIR / (m + SUFFIX)).exists()]
        state = "ported" if modules and len(ported) == len(modules) else ("partial" if ported else "-")
        log("%-40s %-8s %s" % (folder.name, state, ", ".join(modules)))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("toolsets", nargs="*")
    parser.add_argument("--ue59", default=DEFAULT_UE59, help="UE 5.9 source checkout (default $EPIC59_SRC)")
    parser.add_argument("--ue58", default=DEFAULT_UE58, help="UE 5.8 engine (default E:/EpicGames/UE_5.8)")
    parser.add_argument("--force", action="store_true", help="re-copy a module that is already ported")
    parser.add_argument("--check", action="store_true", help="report only; write nothing")
    parser.add_argument("--list", action="store_true", help="list the 5.9 toolsets and their port state")
    args = parser.parse_args()
    if not args.ue59 or not Path(args.ue59, "Engine").is_dir():
        sys.exit("Pass --ue59 <UE 5.9 source root> or set EPIC59_SRC")
    if args.list:
        list_toolsets(args)
        return 0
    if not args.toolsets:
        parser.print_help()
        return 2
    Port(args).run(args.toolsets)
    return 0


if __name__ == "__main__":
    sys.exit(main())
