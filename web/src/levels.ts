// Live audio levels, kept outside React state (docs/design/batch2.md section 8). Each `levels`
// notification is a complete snapshot (section 6), so it replaces the store: a node missing from
// it has no level and reads silent. Listeners are per port, so at 30 messages a second only the
// meters and wires whose port changed re-render, never the whole canvas.
import type { Level, Notifications, PortRef } from "./protocol";

export type LevelSnapshot = Notifications["levels"]["nodes"];

/** The quietest level a meter shows; anything quieter (and silence) reads as this. */
export const floorDb = -60;

/** A linear level (1.0 = full scale) in dBFS, never below floorDb. */
export function toDbfs(linear: number): number {
  if (!(linear > 0)) return floorDb;
  return Math.max(floorDb, 20 * Math.log10(linear));
}

/** How full a meter is, 0 at floorDb and 1 at full scale (0 dBFS). */
export function meterFraction(linear: number): number {
  return Math.min(1, (toDbfs(linear) - floorDb) / -floorDb);
}

const silent: Level = [0, 0];

/** A route carries one channel: a wire's level is its from port's (section 6). */
export function wirePort(route: { from: PortRef }): PortRef {
  return route.from;
}

export function wireLevel(store: LevelStore, route: { from: PortRef }): Level {
  const port = wirePort(route);
  return store.get(port.node, port.port);
}

export class LevelStore {
  private snapshot: LevelSnapshot = {};
  private readonly listeners = new Map<string, Map<string, Set<() => void>>>();

  /** The port's latest level; [0, 0] when the latest snapshot has none for it. */
  get(node: string, port: string): Level {
    return this.snapshot[node]?.[port] ?? silent;
  }

  /** Replaces every level with this snapshot and tells the listeners of each port that changed. */
  replace(next: LevelSnapshot): void {
    const previous = this.snapshot;
    this.snapshot = next;
    const told: Array<() => void> = [];
    for (const [node, ports] of this.listeners) {
      for (const [port, listeners] of ports) {
        const before = previous[node]?.[port] ?? silent;
        const after = next[node]?.[port] ?? silent;
        if (before[0] !== after[0] || before[1] !== after[1]) told.push(...listeners);
      }
    }
    for (const listener of told) listener();
  }

  /** Everything reads silent (the connection is gone, so nothing is measured). */
  clear(): void {
    this.replace({});
  }

  /** Calls `listener` whenever this port's level changes; returns the unsubscribe. */
  subscribe(node: string, port: string, listener: () => void): () => void {
    let ports = this.listeners.get(node);
    if (ports === undefined) this.listeners.set(node, (ports = new Map()));
    let set = ports.get(port);
    if (set === undefined) ports.set(port, (set = new Set()));
    set.add(listener);
    return () => {
      set.delete(listener);
      if (set.size === 0 && ports.get(port) === set) ports.delete(port);
      if (ports.size === 0 && this.listeners.get(node) === ports) this.listeners.delete(node);
    };
  }
}
