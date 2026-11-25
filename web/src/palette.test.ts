// The command palette's logic (docs/editor.md#the-palette): the commands offered per feature set and phase, replaying
// the lifecycle and unknown-values fixtures through the reducer; the ranking; the anchored popover's clamp.
import { describe, expect, it } from "vitest";
import { reduce, type GraphState } from "./graph";
import { describeHealth } from "./health";
import { anchoredAt, commandsOf, matchLabel, rank, shownCommands, type Command, type PaletteContext } from "./palette";
import type { Graph, GraphNode } from "./protocol";
import { nothingSelected, type Selection } from "./selection";
import { fixtures, messagesOf } from "./testFixtures";

const snapshot = fixtures.snapshot.graph;

function context(graph: Graph, more: Partial<PaletteContext> = {}): PaletteContext {
  return { graph, selection: nothingSelected, drawer: null, minimap: false, theme: "system", ...more };
}

const labels = (commands: readonly Command[]) => commands.map((command) => command.label);
const lifecycleWords = ["Start", "Stop", "Restart", "Disable", "Enable"];
const methodOf: Record<string, "start" | "stop" | "restart" | "disable" | "enable"> = {
  "node.start": "start",
  "node.stop": "stop",
  "node.restart": "restart",
  "node.disable": "disable",
  "node.enable": "enable",
};

/** The lifecycle commands offered for one node, as {action: hint}. */
function offered(commands: readonly Command[], node: string): Map<string, string | undefined> {
  const found = new Map<string, string | undefined>();
  for (const command of commands) {
    if (command.action.kind === "lifecycle" && command.action.node === node)
      found.set(methodOf[command.action.method]!, command.hint);
  }
  return found;
}

function enabledActions(node: GraphNode): Map<string, string | undefined> {
  const actions = describeHealth(node).actions;
  return new Map(
    Object.entries(actions)
      .filter(([, availability]) => availability.enabled)
      .map(([action, availability]) => [action, availability.why]),
  );
}

describe("the commands", () => {
  it("offers no lifecycle command without the lifecycle feature", () => {
    const graph = { ...snapshot, features: [] };
    const commands = commandsOf(context(graph));
    for (const node of graph.nodes) {
      expect(labels(commands)).toContain(`Go to ${node.name}`);
      expect(labels(commands)).toContain(`Remove ${node.name}`);
    }
    for (const type of graph.types) expect(labels(commands)).toContain(`Add ${type}`);
    expect(commands.filter((command) => command.action.kind === "remove-route")).toHaveLength(graph.routes.length);
    expect(commands.some((command) => command.action.kind === "lifecycle")).toBe(false);
    for (const word of lifecycleWords) {
      expect(labels(commands).some((label) => label.startsWith(`${word} `))).toBe(false);
    }
  });

  it("offers each node's lifecycle commands exactly as health.ts allows, replaying the lifecycle fixture", () => {
    let state: GraphState = null;
    const seen = { stopping: false, disabled: false, failed: false, lost: false };
    for (const message of messagesOf(fixtures.lifecycle.messages)) {
      state = reduce(state, message);
      if (state === null) continue;
      const commands = commandsOf(context(state));
      for (const node of state.nodes) {
        const mine = offered(commands, node.name);
        expect(mine).toStrictEqual(enabledActions(node));
        if (node.phase === "stopping") {
          seen.stopping = true;
          expect(mine.size).toBe(0);
        }
        if (node.name === "porch" && node.phase === "disabled") {
          seen.disabled = true;
          expect([...mine.keys()]).toStrictEqual(["enable"]);
        }
        if (node.name === "tape" && node.phase === "failed") {
          seen.failed = true;
          expect(mine.get("start")).toBe("try again");
        }
        if (node.blockLost) {
          seen.lost = true;
          expect(mine.has("start")).toBe(false);
          expect(mine.has("restart")).toBe(false);
        }
      }
    }
    expect(seen).toStrictEqual({ stopping: true, disabled: true, failed: true, lost: true });
  });

  it("takes the unknown-values fixture as it comes", () => {
    let state: GraphState = null;
    for (const message of messagesOf(fixtures.unknownValues.messages)) {
      state = reduce(state, message);
      if (state !== null) expect(() => commandsOf(context(state!))).not.toThrow();
    }
    const graph = state!;
    const recovering = graph.nodes.find((node) => node.state === "recovering")!;
    expect(recovering).toBeDefined();
    const commands = commandsOf(context(graph));
    expect(labels(commands)).toContain(`Go to ${recovering.name}`);
    expect(offered(commands, recovering.name)).toStrictEqual(enabledActions(recovering));
    expect(offered(commands, recovering.name).size).toBeGreaterThan(0);
    expect(graph.routes.some((route) => route.status === "held")).toBe(true);
    expect(labels(commands)).toContain("Remove route noise:noise.mono → rec:record.left");
    const invented = { ...graph, features: [...(graph.features ?? []), "invented-feature"] };
    expect(commandsOf(context(invented))).toStrictEqual(commands);
  });

  it("offers ear.conf's actions only when they apply", () => {
    const file = (drawer: PaletteContext["drawer"]) =>
      commandsOf(context(snapshot, { drawer })).filter((command) => command.group === "ear.conf");
    expect(file(null)).toStrictEqual([]);
    expect(labels(file({ open: false, canSave: false, stale: false }))).toStrictEqual(["Open ear.conf"]);
    expect(labels(file({ open: true, canSave: false, stale: false }))).toStrictEqual(["Close ear.conf"]);
    expect(labels(file({ open: true, canSave: true, stale: false }))).toStrictEqual([
      "Close ear.conf",
      "Save ear.conf",
    ]);
    expect(labels(file({ open: true, canSave: false, stale: true }))).toStrictEqual([
      "Close ear.conf",
      "Reload ear.conf",
    ]);
  });

  it("offers the view and the two other themes", () => {
    const view = (more: Partial<PaletteContext>) =>
      commandsOf(context(snapshot, more)).filter((command) => command.group === "View");
    const shown = view({});
    expect(labels(shown)).toStrictEqual([
      "Fit view",
      "Tidy layout",
      "Show minimap",
      "Theme: light",
      "Theme: dark",
      "Keyboard shortcuts",
    ]);
    expect(shown.map((command) => command.shortcut)).toStrictEqual([
      "fit",
      "tidy",
      "minimap",
      undefined,
      undefined,
      "shortcuts",
    ]);
    expect(labels(view({ minimap: true, theme: "dark" }))).toStrictEqual([
      "Fit view",
      "Tidy layout",
      "Hide minimap",
      "Theme: system",
      "Theme: light",
      "Keyboard shortcuts",
    ]);
  });

  it("puts the selection's commands first", () => {
    const selection: Selection = { nodes: ["rec", "not-in-the-graph"], routes: ["not-a-wire"] };
    const commands = commandsOf(context(snapshot, { selection }));
    const rec = snapshot.nodes.find((node) => node.name === "rec")!;
    const lifecycle = [...enabledActions(rec).keys()].length;
    const first = commands.slice(0, lifecycle + 1);
    expect(first.every((command) => command.group === "Selected")).toBe(true);
    expect(first.slice(0, lifecycle).every((command) => command.action.kind === "lifecycle")).toBe(true);
    expect(first[lifecycle]!.label).toBe("Remove rec");
    expect(commands[lifecycle + 1]!.group).toBe("Go to");
    expect(commands.filter((command) => command.group === "Selected")).toHaveLength(lifecycle + 1);
    expect(labels(commands).filter((label) => label === "Remove rec")).toHaveLength(1);
    expect(labels(commands).some((label) => label.includes("not-in-the-graph"))).toBe(false);
  });

  it("scope add gives only Add, one per type, in the graph's order", () => {
    const adds = commandsOf(context(snapshot), "add");
    expect(labels(adds)).toStrictEqual(snapshot.types.map((type) => `Add ${type}`));
    expect(adds.map((command) => command.action)).toStrictEqual(
      snapshot.types.map((type) => ({ kind: "add", type })),
    );
  });
});

describe("the ranking", () => {
  const commands = (...names: string[]): Command[] =>
    names.map((label) => ({ id: label, label, group: "View", action: { kind: "fit" } }));

  it("matches a subsequence, case-insensitively, ignoring spaces", () => {
    expect(matchLabel("fv", "Fit view")?.marks).toStrictEqual([0, 4]);
    expect(matchLabel("xyz", "Fit view")).toBeNull();
    expect(matchLabel("GO REC", "Go to rec")).not.toBeNull();
    const all = commands("Go to rec", "Fit view", "Add file");
    expect(rank(all, "").map((ranked) => ranked.command)).toStrictEqual(all);
    expect(rank(all, "   ").map((ranked) => ranked.command)).toStrictEqual(all);
  });

  it("ranks word starts and runs above scattered matches", () => {
    const desk = rank(commands("Restart desk", "Start desk", "Stop desk"), "st desk").map((r) => r.command.label);
    expect(desk.indexOf("Restart desk")).toBe(2);
    expect(rank(commands("Go to recorder", "Go to rec"), "rec").map((r) => r.command.label)).toStrictEqual([
      "Go to rec",
      "Go to recorder",
    ]);
    const tie = commands("Go to aaa", "Go to bbb");
    expect(rank(tie, "go").map((r) => r.command)).toStrictEqual(tie);
    const many = commands(...Array.from({ length: 80 }, (_, i) => `Go to n${i}`));
    expect(rank(many, "go")).toHaveLength(shownCommands);
    expect(shownCommands).toBe(50);
  });
});

describe("the anchored popover", () => {
  const box = { width: 320, height: 360 };
  const viewport = { width: 1000, height: 800 };

  it("keeps the popover inside the window", () => {
    expect(anchoredAt({ x: 990, y: 790 }, box, viewport)).toStrictEqual({ left: 1000 - 8 - 320, top: 800 - 8 - 360 });
    expect(anchoredAt({ x: 100, y: 200 }, box, viewport)).toStrictEqual({ left: 100, top: 200 });
    expect(anchoredAt({ x: 100, y: 200 }, box, { width: 200, height: 200 })).toStrictEqual({ left: 8, top: 8 });
  });
});
