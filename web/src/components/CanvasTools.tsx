// The canvas's floating tool group (docs/editor.md#the-canvas), in place of React Flow's Controls: zoom out, zoom in,
// fit, tidy (every node laid out again, nothing sent) and the minimap toggle, as patchbay's ToolGroup of IconButtons
// (one tab stop, the arrow keys between tools). Fit, tidy and the minimap are also F, L and M
// (docs/editor.md#shortcuts), which their titles say.
import { IconButton, ToolGroup } from "@mcurcio/patchbay/primitives";
import { Panel, useReactFlow } from "@xyflow/react";

/**
 * Fit's padding, the same for the view's own fit, the button, F and the palette's Fit view: a tenth of the canvas on
 * three sides, and at the bottom room for this tool group (at its tallest, on a touch screen) so it never covers a box.
 */
export const fitPadding = { top: "10%", right: "10%", bottom: "76px", left: "10%" } as const;

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
    <Panel position="bottom-left">
      <ToolGroup label="Canvas">
        <IconButton label="Zoom out" onClick={() => void zoomOut()}>
          −
        </IconButton>
        <IconButton label="Zoom in" onClick={() => void zoomIn()}>
          +
        </IconButton>
        <IconButton label="Fit view" title="Fit view (F)" aria-keyshortcuts="f" onClick={onFit}>
          ⤢
        </IconButton>
        <IconButton label="Tidy layout" title="Tidy layout (L)" aria-keyshortcuts="l" onClick={onTidy}>
          ⊞
        </IconButton>
        <IconButton
          label="Minimap"
          title="Minimap (M)"
          aria-keyshortcuts="m"
          aria-pressed={minimap}
          onClick={onToggleMinimap}
        >
          ▦
        </IconButton>
      </ToolGroup>
    </Panel>
  );
}
