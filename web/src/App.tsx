// The graph editor's shell (docs/editor.md#the-shell): the top bar, the canvas with the inspector on its right, and the
// full-width ear.conf drawer along the bottom; below the narrow width the inspector is a sheet over the canvas. Nodes
// and wires are exactly what ear announced; dragging a wire between ports, the inspector, the drawer's Save, the
// delete key and the command palette's commands send requests (through the session's ask, so each shows while on its
// way and a refusal becomes a toast, docs/editor.md#feedback), and the canvas changes when ear's events arrive. The
// shortcuts and the palette (docs/editor.md#the-palette-and-the-keyboard) and quick add on the canvas
// (docs/editor.md#adding-on-the-canvas) are held here. Node places, the selection, the sizes and the theme are the
// browser's own; places, sizes, the minimap and the theme outlive the page (docs/editor.md#what-the-browser-keeps).
import { CommandPalette, ShortcutSheet } from "@mcurcio/patchbay/composites";
import {
  detectPlatform,
  keyLabel,
  useMediaBreakpoint,
  usePersistentState,
  useShortcuts,
  useTheme,
} from "@mcurcio/patchbay/hooks";
import { useReactFlow, type Connection } from "@xyflow/react";
import { useCallback, useEffect, useMemo, useState } from "react";
import { freeName } from "./addText";
import { routeFromConnection } from "./connect";
import { shortcutKeys, shortcutLines, shortcutTable, type ShortcutAction } from "./keys";
import type { PlaceHint, Position } from "./layout";
import { paletteItems, type PaletteScope } from "./palette";
import { checkFlag, checkWidth, themeKey, type Prefs } from "./prefs";
import { nodeTextFeature, type RouteEnds } from "./protocol";
import { nothingSelected, removals, selectionKey, subjectOf } from "./selection";
import { inspectorWidth } from "./sizes";
import { Canvas } from "./components/Canvas";
import { fitPadding } from "./components/CanvasTools";
import { AutoFit } from "./fit";
import { Drawer } from "./components/Drawer";
import { useEditor, useGraph, useGraphFile } from "./components/hooks";
import { Inspector, type AddIntent } from "./components/Inspector";
import { TopBar } from "./components/TopBar";
import { useCanvas } from "./components/useCanvas";
import { useCommands } from "./components/useCommands";
import { useDrawer } from "./components/useDrawer";

/** The open palette: all commands or only Add; for quick add, the screen point it opens at and the node's place. */
interface PaletteOpen {
  scope: PaletteScope;
  at?: { x: number; y: number };
  place?: Position;
}

export function App({ prefs }: { prefs: Prefs }) {
  const editor = useEditor();
  const graph = useGraph();
  const fileState = useGraphFile();
  const flow = useReactFlow();
  const [platform] = useState(detectPlatform);

  // The theme is patchbay's useTheme under ear's own key (docs/editor.md#tokens-and-themes); index.html's inline
  // script applies a kept choice before the first paint.
  const [theme, setTheme] = useTheme({ key: themeKey() });

  const [keeper] = useState(() => prefs.placeKeeper());
  const canvas = useCanvas(graph, keeper);
  const drawer = useDrawer(fileState, prefs);
  // Kept per ear by patchbay's usePersistentState, under prefs.ts's keys and checks (docs/editor.md#what-the-browser-keeps).
  const kept = { storage: prefs.storage };
  const [inspectorPx, setInspectorPx] = usePersistentState(
    prefs.key("inspector-width"),
    inspectorWidth.initial,
    checkWidth,
    kept,
  );
  const [minimap, setMinimap] = usePersistentState(prefs.key("minimap"), false, checkFlag, kept);
  const toggleMinimap = useCallback(() => setMinimap((shown) => !shown), []);

  // Below the narrow width the inspector is a sheet: it opens on a selection and closes on Escape, its close button or
  // the canvas; below the phone width it rises from the bottom.
  const { width } = useMediaBreakpoint();
  const narrow = width !== "wide";
  const phone = width === "phone";
  const [sheetOpen, setSheetOpen] = useState(false);
  const key = selectionKey(canvas.selection);
  useEffect(() => {
    if (key !== "") setSheetOpen(true);
  }, [key]);
  const closeSheet = useCallback(() => setSheetOpen(false), []);

  const [palette, setPalette] = useState<PaletteOpen | null>(null);
  const [shortcutSheet, setShortcutSheet] = useState(false);
  const [addIntent, setAddIntent] = useState<AddIntent | null>(null);

  const onConnect = useCallback(
    (connection: Connection) => {
      const route = routeFromConnection(connection);
      if (route !== null) editor.send("route.add", route);
    },
    [editor],
  );

  // Delete asks ear; removing a node removes its routes, so only routes between kept nodes are asked for.
  const onRemove = useCallback(
    (nodes: readonly string[], routes: readonly RouteEnds[]) => {
      const asked = removals(nodes, routes);
      for (const name of asked.nodes) editor.send("node.remove", { name });
      for (const ends of asked.routes) editor.send("route.remove", { from: ends.from, to: ends.to });
    },
    [editor],
  );

  const drawerToggle =
    drawer === null ? null : { open: drawer.open, status: drawer.status, onToggle: () => drawer.setOpen(!drawer.open) };
  const subject = graph === null ? null : subjectOf(graph, canvas.selection);

  // The add form, and its half-typed statement, go with the Graph subject (docs/editor.md#the-inspector), so its
  // intent and quick add's hint go too.
  const onGraph = subject?.kind === "graph";
  useEffect(() => {
    if (onGraph) return;
    setAddIntent(null);
    canvas.hint(null);
  }, [onGraph, canvas.hint]);

  // The view fits by itself until the person moves it (fit.ts); Fit lets it go again, a jump to a node holds it.
  const [autoFit] = useState(() => new AutoFit());
  const fit = useCallback(() => {
    autoFit.release();
    void flow.fitView({ padding: fitPadding });
  }, [autoFit, flow]);
  // As a toast's Show does.
  const goTo = useCallback(
    (name: string) => {
      canvas.select({ nodes: [name], routes: [] });
      autoFit.hold();
      void flow.fitView({ nodes: [{ id: name }], padding: 0.4, duration: 180 });
    },
    [autoFit, canvas.select, flow],
  );

  // An Add command: the Graph subject's form with that type's template; quick add's place is kept as a hint for the
  // template's name (with no template, the JSON form, for the next node that arrives).
  const openAdd = useCallback(
    (type: string, place?: Position) => {
      canvas.select(nothingSelected);
      setAddIntent((previous) => ({ type, nonce: (previous?.nonce ?? 0) + 1 }));
      if (narrow) setSheetOpen(true);
      let hint: PlaceHint | null = null;
      if (place !== undefined && graph !== null) {
        const knobly = (graph.features ?? []).includes(nodeTextFeature) && fileState !== null;
        const names = graph.nodes.map((node) => node.name);
        hint = { name: knobly ? freeName(type, names) : null, position: place };
      }
      canvas.hint(hint);
    },
    [canvas.select, canvas.hint, narrow, graph, fileState],
  );

  const { commands, run } = useCommands(
    {
      graph,
      selection: canvas.selection,
      drawer,
      minimap,
      theme,
      goTo,
      onAdd: openAdd,
      onRemove,
      fit,
      tidy: canvas.tidy,
      toggleMinimap,
      setTheme,
      onShortcuts: () => setShortcutSheet(true),
    },
    palette?.scope ?? "all",
  );
  // Quick add (docs/editor.md#adding-on-the-canvas): the Add commands at the pointer, with where the node should land.
  const onQuickAdd = useCallback(
    (at: { x: number; y: number }, place: Position) => setPalette({ scope: "add", at, place }),
    [],
  );
  const place = palette?.place;
  const items = useMemo(() => paletteItems(commands, (action) => run(action, place)), [commands, run, place]);
  const closePalette = useCallback(() => setPalette(null), []);

  // The shortcuts (docs/editor.md#shortcuts), on patchbay's useShortcuts. While the palette is open only its toggle
  // acts (the palette handles its own Escape and Tab); while the shortcut sheet is open none do, since its Dialog owns
  // the keys and the focus.
  const onShortcut = (action: ShortcutAction) => {
    switch (action) {
      case "palette":
        return setPalette((open) => (open !== null ? null : { scope: "all" }));
      case "add":
        // Centred with no place, so the node is laid out by dagre.
        return setPalette({ scope: "add" });
      case "drawer":
        return drawer?.setOpen(!drawer.open);
      case "save":
        // Prevented either way, so the browser's Save page never opens.
        if (drawer?.canSave) drawer.save();
        return;
      case "fit":
        return fit();
      case "tidy":
        return canvas.tidy();
      case "minimap":
        return toggleMinimap();
      case "shortcuts":
        return setShortcutSheet(true);
      case "escape":
        if (narrow && sheetOpen) return setSheetOpen(false);
        if (key !== "") canvas.select(nothingSelected);
        return;
    }
  };
  useShortcuts(shortcutTable(onShortcut, { palette: palette !== null }), { enabled: !shortcutSheet, platform });

  return (
    <div className="app">
      <h1 className="visually-hidden">EAR graph editor</h1>
      <TopBar
        graph={graph}
        theme={theme}
        onTheme={setTheme}
        drawer={drawerToggle}
        narrow={narrow}
        sheetOpen={sheetOpen}
        onToggleSheet={() => setSheetOpen((open) => !open)}
        onPalette={() => setPalette({ scope: "all" })}
        paletteLabel={keyLabel(shortcutKeys.palette, { platform })}
      />
      <div className="workspace">
        <Canvas
          canvas={canvas}
          onConnect={onConnect}
          onRemove={onRemove}
          onPaneClick={closeSheet}
          minimap={minimap}
          onToggleMinimap={toggleMinimap}
          onShow={canvas.select}
          onQuickAdd={onQuickAdd}
          autoFit={autoFit}
          onFit={fit}
        />
        {graph !== null && subject !== null && (
          <Inspector
            graph={graph}
            subject={subject}
            addIntent={addIntent}
            width={inspectorPx}
            onResize={setInspectorPx}
            onSelect={canvas.select}
            onRemove={onRemove}
            sheet={narrow ? { side: phone ? "bottom" : "right", open: sheetOpen, onClose: closeSheet } : null}
          />
        )}
      </div>
      {drawer !== null && <Drawer drawer={drawer} />}
      <CommandPalette
        open={palette !== null}
        onClose={closePalette}
        commands={items}
        at={palette?.at}
        labels={{ placeholder: palette?.scope === "add" ? "Add a node of type…" : "Type a command or a node's name" }}
        platform={platform}
      />
      <ShortcutSheet
        open={shortcutSheet}
        onClose={() => setShortcutSheet(false)}
        shortcuts={shortcutLines}
        platform={platform}
      >
        Single keys act only when the focus is not in a text field.
      </ShortcutSheet>
    </div>
  );
}
