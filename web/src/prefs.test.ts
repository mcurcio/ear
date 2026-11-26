// What the browser keeps (docs/editor.md#what-the-browser-keeps): storage that throws, values of the wrong shape, the
// 30-day expiry of places, the keys per ear, and the theme's key, where patchbay's useTheme keeps the choice.
import { checkTheme, readKept, writeKept } from "@mcurcio/patchbay/hooks";
import { describe, expect, it } from "vitest";
import { Places, placeNodes, type Position } from "./layout";
import {
  checkDrawer,
  checkFlag,
  checkPlaces,
  checkWidth,
  earKey,
  maxPlaces,
  placeKeepMs,
  Prefs,
  themeKey,
  type StorageLike,
} from "./prefs";
import { fixtures } from "./testFixtures";

class MapStorage implements StorageLike {
  readonly items = new Map<string, string>();
  getItem(key: string): string | null {
    return this.items.get(key) ?? null;
  }
  setItem(key: string, value: string): void {
    this.items.set(key, value);
  }
  removeItem(key: string): void {
    this.items.delete(key);
  }
}

/** Blocked storage: every access throws, as a SecurityError or QuotaExceededError does. */
class ThrowingStorage implements StorageLike {
  getItem(): string | null {
    throw new Error("SecurityError: storage is blocked");
  }
  setItem(): void {
    throw new Error("QuotaExceededError: storage is full");
  }
  removeItem(): void {
    throw new Error("SecurityError: storage is blocked");
  }
}

const host = "ear.local:7777";
const at = (x: number, y: number): Position => ({ x, y });

describe("storage that throws", () => {
  it("keeps today's behaviour", () => {
    for (const storage of [new ThrowingStorage(), null]) {
      const prefs = new Prefs(storage, host);
      expect(prefs.inspectorWidth()).toBeUndefined();
      expect(prefs.drawer()).toBeUndefined();
      expect(prefs.minimap()).toBeUndefined();
      expect(prefs.places()).toStrictEqual(new Map());
      expect(() => {
        prefs.setInspectorWidth(400);
        prefs.setDrawer({ open: true, height: 300 });
        prefs.setMinimap(true);
        prefs.savePlaces(new Map([["rec", at(1, 2)]]), false);
        prefs.savePlaces(new Map(), true);
      }).not.toThrow();
      const graph = fixtures.snapshot.graph;
      expect(new Places(prefs.placeKeeper()).place(graph, new Map())).toStrictEqual(placeNodes(graph, new Map()));
    }
  });
});

describe("values of the wrong shape", () => {
  it("ignores them", () => {
    const storage = new MapStorage();
    const prefs = new Prefs(storage, host, () => 1000);
    storage.setItem(themeKey(), "not json");
    expect(readKept(storage, themeKey(), checkTheme)).toBeUndefined();
    for (const value of ["Dark", 3]) {
      storage.setItem(themeKey(), JSON.stringify(value));
      expect(readKept(storage, themeKey(), checkTheme)).toBeUndefined();
    }
    for (const value of ["320", null, Infinity]) expect(checkWidth(value)).toBeUndefined();
    storage.setItem(earKey(host, "inspector-width"), "1e999");
    expect(prefs.inspectorWidth()).toBeUndefined();
    for (const value of [{ open: "yes", height: 200 }, { open: true, height: -1 }, null, [true, 200]]) {
      expect(checkDrawer(value)).toBeUndefined();
    }
    expect(checkFlag(1)).toBeUndefined();
    const badPlaces = [
      [],
      null,
      { rec: { x: "1", y: 2, seen: 0 } },
      { rec: { x: 1, y: 2 } },
      { "": { x: 1, y: 2, seen: 0 } },
    ];
    for (const value of badPlaces) {
      expect(checkPlaces(value, 1000)).toStrictEqual(new Map());
    }
    const mixed = checkPlaces({ rec: { x: 1, y: 2, seen: 0 }, bad: { x: 1, y: null, seen: 0 } }, 1000);
    expect([...mixed.keys()]).toStrictEqual(["rec"]);
  });

  it("clamps a kept width to the inspector's bounds", () => {
    expect(checkWidth(100)).toBe(280);
    expect(checkWidth(9999)).toBe(560);
  });
});

describe("each preference", () => {
  it("round-trips", () => {
    const prefs = new Prefs(new MapStorage(), host, () => 1000);
    prefs.setInspectorWidth(400);
    prefs.setDrawer({ open: true, height: 333 });
    prefs.setMinimap(true);
    prefs.savePlaces(new Map([["rec", at(-5, 7.5)]]), false);
    expect(prefs.inspectorWidth()).toBe(400);
    expect(prefs.drawer()).toStrictEqual({ open: true, height: 333 });
    expect(prefs.minimap()).toBe(true);
    expect(prefs.places()).toStrictEqual(new Map([["rec", at(-5, 7.5)]]));
  });
});

describe("places", () => {
  it("a place unseen for 30 days is forgotten", () => {
    const storage = new MapStorage();
    let now = 1_000_000;
    const prefs = new Prefs(storage, host, () => now);
    const t0 = now;
    prefs.savePlaces(new Map([["rec", at(1, 1)]]), false);
    now = t0 + placeKeepMs;
    expect(prefs.places().has("rec")).toBe(true);
    now = t0 + placeKeepMs + 1;
    expect(prefs.places().has("rec")).toBe(false);

    now = t0 + 1000;
    prefs.savePlaces(new Map([["noise", at(2, 2)]]), false);
    const kept = JSON.parse(storage.getItem(earKey(host, "places"))!);
    expect(kept).toStrictEqual({ rec: { x: 1, y: 1, seen: t0 }, noise: { x: 2, y: 2, seen: t0 + 1000 } });
    prefs.savePlaces(new Map([["noise", at(2, 2)]]), true);
    expect([...prefs.places().keys()]).toStrictEqual(["noise"]);

    // A seen in the future counts as now.
    const future = checkPlaces({ rec: { x: 1, y: 1, seen: now + 1e12 } }, now);
    expect(future.get("rec")).toStrictEqual({ x: 1, y: 1, seen: now });
  });

  it("keeps at most 500 places, the most recently seen", () => {
    const value: Record<string, { x: number; y: number; seen: number }> = {};
    for (let i = 0; i < maxPlaces + 20; i++) value[`n${i}`] = { x: i, y: i, seen: i };
    const kept = checkPlaces(value, maxPlaces + 20);
    expect(kept.size).toBe(maxPlaces);
    expect(kept.has("n0")).toBe(false);
    expect(kept.has(`n${maxPlaces + 19}`)).toBe(true);

    const prefs = new Prefs(new MapStorage(), host, () => 10_000);
    prefs.savePlaces(new Map(Object.keys(value).map((name, i) => [name, at(i, i)])), false);
    expect(prefs.places().size).toBe(maxPlaces);
  });
});

describe("the keys", () => {
  it("keys places and sizes per ear and the theme per browser", () => {
    const storage = new MapStorage();
    const a = new Prefs(storage, "a:7777", () => 1000);
    const b = new Prefs(storage, "b:7777", () => 1000);
    a.savePlaces(new Map([["rec", at(1, 1)]]), false);
    a.setInspectorWidth(400);
    a.setDrawer({ open: true, height: 300 });
    a.setMinimap(true);
    writeKept(storage, themeKey(), "dark");
    expect(b.places()).toStrictEqual(new Map());
    expect(b.inspectorWidth()).toBeUndefined();
    expect(b.drawer()).toBeUndefined();
    expect(b.minimap()).toBeUndefined();
    expect(readKept(storage, themeKey(), checkTheme)).toBe("dark");
    expect(storage.items.has("ear-editor/1/a:7777/places")).toBe(true);
    expect(storage.items.has("ear-editor/1/theme")).toBe(true);
    expect(earKey("a:7777", "places")).toBe("ear-editor/1/a:7777/places");
    expect(themeKey()).toBe("ear-editor/1/theme");
  });

  it("reads a theme kept before the editor took patchbay's useTheme", () => {
    // ear kept the choice as JSON under the same key, as patchbay's useTheme does, so a choice made before still holds.
    const storage = new MapStorage();
    storage.setItem("ear-editor/1/theme", JSON.stringify("light"));
    expect(readKept(storage, themeKey(), checkTheme)).toBe("light");
  });
});
