// The node types' schemas (feature types, docs/control-port.md#type-schemas; docs/editor.md#the-settings-form): fetched
// once per hash. The Graph names the hash of the schemas ear has (typeSchemaHash); when it is not the one held, the
// store asks types.get with the held hash as ifNoneMatch, so an ear whose types did not change answers notModified.
// The set is kept in the browser per ear (prefs.ts), so a reload draws the forms before ear answers. Pure but for the
// call it is handed.
import type { Graph, TypeSchema, TypesResult } from "./protocol";
import { typesFeature } from "./protocol";

/** One set of schemas and its hash, as ear sent them. */
export interface TypeSchemaSet {
  hash: string;
  typeSchemas: TypeSchema[];
}

/** Where a set is kept across reloads (prefs.ts's, per ear); null keeps nothing. */
export interface TypeSchemaKeeper {
  load(): TypeSchemaSet | undefined;
  save(set: TypeSchemaSet): void;
}

/** What the store needs of the client: types.get. */
export type TypesCall = (params: { ifNoneMatch?: string }) => Promise<TypesResult>;

export class TypeStore {
  private set: TypeSchemaSet | null;
  private byName = new Map<string, TypeSchema>();
  /** The hash a types.get is on its way for, so one Graph after another asks once. */
  private asking: string | null = null;
  private readonly listeners = new Set<() => void>();

  constructor(private readonly keeper: TypeSchemaKeeper | null = null) {
    this.set = keeper?.load() ?? null;
    this.index();
  }

  /** The set held, or null before any; the same object until it changes, as useSyncExternalStore needs. */
  get(): TypeSchemaSet | null {
    return this.set;
  }

  /** The held schema of `type`, when the held set is the one `graph` names (an older set may not match ear's rows). */
  schemaOf(graph: Graph | null, type: string): TypeSchema | undefined {
    if (graph === null || this.set === null || graph.typeSchemaHash !== this.set.hash) return undefined;
    return this.byName.get(type);
  }

  /** The hash `graph` names when this store must fetch it: ear has the feature, and the held set is another. */
  wanted(graph: Graph): string | undefined {
    if (!(graph.features ?? []).includes(typesFeature) || graph.typeSchemaHash === undefined) return undefined;
    if (this.set?.hash === graph.typeSchemaHash || this.asking === graph.typeSchemaHash) return undefined;
    return graph.typeSchemaHash;
  }

  /** Fetches what `graph` names, if it must (wanted); a failure leaves the held set and asks again at the next Graph. */
  async sync(graph: Graph, call: TypesCall): Promise<void> {
    const hash = this.wanted(graph);
    if (hash === undefined) return;
    this.asking = hash;
    try {
      const result = await call(this.set !== null ? { ifNoneMatch: this.set.hash } : {});
      this.take(result);
    } catch {
      // The connection closed or ear refused: the next Graph asks again.
    } finally {
      if (this.asking === hash) this.asking = null;
    }
  }

  /** A types.get result: notModified keeps the held set; a new set replaces it and is kept. */
  take(result: TypesResult): void {
    if (result.notModified === true) return;
    this.set = { hash: result.hash, typeSchemas: result.typeSchemas };
    this.index();
    this.keeper?.save(this.set);
    for (const listener of [...this.listeners]) listener();
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private index(): void {
    this.byName = new Map((this.set?.typeSchemas ?? []).map((schema) => [schema.name, schema]));
  }
}
