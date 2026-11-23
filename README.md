# Prusaslicer - Web Boss 2.9.7

[PrusaSlicer BOSS](https://github.com/mjonuschat/PrusaSlicer) 2.9.7 (`version_2.9.7+boss`,
BOSS's stable line) in a web page. Slicing runs on the server with BOSS's own slicing
library; the 3D view and the G-code preview are drawn in your browser. See
[docs/DESIGN.md](docs/DESIGN.md).

This is the 2.9.7 sibling of [Prusaslicer - Web Boss](https://github.com/The-Original-Reth/Prusaslicer-Web-Boss),
which follows BOSS's 3.0 nightlies. The two are separate apps.

## Run it

```bash
docker compose up -d --build
```

Then open http://localhost:8960. The first build compiles BOSS's dependencies and slicing
library, which takes a while (an hour or more); later builds reuse them.

## On a server

Run it from a checkout of this repo, e.g. in `/opt/prusaslicer-web-boss-297`, and update it
with:

```bash
cd /opt/prusaslicer-web-boss-297 && sudo sh update.sh
```

It pulls the repo, rebuilds what changed (BOSS itself only when the pinned `SLICER_COMMIT`
in the Dockerfile moves) and restarts the app. Data is kept in `./data`.

## What works (milestones 2 and 3)

- Add STL, 3MF, OBJ and AMF models (button or drag and drop), move, rotate and scale them
  on the bed, arrange, delete.
- Choose any printer bundled with BOSS, with the print and filament profiles that fit it.
- Supports, infill and brim from the sidebar, like BOSS's plater.
- Print, Filament and Printer Settings tabs with every setting, laid out as in BOSS (pages,
  groups, Simple / Advanced / Expert modes, greyed-out options, search, help links).
  Changes are marked and can be undone per setting or per tab; switching presets asks
  whether to transfer or discard them.
- Per-object settings: select an object to override print settings for it alone (BOSS's
  quick groups, or any setting an object can have).
- Parts, modifiers, negative volumes (holes), support enforcers and blockers: box,
  cylinder, sphere or slab, added from the object's panel, moved / rotated / scaled with
  the gizmos, with their own settings for parts and modifiers.
- Slice, see used filament and print time, preview the toolpaths by feature and layer, and
  download the G-code. Re-slicing only redoes the steps a change affects.
- G-code thumbnails for printers that show them (the sizes and formats in the printer's
  "G-code thumbnails" setting), drawn by the page like BOSS draws them.

- Save your changes as presets of your own (Save… in each settings tab), delete them, and
  find saved printers under "My printers". They're kept in `data/presets/`, one folder per
  vendor, in BOSS's own .ini format, and are shared by everyone using the server.

## Pieces

| Folder | What | Runs as |
|---|---|---|
| `engine/` | C++ program built inside BOSS's source tree; one per open project | JSON lines on stdin/stdout |
| `gateway/` | Python (FastAPI) server: web page, uploads, websocket to each project's engine | port 8080 in the container |
| `web/` | React + three.js page | served by the gateway |

Engine alone, for tests: `docker build --target engine -t prusaslicer-web-engine .`, then
pipe commands into `docker run -i --rm prusaslicer-web-engine` (examples in `engine/test/`:
`plate.jsonl` for the plate and slicing, `settings.jsonl` for editing presets, and
`presets.jsonl` for saving them, which needs `-e USER_PRESETS_DIR=/some/folder`, and
`objects.jsonl` for per-object settings, `volumes.jsonl` for modifiers and holes, and
`boss297.jsonl` for some of BOSS 2.9.7's own features: Flowsnake and CrossHatch infill,
Nip/Tuck seams, aligned rear seams, small-area flow compensation).

Developing the page: run the container, then `npm run dev` in `web/` (it proxies `/api` to
port 8080, so publish that port, e.g. `docker run -p 8080:8080 prusaslicer-web-boss-297`).
