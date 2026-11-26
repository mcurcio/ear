// What the browser keeps (docs/editor.md#what-the-browser-keeps): ear's keys and the checks of every kept value, and
// its places. The simple values (the inspector's width, the drawer, the minimap) are patchbay's usePersistentState
// under these keys and checks, the theme its useTheme under themeKey(), and the places are read and written here with
// patchbay's readKept and writeKept, so a private window or blocked storage keeps a page's-life editor and a bad value
// is ignored. Places, sizes and the minimap are kept per ear (keyed by the page's host), the theme per browser. Places
// are never sent to ear (docs/editor.md#its-rules). Times here are the wall clock (Date.now), since they outlive the
// page; every other clock in the editor is monotonic.
import { readKept, writeKept, type StorageLike } from "@mcurcio/patchbay/hooks";
import type { PlaceKeeper, Position } from "./layout";
import { clamp, inspectorBounds } from "./sizes";

/** Every key's start; the number moves if a value's shape ever changes. */
export const prefsPrefix = "ear-editor/1";

/** What is kept per ear. */
export type EarPref = "places" | "inspector-width" | "drawer" | "minimap";

/** The theme's key: one per browser. App's useTheme (patchbay's) keeps the choice under it, and index.html's inline
 * script reads it (docs/editor.md#tokens-and-themes). */
export function themeKey(): string {
  return `${prefsPrefix}/theme`;
}

/** A per-ear key: the ear is the page's location.host. */
export function earKey(host: string, pref: EarPref): string {
  return `${prefsPrefix}/${host}/${pref}`;
}

/** A place not seen for 30 days is forgotten. */
export const placeKeepMs = 30 * 24 * 60 * 60 * 1000;

/** At most this many places are kept, the most recently seen. */
export const maxPlaces = 500;

/** A place and when its node was last seen, in ms on the wall clock. */
export interface KeptPlace {
  x: number;
  y: number;
  seen: number;
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** A finite number, clamped to the inspector's bounds. */
export function checkWidth(value: unknown): number | undefined {
  return isFiniteNumber(value) ? clamp(value, inspectorBounds) : undefined;
}

/** A boolean open and a finite height above 0; the drawer clamps the height to the window it finds. */
export function checkDrawer(value: unknown): { open: boolean; height: number } | undefined {
  if (!isPlainObject(value)) return undefined;
  const { open, height } = value;
  if (typeof open !== "boolean" || !isFiniteNumber(height) || height <= 0) return undefined;
  return { open, height };
}

export function checkFlag(value: unknown): boolean | undefined {
  return typeof value === "boolean" ? value : undefined;
}

/** The most recently seen `maxPlaces` of `places`. */
function capped(places: Map<string, KeptPlace>): Map<string, KeptPlace> {
  if (places.size <= maxPlaces) return places;
  return new Map([...places].sort((a, b) => b[1].seen - a[1].seen).slice(0, maxPlaces));
}

/**
 * Kept places: a plain object of non-empty name to {x, y, seen}, each a finite number; a bad entry is dropped and the
 * rest kept. A place unseen for more than placeKeepMs before `now` is dropped; a seen in the future counts as now.
 */
export function checkPlaces(value: unknown, now: number): Map<string, KeptPlace> {
  const places = new Map<string, KeptPlace>();
  if (!isPlainObject(value)) return places;
  for (const [name, entry] of Object.entries(value)) {
    if (name === "" || !isPlainObject(entry)) continue;
    const { x, y, seen } = entry;
    if (!isFiniteNumber(x) || !isFiniteNumber(y) || !isFiniteNumber(seen)) continue;
    const when = Math.min(seen, now);
    if (now - when > placeKeepMs) continue;
    places.set(name, { x, y, seen: when });
  }
  return capped(places);
}

/**
 * The places to keep: the present ones seen `now`; an absent one kept with its own seen unless `forgetAbsent` (Tidy)
 * or it expired. At most maxPlaces.
 */
export function mergePlaces(
  kept: ReadonlyMap<string, KeptPlace>,
  present: ReadonlyMap<string, Position>,
  now: number,
  forgetAbsent: boolean,
): Map<string, KeptPlace> {
  const merged = new Map<string, KeptPlace>();
  if (!forgetAbsent) {
    for (const [name, place] of kept) {
      if (!present.has(name) && now - Math.min(place.seen, now) <= placeKeepMs) merged.set(name, place);
    }
  }
  for (const [name, { x, y }] of present) merged.set(name, { x, y, seen: now });
  return capped(merged);
}

/** The editor's kept values over one storage, for one ear: the keys of its simple values, and its places. */
export class Prefs {
  constructor(
    /** Where everything is kept; null keeps nothing. */
    readonly storage: StorageLike | null,
    private readonly host: string,
    private readonly now: () => number = Date.now,
  ) {}

  /** This ear's key for `pref`, for usePersistentState with its check (checkWidth, checkDrawer, checkFlag). */
  key(pref: EarPref): string {
    return earKey(this.host, pref);
  }

  private keptPlaces(): Map<string, KeptPlace> {
    return readKept(this.storage, this.key("places"), (value) => checkPlaces(value, this.now())) ?? new Map();
  }

  /** The kept places, checked and unexpired. */
  places(): Map<string, Position> {
    return new Map([...this.keptPlaces()].map(([name, { x, y }]) => [name, { x, y }]));
  }

  /** Reads, merges `present` in (mergePlaces) and writes. */
  savePlaces(present: ReadonlyMap<string, Position>, forgetAbsent: boolean): void {
    const merged = mergePlaces(this.keptPlaces(), present, this.now(), forgetAbsent);
    writeKept(this.storage, this.key("places"), Object.fromEntries(merged));
  }

  /** layout.ts's Places keeps its places through this. */
  placeKeeper(): PlaceKeeper {
    return { load: () => this.places(), save: (present, forgetAbsent) => this.savePlaces(present, forgetAbsent) };
  }
}
