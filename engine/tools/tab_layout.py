#!/usr/bin/env python3
"""Reads the layout of BOSS's settings tabs out of its own GUI source (src/slic3r/GUI/Tab.cpp)
and writes it as JSON for the web page: tabs > pages > groups > lines > options.

BOSS builds its Print/Filament/Printer Settings tabs in code, with calls like

    auto page = add_options_page(L("Layers and perimeters"), "layers");
    auto optgroup = page->new_optgroup(L("Layer height"));
    optgroup->append_single_option_line("layer_height", category_path + "layer-height");

so the web page follows BOSS's own pages, groups and order instead of a hand-made copy, and
picks up changes when the pinned BOSS version moves. The calls are regular enough to read
with a few patterns; anything unexpected that names an option is reported and fails the build.

It also reads the quick-add groups of the object list's "Add settings" menu from
src/slic3r/GUI/GUI_Factories.cpp.

Usage: tab_layout.py <Tab.cpp> <GUI_Factories.cpp> <output.json>
"""

import json
import re
import sys

STR = r'(?:L\()?"((?:[^"\\]|\\.)*)"\)?'

PATTERNS = [
    ("page", re.compile(r'add_options_page\(\s*' + STR + r'\s*,\s*"([^"]*)"')),
    ("extruder_page", re.compile(r'add_options_page\(\s*page_name\s*,\s*"([^"]*)"')),
    ("group", re.compile(r'new_optgroup\(\s*' + STR + r'\s*(?:,\s*(\d+))?\s*\)')),
    ("single", re.compile(r'append_single_option_line\(\s*"([^"]+)"\s*(?:,\s*([^;]*?))?\)\s*;')),
    ("single_var", re.compile(r'append_single_option_line\(\s*option\s*(?:,\s*([^;]*?))?\)\s*;')),
    ("option_var", re.compile(r'(?:Option\s+)?\boption\s*=\s*optgroup->get_option\(\s*"([^"]+)"')),
    ("option_def", re.compile(r'\bOption\s+option\(\s*def\s*,\s*"([^"]+)"\s*\)')),
    ("option_flag", re.compile(r'\boption\.opt\.(full_width|is_code|height)\s*=\s*([^;]+);')),
    ("line_new", re.compile(r'(?:\bLine\s+)?\bline\s*=?\s*(?:Line\s*)?\{\s*' + STR + r'\s*,\s*""\s*\}')),
    ("line_create", re.compile(r'\bline\s*=\s*optgroup->create_single_option_line\(\s*"([^"]+)"')),
    ("line_option", re.compile(r'\bline\.append_option\(\s*optgroup->get_option\(\s*"([^"]+)"')),
    ("line_path", re.compile(r'\bline\.label_path\s*=\s*([^;]+);')),
    ("append_line", re.compile(r'optgroup->append_line\(\s*line\s*\)')),
    ("widget_line", re.compile(r'create_line_with_widget\(\s*optgroup(?:\.get\(\))?\s*,\s*"([^"]+)"\s*,\s*([^,]*),')),
    ("near_label", re.compile(r'create_line_with_near_label_widget\(\s*optgroup\s*,\s*"([^"]+)"')),
    ("limits_line", re.compile(r'append_option_line\(\s*optgroup\s*,\s*"([^"]+)"(\s*\+\s*axis)?\s*\)')),
    ("overrides_page", re.compile(r'\badd_filament_overrides_page\(\s*\)\s*;')),
    ("string_var", re.compile(r'(?:(?:const\s+)?std::string\s+)?\b(category_path|path|scarf_seam_path)\s*(?:=\s*|\{)("[^"]*")\}?\s*;')),
    ("condition", re.compile(r'\bif\s*\(\s*(m_supports_travel_acceleration|m_supports_min_feedrates)\s*\)')),
]

# Mentions of options the patterns above don't account for mean the source changed shape
SUSPICIOUS = re.compile(r'append_single_option_line|append_option\(|create_line_with|append_option_line\(')
# The machine limits page loops over these axes
AXES = ["x", "y", "z", "e"]


def function_body(source, signature):
    start = source.find(signature)
    if start < 0:
        sys.exit(f"tab_layout: {signature} not found in Tab.cpp")
    i = source.index("{", start)
    depth = 0
    for j in range(i, len(source)):
        if source[j] == "{":
            depth += 1
        elif source[j] == "}":
            depth -= 1
            if depth == 0:
                return source[i + 1:j]
    sys.exit(f"tab_layout: unbalanced braces in {signature}")


def strip_comments(code):
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    return re.sub(r"//[^\n]*", "", code)


def condition_ranges(body):
    """(start, end, name) spans guarded by `if (m_supports_...)`, with or without braces."""
    spans = []
    for m in PATTERNS[-1][1].finditer(body):
        k = m.end()
        while body[k].isspace():
            k += 1
        if body[k] == "{":
            depth = 0
            for j in range(k, len(body)):
                depth += {"{": 1, "}": -1}.get(body[j], 0)
                if depth == 0:
                    break
            spans.append((k, j, m.group(1)))
        else:
            spans.append((k, body.index(";", k), m.group(1)))
    return spans


def condition_name(name):
    return {"m_supports_travel_acceleration": "supports_travel_acceleration",
            "m_supports_min_feedrates": "supports_min_feedrates"}[name]


def help_path(expr, strings):
    """Evaluates `category_path + "layer-height"` style help link arguments."""
    if not expr:
        return None
    parts = []
    for term in expr.split("+"):
        term = term.strip()
        if term.startswith('"') and term.endswith('"'):
            parts.append(term[1:-1])
        elif term in strings:
            parts.append(strings[term])
        else:
            return None  # e.g. "", extruder index arguments
    path = "".join(parts)
    return path or None


def parse(body, overrides, where):
    body = strip_comments(body)
    events = []
    for kind, pattern in PATTERNS:
        if kind == "condition":
            continue
        for m in pattern.finditer(body):
            events.append((m.start(), kind, m))
    events.sort(key=lambda e: e[0])
    conditions = condition_ranges(body)

    def when(pos):
        for a, b, name in conditions:
            if a <= pos <= b:
                return condition_name(name)
        return None

    # Every suspicious call must be covered by a pattern
    for m in SUSPICIOUS.finditer(body):
        if not any(e.start() <= m.start() < e.end() for _, _, e in events):
            line_start = body.rfind("\n", 0, m.start()) + 1
            snippet = body[line_start:body.find("\n", m.start())].strip()
            sys.exit(f"tab_layout: {where}: can't read: {snippet}")

    pages, page, group = [], None, None
    strings = {}
    pending_option = None  # (key, flags) from `option = optgroup->get_option(...)`
    line = None            # line being built with `line.append_option(...)`

    def add_line(entry, pos):
        if group is None:
            sys.exit(f"tab_layout: {where}: option outside a group")
        cond = when(pos)
        if cond:
            entry["when"] = cond
        group["lines"].append(entry)

    for pos, kind, m in events:
        if kind == "page":
            page = {"title": m.group(1), "icon": m.group(2), "groups": []}
            pages.append(page)
            group = None
        elif kind == "extruder_page":
            page = {"title": "Extruder", "icon": m.group(1), "groups": []}
            pages.append(page)
            group = None
        elif kind == "group":
            if page is None:
                sys.exit(f"tab_layout: {where}: group outside a page")
            group = {"title": m.group(1), "lines": []}
            if m.group(2) == "0":
                group["no_labels"] = True
            if when(pos):
                group["when"] = when(pos)
            page["groups"].append(group)
        elif kind == "string_var":
            strings[m.group(1)] = m.group(2)[1:-1]
        elif kind == "single":
            entry = {"options": [{"key": m.group(1)}]}
            path = help_path(m.group(2), strings)
            if path:
                entry["help"] = path
            add_line(entry, pos)
        elif kind in ("option_var", "option_def"):
            pending_option = {"key": m.group(1)}
        elif kind == "option_flag":
            if pending_option is None:
                sys.exit(f"tab_layout: {where}: option flag without an option")
            name, value = m.group(1), m.group(2).strip()
            if name == "height":
                pending_option["height"] = float(eval(value, {}, {"gcode_field_height": 15, "notes_field_height": 25}))
            else:
                pending_option[name] = value == "true"
        elif kind == "single_var":
            if pending_option is None:
                sys.exit(f"tab_layout: {where}: append of an unknown option")
            entry = {"options": [pending_option]}
            path = help_path(m.group(1), strings)
            if path:
                entry["help"] = path
            add_line(entry, pos)
            pending_option = None
        elif kind == "line_new":
            line = {"label": m.group(1), "options": []}
        elif kind == "line_create":
            line = {"label": "", "options": [{"key": m.group(1)}]}
        elif kind == "line_option":
            if line is None:
                sys.exit(f"tab_layout: {where}: append_option without a line")
            line["options"].append({"key": m.group(1)})
        elif kind == "line_path":
            if line is not None:
                path = help_path(m.group(1), strings)
                if path:
                    line["help"] = path
        elif kind == "append_line":
            # Lines with no options are BOSS's computed description texts; they're skipped
            if line is not None and line["options"]:
                entry = {"options": line["options"]}
                if line["label"]:
                    entry["label"] = line["label"]
                if line.get("help"):
                    entry["help"] = line["help"]
                add_line(entry, pos)
            line = None
        elif kind == "widget_line":
            entry = {"options": [{"key": m.group(1), "widget": m.group(1)}]}
            path = help_path(m.group(2), strings)
            if path:
                entry["help"] = path
            add_line(entry, pos)
        elif kind == "near_label":
            add_line({"options": [{"key": m.group(1), "widget": "override"}]}, pos)
        elif kind == "limits_line":
            keys = [m.group(1) + a for a in AXES] if m.group(2) else [m.group(1)]
            for key in keys:
                # Two columns: normal and stealth mode values (index 0 and 1)
                add_line({"options": [{"key": key, "columns": 2}]}, pos)
        elif kind == "overrides_page":
            page = {"title": "Filament Overrides", "icon": "wrench", "groups": []}
            for title, keys in overrides:
                page["groups"].append({"title": title,
                                       "lines": [{"options": [{"key": k, "widget": "override"}]} for k in keys]})
            pages.append(page)
            group = None
    return pages


def filament_overrides(source):
    m = re.search(r"filament_overrides_option_keys\s*\{(.*?)\n\};", source, re.S)
    if not m:
        sys.exit("tab_layout: filament_overrides_option_keys not found")
    block = strip_comments(m.group(1))
    groups = []
    for g in re.finditer(r'\{\s*"([^"]+)"\s*,\s*\{([^}]*)\}\s*\}', block):
        groups.append((g.group(1), re.findall(r'"([^"]+)"', g.group(2))))
    return groups


def only_page(pages, title, where):
    found = [p for p in pages if p["title"] == title]
    if len(found) != 1:
        sys.exit(f"tab_layout: {where}: expected one '{title}' page")
    return found[0]


def object_bundles(path):
    """The quick-add groups of BOSS's object settings menu (FREQ_SETTINGS_BUNDLE_FFF in
    GUI_Factories.cpp): {"title": ..., "keys": [...]}"""
    source = open(path, encoding="utf-8").read()
    m = re.search(r"FREQ_SETTINGS_BUNDLE_FFF\s*=\s*\{(.*?)\n\};", source, re.S)
    if not m:
        sys.exit("tab_layout: FREQ_SETTINGS_BUNDLE_FFF not found")
    block = strip_comments(m.group(1))
    bundles = [{"title": g.group(1), "keys": re.findall(r'"([^"]+)"', g.group(2))}
               for g in re.finditer(r'\{\s*' + STR + r'\s*,\s*\{([^}]*)\}\s*\}', block)]
    if not bundles:
        sys.exit("tab_layout: no bundles in FREQ_SETTINGS_BUNDLE_FFF")
    return bundles


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source = open(sys.argv[1], encoding="utf-8").read()
    overrides = filament_overrides(source)

    printer_pages = parse(function_body(source, "void TabPrinter::build_fff()"), overrides, "TabPrinter::build_fff")
    layout = {
        "print": parse(function_body(source, "void TabPrint::build()"), overrides, "TabPrint::build"),
        "filament": parse(function_body(source, "void TabFilament::build()"), overrides, "TabFilament::build"),
        # The printer tab's pages depend on the printer: BOSS puts "Machine limits" (for
        # Marlin, RepRapFirmware and Klipper), one page per extruder and, for single extruder
        # multi-material printers, "Single extruder MM setup" between these (see
        # TabPrinter::build_unregular_pages)
        "printer": printer_pages,
        "printer_machine_limits": only_page(
            parse(function_body(source, "PageShp TabPrinter::build_kinematics_page()"), overrides, "build_kinematics_page"),
            "Machine limits", "build_kinematics_page"),
        "printer_extruder": only_page(
            parse(function_body(source, "void TabPrinter::build_extruder_pages("), overrides, "build_extruder_pages"),
            "Extruder", "build_extruder_pages"),
        "printer_single_extruder_mm": only_page(
            parse(function_body(source, "void TabPrinter::build_unregular_pages("), overrides, "build_unregular_pages"),
            "Single extruder MM setup", "build_unregular_pages"),
        # Object settings: quick-add groups of the object list's menu
        "object_bundles": object_bundles(sys.argv[2]),
    }
    with open(sys.argv[3], "w", encoding="utf-8") as f:
        json.dump(layout, f, indent=1)
    count = sum(len(l["options"]) for tab in ("print", "filament", "printer") for p in layout[tab] for g in p["groups"] for l in g["lines"])
    print(f"tab_layout: {count} options on {sum(len(layout[t]) for t in ('print', 'filament', 'printer'))} pages")


if __name__ == "__main__":
    main()
