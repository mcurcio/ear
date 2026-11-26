// The shell's sizes, pure (docs/editor.md#the-shell): the inspector's width and the drawer's height with their bounds,
// which patchbay's Splitter keeps a drag or a key within, and the widths below which the inspector is patchbay's Sheet
// over the canvas, from its right edge and then from its bottom. editor.css holds the same bounds and the phone
// width, which sizes.test.ts checks.

export interface Bounds {
  min: number;
  max: number;
}

/** The inspector's width: 320 px at first, dragged between 280 and 560 (editor.css's clamp holds the same). */
export const inspectorWidth = { initial: 320, min: 280, max: 560 } as const;
export const inspectorBounds: Bounds = { min: inspectorWidth.min, max: inspectorWidth.max };

/** The drawer's body: 280 px at first, dragged between 120 px and 70 % of the window. */
export const drawerHeight = { initial: 280, min: 120, maxShare: 0.7 } as const;

/** The drawer body's bounds in a window `windowHeight` px tall; the max never falls below the min. */
export function drawerBounds(windowHeight: number): Bounds {
  return { min: drawerHeight.min, max: Math.max(drawerHeight.min, Math.floor(drawerHeight.maxShare * windowHeight)) };
}

/** `value` within `bounds`, in whole px. */
export function clamp(value: number, bounds: Bounds): number {
  return Math.round(Math.min(bounds.max, Math.max(bounds.min, value)));
}

/**
 * Below this width the inspector is a sheet over the canvas (editor.css's @media holds the same). 760 keeps it docked
 * on a foldable phone unfolded (about 890 px wide) and on a tablet in portrait, with the canvas still 480 px wide.
 */
export const narrowWidth = 760;
export const narrowQuery = `(max-width: ${narrowWidth - 1}px)`;

/**
 * Below this width (a phone in portrait) the sheet rises from the bottom instead, so the graph stays in sight above
 * it, and the top bar drops its wordmark and engine chip (editor.css's @media holds the same).
 */
export const phoneWidth = 600;
export const phoneQuery = `(max-width: ${phoneWidth - 1}px)`;
