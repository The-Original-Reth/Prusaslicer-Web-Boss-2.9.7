// The window, laid out like BOSS 2.9.7's (its "new" layout, dark theme): a menu bar, a top
// bar with Settings, search and the mode, then the plater (3D scene with floating
// toolbars, sidebar on the right) or the settings tabs.

import { useEffect, useRef, useState } from "react";
import { PreviewOverlay } from "./PreviewOverlay";
import { Scene } from "./Scene";
import { MODES, SettingsView, UnsavedChangesDialog } from "./Settings";
import { Sidebar } from "./Sidebar";
import { deleteSelected, dismiss, fileUrl, run, select, setMode, slice, uploadModels, useStore, type Gizmo, type View } from "./store";
import type { PresetType } from "./types";

const MODEL_TYPES = ".stl,.3mf,.obj,.amf";
const SETTINGS_TABS: { view: PresetType; label: string }[] = [
  { view: "print", label: "Print Settings" },
  { view: "filament", label: "Filament Settings" },
  { view: "printer", label: "Printer Settings" },
];

const isSettings = (v: View): v is PresetType => v === "print" || v === "filament" || v === "printer";

// Switching to Preview slices first when the plate changed, like BOSS
export function showView(view: View) {
  const s = useStore.getState();
  useStore.setState(isSettings(view) ? { view, lastSettings: view } : { view });
  if (view === "preview" && s.stale && !s.busy && (s.plate?.objects.length ?? 0) > 0) void slice();
}

// Back to the plater, in whichever of its views was last used
let lastPlater: View = "3d";
function showPlater() {
  showView(lastPlater);
}

// One of BOSS's own icons (resources/icons), drawn in the current text color like BOSS's
// dark-mode toolbars
export function Icon({ name, className = "" }: { name: string; className?: string }) {
  return <span className={`boss-icon ${className}`} style={{ ["--icon" as string]: `url(/api/resources/icons/${name}.svg)` }} />;
}

let openFileDialog: () => void = () => {};

function exportGcode() {
  const s = useStore.getState();
  if (!s.slice || s.stale) return;
  const a = document.createElement("a");
  a.href = fileUrl(s.slice.gcode, `?download=${encodeURIComponent(s.slice.output_name)}`);
  a.click();
}

export function App() {
  const view = useStore((s) => s.view);
  const collapsed = useStore((s) => s.sidebarCollapsed);
  const [dragging, setDragging] = useState(false);
  const settingsTab = isSettings(view);
  if (!settingsTab) lastPlater = view;

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const s = useStore.getState();
      const ctrl = e.ctrlKey || e.metaKey;
      // BOSS's window shortcuts work everywhere
      const tabs: Record<string, View> = { "1": lastPlater, "2": "print", "3": "filament", "4": "printer", "5": "3d", "6": "preview" };
      if (ctrl && tabs[e.key]) {
        showView(tabs[e.key]);
        e.preventDefault();
        return;
      }
      if (ctrl && e.key.toLowerCase() === "i") {
        openFileDialog();
        e.preventDefault();
        return;
      }
      if (ctrl && e.key.toLowerCase() === "g") {
        exportGcode();
        e.preventDefault();
        return;
      }
      if (e.target instanceof Element && e.target.closest("input, select, textarea, .modal")) return;
      const gizmos: Record<string, Gizmo> = { m: "move", r: "rotate", s: "scale" };
      const plater = s.view === "3d" || s.view === "preview";
      if (plater && e.key === "Tab" && e.shiftKey) useStore.setState({ sidebarCollapsed: !s.sidebarCollapsed });
      else if (plater && e.key === "Tab") showView(s.view === "3d" ? "preview" : "3d");
      else if (!plater) return;
      else if (s.view === "3d" && ctrl && e.key === "Delete") void run("clear");
      else if (s.view === "3d" && (e.key === "Delete" || e.key === "Backspace")) void deleteSelected();
      else if (s.view === "3d" && e.key.toLowerCase() === "a" && !ctrl) void run("arrange");
      else if (s.view === "3d" && gizmos[e.key.toLowerCase()] && !ctrl) useStore.setState({ gizmo: gizmos[e.key.toLowerCase()] });
      else if (e.key === "Escape") select(null);
      else return;
      e.preventDefault();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  return (
    <div className={`app${settingsTab ? " in-settings" : ""}${collapsed ? " collapsed" : ""}`}>
      <MenuBar />
      <TopBar />
      {settingsTab && <SettingsTabs />}
      {settingsTab && <SettingsView key={view} type={view} />}
      <main
        // The 3D scene stays alive behind the settings tabs, so coming back is instant
        hidden={settingsTab}
        className={`stage${dragging ? " dropping" : ""}`}
        onDragOver={(e) => {
          if (!e.dataTransfer.types.includes("Files")) return;
          e.preventDefault();
          setDragging(true);
        }}
        onDragLeave={(e) => e.currentTarget === e.target && setDragging(false)}
        onDrop={(e) => {
          e.preventDefault();
          setDragging(false);
          if (e.dataTransfer.files.length) {
            useStore.setState({ view: "3d" });
            void uploadModels(e.dataTransfer.files);
          }
        }}
      >
        <Scene />
        {view === "3d" && <PlaterToolbars />}
        {view === "preview" && <PreviewOverlay />}
        <ViewToolbar />
        {!settingsTab && <Notices />}
        {dragging && <div className="drop-hint">Drop models to add them to the plate</div>}
      </main>
      {!settingsTab && !collapsed && <Sidebar />}
      {settingsTab && <Notices />}
      <FileInput />
      <UnsavedChangesDialog />
    </div>
  );
}

// The hidden file chooser behind Import and the + button
function FileInput() {
  const file = useRef<HTMLInputElement>(null);
  useEffect(() => {
    openFileDialog = () => file.current?.click();
  }, []);
  return (
    <input
      ref={file}
      type="file"
      multiple
      accept={MODEL_TYPES}
      hidden
      onChange={(e) => {
        if (e.target.files?.length) {
          useStore.setState({ view: "3d" });
          void uploadModels(e.target.files);
        }
        e.target.value = "";
      }}
    />
  );
}

// ---- menu bar ----

interface MenuItem {
  label: string;
  shortcut?: string;
  action?: () => void;
  disabled?: boolean;
  checked?: boolean;
  separator?: boolean;
}

function MenuBar() {
  const [open, setOpen] = useState<string | null>(null);
  const hasObjects = useStore((s) => (s.plate?.objects.length ?? 0) > 0);
  const hasSelection = useStore((s) => s.selected !== null);
  const canExport = useStore((s) => !!s.slice && !s.stale);
  const mode = useStore((s) => s.mode);
  const collapsed = useStore((s) => s.sidebarCollapsed);
  const ref = useRef<HTMLElement>(null);

  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent) => !ref.current?.contains(e.target as Node) && setOpen(null);
    window.addEventListener("mousedown", close);
    return () => window.removeEventListener("mousedown", close);
  }, [open]);

  const menus: Record<string, MenuItem[]> = {
    File: [
      { label: "Import STL/3MF/OBJ/AMF…", shortcut: "Ctrl+I", action: () => openFileDialog() },
      { label: "", separator: true },
      { label: "Export G-code…", shortcut: "Ctrl+G", action: exportGcode, disabled: !canExport },
    ],
    Edit: [
      { label: "Delete selected", shortcut: "Del", action: () => void deleteSelected(), disabled: !hasSelection },
      { label: "Delete all", shortcut: "Ctrl+Del", action: () => void run("clear"), disabled: !hasObjects },
      { label: "", separator: true },
      { label: "Arrange", shortcut: "A", action: () => void run("arrange"), disabled: !hasObjects },
    ],
    Window: [
      { label: "Plater", shortcut: "Ctrl+1", action: showPlater },
      { label: "", separator: true },
      { label: "Print Settings", shortcut: "Ctrl+2", action: () => showView("print") },
      { label: "Filament Settings", shortcut: "Ctrl+3", action: () => showView("filament") },
      { label: "Printer Settings", shortcut: "Ctrl+4", action: () => showView("printer") },
      { label: "", separator: true },
      { label: "3D", shortcut: "Ctrl+5", action: () => showView("3d") },
      { label: "Preview", shortcut: "Ctrl+6", action: () => showView("preview") },
    ],
    View: [
      { label: "Collapse sidebar", shortcut: "Shift+Tab", checked: collapsed, action: () => useStore.setState({ sidebarCollapsed: !collapsed }) },
    ],
    Configuration: MODES.map((m) => ({ label: `${m.label} mode`, checked: mode === m.mode, action: () => setMode(m.mode) })),
    Help: [
      { label: "PrusaSlicer BOSS on GitHub", action: () => window.open("https://github.com/mjonuschat/PrusaSlicer", "_blank") },
      { label: "Prusa Knowledge Base", action: () => window.open("https://help.prusa3d.com/", "_blank") },
    ],
  };

  return (
    <nav className="menubar" ref={ref}>
      {Object.entries(menus).map(([name, items]) => (
        <div key={name} className="menu">
          <button
            className={open === name ? "open" : ""}
            onClick={() => setOpen(open === name ? null : name)}
            onMouseEnter={() => open && setOpen(name)}
          >
            {name}
          </button>
          {open === name && (
            <ul className="menu-items">
              {items.map((item, i) =>
                item.separator ? (
                  <li key={i} className="menu-sep" />
                ) : (
                  <li key={i}>
                    <button
                      disabled={item.disabled}
                      onClick={() => {
                        setOpen(null);
                        item.action?.();
                      }}
                    >
                      <span className="check">{item.checked ? "✓" : ""}</span>
                      <span className="label">{item.label}</span>
                      <span className="shortcut">{item.shortcut}</span>
                    </button>
                  </li>
                ),
              )}
            </ul>
          )}
        </div>
      ))}
    </nav>
  );
}

// ---- top bar ----

function TopBar() {
  const view = useStore((s) => s.view);
  const search = useStore((s) => s.search);
  const mode = useStore((s) => s.mode);
  const connected = useStore((s) => s.connected);
  const [modeOpen, setModeOpen] = useState(false);
  const current = MODES.find((m) => m.mode === mode) ?? MODES[2];
  return (
    <header className="topbar">
      <button
        className={`topbar-button${isSettings(view) ? " active" : ""}`}
        onClick={() => (isSettings(view) ? showPlater() : showView(useStore.getState().lastSettings))}
        title={isSettings(view) ? "Back to the plater" : "Print, filament and printer settings"}
      >
        Settings
      </button>
      <label className="topbar-search">
        <input
          placeholder="Enter a search term"
          value={search}
          onChange={(e) => {
            useStore.setState({ search: e.target.value });
            // Like BOSS, a search shows the settings it finds
            if (e.target.value && !isSettings(useStore.getState().view)) showView(useStore.getState().lastSettings);
          }}
          onKeyDown={(e) => e.key === "Escape" && useStore.setState({ search: "" })}
        />
        <Icon name="search" />
      </label>
      <div className="topbar-right">
        <div className="mode-menu">
          <button className={`topbar-button mode-${mode}`} onClick={() => setModeOpen(!modeOpen)}>
            <span className="mode-dot" />
            {current.label} mode
            <span className="chevron">⌵</span>
          </button>
          {modeOpen && (
            <ul className="menu-items right" onMouseLeave={() => setModeOpen(false)}>
              {MODES.map((m) => (
                <li key={m.mode}>
                  <button
                    className={`mode-${m.mode}`}
                    onClick={() => {
                      setMode(m.mode);
                      setModeOpen(false);
                    }}
                  >
                    <span className="check"><span className="mode-dot" /></span>
                    <span className="label">{m.label}</span>
                    <span className="shortcut">{mode === m.mode ? "✓" : ""}</span>
                  </button>
                </li>
              ))}
            </ul>
          )}
        </div>
        <span className={`connection${connected ? " ok" : ""}`} title={connected ? "Connected to the server" : "Connecting to the server…"}>
          <Icon name="user" />
          {connected ? "Connected" : "Connecting…"}
        </span>
      </div>
    </header>
  );
}

// The settings tabs' own strip (Plater goes back to the 3D scene)
function SettingsTabs() {
  const view = useStore((s) => s.view);
  return (
    <nav className="settings-tabs">
      <button onClick={showPlater}>
        <Icon name="editor" /> Plater
      </button>
      {SETTINGS_TABS.map((t) => (
        <button key={t.view} className={view === t.view ? "active" : ""} onClick={() => showView(t.view)}>
          <Icon name={t.view === "print" ? "cog" : t.view === "filament" ? "spool" : "printer"} /> {t.label}
        </button>
      ))}
    </nav>
  );
}

// ---- the plater's floating toolbars ----

const NOT_YET = "not available in Web Boss yet";

function ToolButton({ icon, title, onClick, disabled, active }: {
  icon: string;
  title: string;
  onClick?: () => void;
  disabled?: boolean;
  active?: boolean;
}) {
  return (
    <button className={active ? "active" : ""} title={title} disabled={disabled || !onClick} onClick={onClick}>
      <Icon name={icon} />
    </button>
  );
}

function PlaterToolbars() {
  const gizmo = useStore((s) => s.gizmo);
  const hasSelection = useStore((s) => s.selected !== null);
  const hasObjects = useStore((s) => (s.plate?.objects.length ?? 0) > 0);
  const busy = useStore((s) => !!s.busy);
  const collapsed = useStore((s) => s.sidebarCollapsed);
  const tool = (g: Gizmo, label: string, key: string, icon: string) => (
    <ToolButton icon={icon} title={`${label} [${key}]`} active={gizmo === g} onClick={() => useStore.setState({ gizmo: g })} />
  );
  return (
    <>
      <div className="toolbar top">
        <ToolButton icon="add" title="Add... [Ctrl+I]" disabled={busy} onClick={() => openFileDialog()} />
        <ToolButton icon="delete" title="Delete [Del]" disabled={!hasSelection} onClick={() => void deleteSelected()} />
        <ToolButton icon="delete_all" title="Delete all [Ctrl+Del]" disabled={!hasObjects} onClick={() => void run("clear")} />
        <ToolButton icon="arrange" title="Arrange [A]" disabled={!hasObjects} onClick={() => void run("arrange")} />
        <ToolButton icon="arrange_current" title={`Arrange selection [Shift+A] (${NOT_YET})`} />
        <span className="sep" />
        <ToolButton icon="copy" title={`Copy [Ctrl+C] (${NOT_YET})`} />
        <ToolButton icon="paste" title={`Paste [Ctrl+V] (${NOT_YET})`} />
        <span className="sep" />
        <ToolButton icon="instance_add" title={`Add instance [+] (${NOT_YET})`} />
        <ToolButton icon="instance_remove" title={`Remove instance [-] (${NOT_YET})`} />
        <span className="sep" />
        <ToolButton icon="split_objects" title={`Split to objects (${NOT_YET})`} />
        <ToolButton icon="split_parts" title={`Split to parts (${NOT_YET})`} />
        <span className="sep" />
        <ToolButton icon="layers" title={`Variable layer height (${NOT_YET})`} />
        <span className="sep" />
        <ToolButton icon="undo_toolbar" title={`Undo [Ctrl+Z] (${NOT_YET})`} />
        <ToolButton icon="redo_toolbar" title={`Redo [Ctrl+Y] (${NOT_YET})`} />
      </div>
      <CollapseButton collapsed={collapsed} />
      <div className="toolbar left">
        {tool("move", "Move", "M", "move")}
        {tool("scale", "Scale", "S", "scale")}
        {tool("rotate", "Rotate", "R", "rotate")}
        <ToolButton icon="place" title={`Place on face [F] (${NOT_YET})`} />
        <ToolButton icon="cut" title={`Cut [C] (${NOT_YET})`} />
        <ToolButton icon="fdm_supports" title={`Paint-on supports [L] (${NOT_YET})`} />
        <ToolButton icon="seam" title={`Seam painting [P] (${NOT_YET})`} />
        <ToolButton icon="fuzzy_skin_painting" title={`Fuzzy skin painting [H] (${NOT_YET})`} />
        <ToolButton icon="mmu_segmentation" title={`Multimaterial painting [N] (${NOT_YET})`} />
        <ToolButton icon="measure" title={`Measure [U] (${NOT_YET})`} />
      </div>
    </>
  );
}

function CollapseButton({ collapsed }: { collapsed: boolean }) {
  return (
    <div className="toolbar collapse">
      <button
        title={collapsed ? "Expand sidebar [Shift+Tab]" : "Collapse sidebar [Shift+Tab]"}
        onClick={() => useStore.setState({ sidebarCollapsed: !collapsed })}
      >
        <Icon name="collapse" />
      </button>
    </div>
  );
}

// Bottom left: BOSS's 3D / Preview switch
function ViewToolbar() {
  const view = useStore((s) => s.view);
  return (
    <div className="toolbar views">
      <ToolButton icon="editor" title="3D editor view [Tab]" active={view === "3d"} onClick={() => showView("3d")} />
      <ToolButton icon="preview" title="Preview [Tab]" active={view === "preview"} onClick={() => showView("preview")} />
    </div>
  );
}

function Notices() {
  const notices = useStore((s) => s.notices);
  return (
    <div className="notices">
      {notices.map((n) => (
        <div key={n.id} className={`notice ${n.kind}`}>
          <span>{n.text}</span>
          <button className="notice-close" title="Close" onClick={() => dismiss(n.id)}>×</button>
        </div>
      ))}
    </div>
  );
}
