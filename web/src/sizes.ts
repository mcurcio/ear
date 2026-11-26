// The shell's sizes, pure (docs/editor.md#the-shell): the inspector's width and the drawer's height with their bounds,
// which patchbay's Splitter keeps a drag or a key within. editor.css holds the same bounds, which sizes.test.ts
// checks. The widths below which the inspector is a sheet are patchbay's useMediaBreakpoint's
// (docs/editor.md#narrow-windows).

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
