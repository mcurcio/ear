// When the canvas fits its view by itself (docs/editor.md#the-canvas): once the boxes are measured, and again each
// time the nodes on it or the canvas's size change, until the person moves the view. Fitting once on React Flow's
// first measurement fitted boxes not yet laid out, and clipped the graph; a fit that follows the graph never does.
// A pan, a zoom or a jump to a node holds it; Fit (the button, F, the palette) lets it go again.

/** The view's key: the canvas's size and the nodes on it. A fit is due when it changes. */
export function fitKey(width: number, height: number, nodes: readonly string[]): string {
  return `${Math.round(width)}x${Math.round(height)}|${nodes.join("\n")}`;
}

export class AutoFit {
  private held = false;
  private last: string | null = null;

  /** The person moved the view (a pan, a zoom, a jump to a node): stop fitting by itself. */
  hold(): void {
    this.held = true;
  }

  /** The person asked for a fit: fit by itself again from here on. */
  release(): void {
    this.held = false;
  }

  /** True when the view should be fitted now for this key: not held, and the key is new. */
  due(key: string): boolean {
    if (this.held || key === this.last) return false;
    this.last = key;
    return true;
  }
}
