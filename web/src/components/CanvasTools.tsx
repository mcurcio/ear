// The canvas's floating tool group (docs/editor.md#the-canvas), in place of React Flow's Controls: zoom out, zoom in,
// fit, tidy (every node laid out again, nothing sent) and the minimap toggle. Each glyph is hidden from screen readers
// beside its button's label. Fit, tidy and the minimap are also F, L and M (docs/editor.md#shortcuts), which their
// titles say.
import { Panel, useReactFlow } from "@xyflow/react";

/** Fit's padding, the same for the button, F and the palette's Fit view. */
export const fitPadding = 0.1;

export function CanvasTools({
  minimap,
  onToggleMinimap,
  onTidy,
  onFit,
}: {
  minimap: boolean;
  onToggleMinimap: () => void;
  onTidy: () => void;
  /** Fit, as F and the palette's Fit view (App's, which lets the view fit by itself again). */
  onFit: () => void;
}) {
  const { zoomIn, zoomOut } = useReactFlow();
  return (
    <Panel position="bottom-left" className="canvas-tools">
      <div role="toolbar" aria-label="Canvas" aria-orientation="horizontal">
        <button type="button" aria-label="Zoom out" title="Zoom out" onClick={() => void zoomOut()}>
          <span aria-hidden="true">−</span>
        </button>
        <button type="button" aria-label="Zoom in" title="Zoom in" onClick={() => void zoomIn()}>
          <span aria-hidden="true">+</span>
        </button>
        <button
          type="button"
          aria-label="Fit view"
          title="Fit view (F)"
          aria-keyshortcuts="f"
          onClick={onFit}
        >
          <span aria-hidden="true">⤢</span>
        </button>
        <button type="button" aria-label="Tidy layout" title="Tidy layout (L)" aria-keyshortcuts="l" onClick={onTidy}>
          <span aria-hidden="true">⊞</span>
        </button>
        <button
          type="button"
          aria-label="Minimap"
          title="Minimap (M)"
          aria-keyshortcuts="m"
          aria-pressed={minimap}
          onClick={onToggleMinimap}
        >
          <span aria-hidden="true">▦</span>
        </button>
      </div>
    </Panel>
  );
}
