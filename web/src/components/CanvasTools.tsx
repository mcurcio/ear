// The canvas's floating tool group (docs/editor.md#the-canvas), in place of React Flow's Controls: zoom out, zoom in,
// fit, tidy (every node laid out again, nothing sent) and the minimap toggle. Each glyph is hidden from screen readers
// beside its button's label.
import { Panel, useReactFlow } from "@xyflow/react";

export function CanvasTools({
  minimap,
  onToggleMinimap,
  onTidy,
}: {
  minimap: boolean;
  onToggleMinimap: () => void;
  onTidy: () => void;
}) {
  const { zoomIn, zoomOut, fitView } = useReactFlow();
  return (
    <Panel position="bottom-left" className="canvas-tools">
      <div role="toolbar" aria-label="Canvas" aria-orientation="horizontal">
        <button type="button" aria-label="Zoom out" title="Zoom out" onClick={() => void zoomOut()}>
          <span aria-hidden="true">−</span>
        </button>
        <button type="button" aria-label="Zoom in" title="Zoom in" onClick={() => void zoomIn()}>
          <span aria-hidden="true">+</span>
        </button>
        <button type="button" aria-label="Fit view" title="Fit view" onClick={() => void fitView({ padding: 0.1 })}>
          <span aria-hidden="true">⤢</span>
        </button>
        <button type="button" aria-label="Tidy layout" title="Tidy layout" onClick={onTidy}>
          <span aria-hidden="true">⊞</span>
        </button>
        <button type="button" aria-label="Minimap" title="Minimap" aria-pressed={minimap} onClick={onToggleMinimap}>
          <span aria-hidden="true">▦</span>
        </button>
      </div>
    </Panel>
  );
}
