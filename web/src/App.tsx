// The graph editor's shell (docs/editor.md#the-shell): the top bar, the canvas with the inspector on its right, and the
// full-width ear.conf drawer along the bottom; below the narrow width the inspector is a sheet over the canvas. Nodes
// and wires are exactly what ear announced; dragging a wire between ports, the inspector, the drawer's Save, the
// delete key and the command palette's commands send requests (through the session's ask, so each shows while on its
// way and a refusal becomes a toast, docs/editor.md#feedback), and the canvas changes when ear's events arrive. The
// shortcuts and the palette (docs/editor.md#the-palette-and-the-keyboard) and quick add on the canvas
// (docs/editor.md#adding-on-the-canvas) are held here. Node places, the selection, the sizes and the theme are the
// browser's own; places, sizes, the minimap and the theme outlive the page (docs/editor.md#what-the-browser-keeps).
import { useReactFlow, type Connection } from "@xyflow/react";
import { useCallback, useEffect, useState } from "react";
import { freeName } from "./addText";
import { routeFromConnection } from "./connect";
import { isMac, keyLabel, type ShortcutAction } from "./keys";
import type { PlaceHint, Position } from "./layout";
import type { Command, PaletteScope } from "./palette";
import type { Prefs } from "./prefs";
import { nodeTextFeature, type RouteEnds } from "./protocol";
import { nothingSelected, removals, selectionKey, subjectOf } from "./selection";
import { inspectorWidth, narrowQuery } from "./sizes";
import { applyTheme, defaultTheme, type ThemeChoice } from "./theme";
import { Canvas } from "./components/Canvas";
import { fitPadding } from "./components/CanvasTools";
import { Drawer } from "./components/Drawer";
import { useEditor, useGraph, useGraphFile, useMediaQuery } from "./components/hooks";
import { Inspector, type AddIntent } from "./components/Inspector";
import { Palette } from "./components/Palette";
import { ShortcutSheet } from "./components/ShortcutSheet";
import { TopBar } from "./components/TopBar";
import { useCanvas } from "./components/useCanvas";
import { useCommands } from "./components/useCommands";
import { useDrawer } from "./components/useDrawer";
import { useShortcuts } from "./components/useShortcuts";

/** The open palette: all commands or only Add; for quick add, the screen point it opens at and the node's place. */
interface PaletteOpen {
  scope: PaletteScope;
  at?: { x: number; y: number };
  place?: Position;
}

/** navigator.userAgentData is not in every browser, nor in TypeScript's DOM types. */
function platform(): string {
  const nav = navigator as Navigator & { userAgentData?: { platform?: string } };
  return nav.userAgentData?.platform ?? nav.platform ?? "";
}

export function App({ prefs }: { prefs: Prefs }) {
  const editor = useEditor();
  const graph = useGraph();
  const fileState = useGraphFile();
  const flow = useReactFlow();
  const [mac] = useState(() => isMac(platform()));

  const [theme, setTheme] = useState<ThemeChoice>(() => prefs.theme() ?? defaultTheme);
  useEffect(() => {
    applyTheme(document.documentElement, theme);
    prefs.setTheme(theme);
  }, [prefs, theme]);

  const [keeper] = useState(() => prefs.placeKeeper());
  const canvas = useCanvas(graph, keeper);
  const drawer = useDrawer(fileState, prefs);
  const [inspectorPx, setInspectorPx] = useState<number>(() => prefs.inspectorWidth() ?? inspectorWidth.initial);
  useEffect(() => prefs.setInspectorWidth(inspectorPx), [prefs, inspectorPx]);
  const [minimap, setMinimap] = useState(() => prefs.minimap() ?? false);
  useEffect(() => prefs.setMinimap(minimap), [prefs, minimap]);
  const toggleMinimap = useCallback(() => setMinimap((shown) => !shown), []);

  // Below the narrow width the inspector is a sheet: it opens on a selection and closes on Escape or the canvas.
  const narrow = useMediaQuery(narrowQuery);
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

  const fit = useCallback(() => void flow.fitView({ padding: fitPadding }), [flow]);
  // As a toast's Show does.
  const goTo = useCallback(
    (name: string) => {
      canvas.select({ nodes: [name], routes: [] });
      void flow.fitView({ nodes: [{ id: name }], padding: 0.4, duration: 180 });
    },
    [canvas.select, flow],
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
  const onRun = useCallback((command: Command) => run(command.action, palette?.place), [run, palette]);

  // The shortcuts (docs/editor.md#shortcuts). While the palette or the sheet is open only the palette's toggle and
  // Escape act; the palette handles its own Escape.
  useShortcuts(mac, (action: ShortcutAction) => {
    const modal = palette !== null || shortcutSheet;
    if (modal && action !== "palette" && action !== "escape") return;
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
        if (palette !== null) return setPalette(null);
        if (shortcutSheet) return setShortcutSheet(false);
        if (narrow && sheetOpen) return setSheetOpen(false);
        if (key !== "") canvas.select(nothingSelected);
        return;
    }
  });

  return (
    <div className="app" data-narrow={narrow || undefined} data-sheet={sheetOpen ? "open" : "closed"}>
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
        paletteLabel={keyLabel("palette", mac)}
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
          />
        )}
      </div>
      {drawer !== null && <Drawer drawer={drawer} />}
      {palette !== null && (
        <Palette
          commands={commands}
          at={palette.at}
          placeholder={palette.scope === "add" ? "Add a node of type…" : "Type a command or a node's name"}
          mac={mac}
          onRun={onRun}
          onClose={() => setPalette(null)}
        />
      )}
      {shortcutSheet && <ShortcutSheet mac={mac} onClose={() => setShortcutSheet(false)} />}
    </div>
  );
}
