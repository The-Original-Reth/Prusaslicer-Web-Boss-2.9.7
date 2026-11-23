# Prusaslicer - Web Boss 2.9.7: design

A web page that works like [PrusaSlicer BOSS](https://github.com/mjonuschat/PrusaSlicer)
(pinned to its 2.9.7 release, `version_2.9.7+boss`): the 3D scene is drawn in the browser, the slicing runs
on the server with BOSS's own slicing library.

## Why a long-running engine, not the command-line slicer

PrusaSlicer feels fast when you tweak a setting because its library (`libslic3r`) keeps
the sliced result in memory and only redoes the steps a change affects:

```
per object: slice → perimeters → prepare infill → infill → ironing → support spots →
            supports → curled extrusions → overhangs
whole print: wipe tower → skirt/brim → G-code export
```

Changing infill density redoes infill and later steps, but keeps slicing and perimeters.
Changing the bed temperature only redoes the G-code export. The command-line slicer throws
all of this away each run, so every change would be a full re-slice.

So the server runs an **engine**: a small C++ program built from BOSS's own source tree
that holds one project (models + settings + sliced state) in memory and applies changes
to it, the same way BOSS's GUI does (`Print::apply` → `Print::process` →
`Print::export_gcode`).

## Pieces

```
browser (React + three.js)          gateway (Python)              engine (C++, libslic3r)
 3D bed, move/rotate/scale    ⇄      sessions, uploads,      ⇄     one per open project,
 settings pages                      profiles, files,              JSON lines on stdin/stdout:
 G-code preview (drawn here)         starts/stops engines          load, transform, set, slice
```

- **engine**: one process per open project, no network code of its own. Commands in,
  events out (progress, statistics, which steps were reused or recomputed, results).
- **gateway**: serves the web page, accepts uploads, keeps a websocket per browser tab,
  starts and talks to engines, serves profiles and G-code.
- **web**: everything you see and touch; the 3D view and the G-code preview are drawn
  on your device's GPU.

## Milestones

1. **Engine spike** (done): load a model, slice, change one setting, re-slice,
   and show that only the affected steps were recomputed, with timings.
2. **Core web page** (done): load STL/3MF, arrange/move/rotate/scale on the bed, choose
   printer/filament/print presets from BOSS's bundled profiles, slice, preview, download.

   - Profiles: the engine loads one vendor's bundle (`resources/profiles/<vendor>.ini`) at a
     time with libslic3r's own `PresetBundle`, and lists the prints and filaments compatible
     with the chosen printer the same way BOSS's CLI does. The gateway asks a short-lived
     engine for the catalog of every vendor's printers once at startup.
   - Geometry goes to the browser as raw binary meshes (one file per object, placed by
     instance matrices); the engine stays the source of truth for placement (it puts
     objects back on the bed after every change).
   - G-code thumbnails: BOSS renders them with OpenGL during export, which the engine can't.
     The page draws them with three.js before slicing (`web/src/thumbnails.ts`, following
     `GLCanvas3D::_render_thumbnail_internal`: camera, lights, colors), uploads raw pixels,
     and the engine hands them to `Print::export_gcode` through its thumbnail callback, so
     libslic3r does the PNG/JPG/QOI encoding and the binary G-code blocks.
   - The preview comes from the G-code processor's own move list (`GCodeProcessorResult`),
     not from parsing G-code, so it works for binary G-code too. The browser draws each
     extrusion as a box instance stretched in a vertex shader.
3. **Settings tabs** (this branch): generated from the engine's own setting definitions (label, tooltip,
   group, Simple/Advanced/Expert), not hand-built.

   - Layout: BOSS builds its tabs in GUI code (`src/slic3r/GUI/Tab.cpp`), which the engine
     doesn't compile. `engine/tools/tab_layout.py` reads the pages, groups and lines out of
     that file during the image build, and fails the build on calls it doesn't recognize, so
     a new BOSS version can't silently lose settings. The printer tab's variable pages
     (machine limits, one per extruder, single extruder MM) are assembled in the page with
     BOSS's rules.
   - Definitions (label, tooltip, units, limits, choices, mode) come from libslic3r's
     `print_config_def` through the engine's `schema` command; the gateway asks once at startup.
   - Edits go into the bundle's edited presets, as in the GUI, so "modified" is libslic3r's own
     `current_dirty_options()`, and the slicing config is `PresetBundle::full_config()`.
     Switching presets transfers or discards the changes, like BOSS's unsaved-changes dialog.
   - Greyed-out options follow `ConfigManipulation::toggle_print_fff_options` and the tabs'
     `toggle_options`, ported to `engine/src/settings.cpp` (that code lives in the GUI library).
     Not ported: the GUI's automatic fixes and their dialogs (e.g. spiral vase forcing other
     settings), and the computed description lines.
   - User presets are saved with libslic3r's own `save_current_preset()` into
     `$DATA_DIR/presets/<vendor>/{print,filament,printer}/`, the layout of BOSS's data folder,
     and loaded with `load_presets()` whenever an engine loads that vendor. A new preset
     inherits from the system preset it started from. They're shared by every project on
     the server; there are no accounts.
   - Not yet: physical printers, the bed texture and custom bed shapes (non-rectangular beds
     show read-only), editing compatibility conditions, renaming presets, "Save" in the
     unsaved-changes dialog.
4. **Advanced tools**: modifiers, painted supports and seams, cut, measure, BOSS-only features.

   - Per-object settings (this branch): overrides live in `ModelObject::config`, so
     `Print::apply()` re-slices only that object. The options offered are BOSS's
     (`SettingsFactory::get_options`: region + object settings with a category), and the
     quick-add groups are read from `GUI_Factories.cpp` at build time like the tab layout.
   - Parts and modifiers (this branch): each `ModelVolume` goes to the browser as its own mesh
     file with its own matrix, drawn inside its instance. Generic shapes are sized and placed
     like `ObjectList::load_generic_subobject`; parts and modifiers take region settings.
     Not yet: loading a part or modifier from a file, text and SVG shapes, layer range
     modifiers, numeric manipulation of a part.

## Risks

- `libslic3r` is not a public API; moving to a newer BOSS may need engine changes.
  Pinning the version keeps that a deliberate step.
- Full parity with BOSS's interface is a long project; milestones 1-2 are the first
  usable result.
