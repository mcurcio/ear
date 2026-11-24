// The graph editor's shell (docs/editor.md#the-shell): the top bar, the canvas with the inspector on its right, and the
// full-width ear.conf drawer along the bottom; below the narrow width the inspector is a sheet over the canvas. Nodes
// and wires are exactly what ear announced; dragging a wire between ports, the inspector, the drawer's Save and the
// delete key send requests, and the canvas changes when ear's events arrive. Node positions, the selection, the sizes
// and the theme (docs/editor.md#tokens-and-themes) are the browser's own.
import type { Connection } from "@xyflow/react";
import { useCallback, useEffect, useState } from "react";
import { routeFromConnection } from "./connect";
import { RpcError } from "./rpc";
import { removals, selectionKey, subjectOf } from "./selection";
import { inspectorWidth, narrowQuery } from "./sizes";
import { applyTheme, defaultTheme, type ThemeChoice } from "./theme";
import type { RouteEnds } from "./protocol";
import { Canvas } from "./components/Canvas";
import { Drawer } from "./components/Drawer";
import { useEditor, useGraph, useGraphFile, useMediaQuery } from "./components/hooks";
import { Inspector } from "./components/Inspector";
import { TopBar } from "./components/TopBar";
import { useCanvas } from "./components/useCanvas";
import { useDrawer } from "./components/useDrawer";

function describe(error: unknown): string {
  if (error instanceof RpcError)
    return error.data?.code !== undefined ? `${error.data.code}: ${error.message}` : error.message;
  return error instanceof Error ? error.message : String(error);
}

export function App() {
  const { client } = useEditor();
  const graph = useGraph();
  const fileState = useGraphFile();
  const [error, setError] = useState<string | null>(null);
  const showError = useCallback((caught: unknown) => setError(describe(caught)), []);
  const dismissError = useCallback(() => setError(null), []);
  const [theme, setTheme] = useState<ThemeChoice>(defaultTheme);
  useEffect(() => applyTheme(document.documentElement, theme), [theme]);

  const canvas = useCanvas(graph);
  const drawer = useDrawer(fileState, showError);
  const [inspectorPx, setInspectorPx] = useState<number>(inspectorWidth.initial);
  const [minimap, setMinimap] = useState(false);
  const toggleMinimap = useCallback(() => setMinimap((shown) => !shown), []);

  // Below the narrow width the inspector is a sheet: it opens on a selection and closes on Escape or the canvas.
  const narrow = useMediaQuery(narrowQuery);
  const [sheetOpen, setSheetOpen] = useState(false);
  const key = selectionKey(canvas.selection);
  useEffect(() => {
    if (key !== "") setSheetOpen(true);
  }, [key]);
  useEffect(() => {
    if (!narrow || !sheetOpen) return undefined;
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) setSheetOpen(false);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [narrow, sheetOpen]);
  const closeSheet = useCallback(() => setSheetOpen(false), []);

  const onConnect = useCallback(
    (connection: Connection) => {
      const route = routeFromConnection(connection);
      if (route !== null) client.call("route.add", route).catch(showError);
    },
    [client, showError],
  );

  // Delete asks ear; removing a node removes its routes, so only routes between kept nodes are asked for.
  const onRemove = useCallback(
    (nodes: readonly string[], routes: readonly RouteEnds[]) => {
      const asked = removals(nodes, routes);
      for (const name of asked.nodes) client.call("node.remove", { name }).catch(showError);
      for (const ends of asked.routes) client.call("route.remove", { from: ends.from, to: ends.to }).catch(showError);
    },
    [client, showError],
  );

  const drawerToggle =
    drawer === null ? null : { open: drawer.open, status: drawer.status, onToggle: () => drawer.setOpen(!drawer.open) };
  const subject = graph === null ? null : subjectOf(graph, canvas.selection);

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
      />
      <div className="workspace">
        <Canvas
          canvas={canvas}
          onConnect={onConnect}
          onRemove={onRemove}
          onPaneClick={closeSheet}
          minimap={minimap}
          onToggleMinimap={toggleMinimap}
          error={error}
          onDismissError={dismissError}
        />
        {graph !== null && subject !== null && (
          <Inspector
            graph={graph}
            subject={subject}
            width={inspectorPx}
            onResize={setInspectorPx}
            onSelect={canvas.select}
            onRemove={onRemove}
            onError={showError}
          />
        )}
      </div>
      {drawer !== null && <Drawer drawer={drawer} />}
    </div>
  );
}
