// health.ts against control-plane.md 8.6: every phase's word, style, line and enabled actions; a
// starting phase's line hidden for the first 250 ms the editor shows it; an unknown state and phase drawn by their word.
import { describe, expect, it } from "vitest";
import { describeHealth, startingLineDelayMs } from "./health";
import type { GraphNode } from "./protocol";

const since = "2026-11-09T10:00:00.000000Z";
const at = 0; // ms the editor has seen the phase
const long = 10_000;

function node(health: Partial<GraphNode>): GraphNode {
  return {
    name: "desk",
    type: "card",
    settings: {},
    inputs: [],
    outputs: [],
    state: "starting",
    phase: "added",
    since,
    wanted: "running",
    blockLost: false,
    ...health,
  };
}

/** The enabled actions, by name. */
function enabled(view: ReturnType<typeof describeHealth>): string[] {
  return Object.entries(view.actions)
    .filter(([, action]) => action.enabled)
    .map(([name]) => name);
}

describe("describeHealth", () => {
  it("added: starting, restart disabled until it starts by itself; Stop, or Start once it will rest stopped", () => {
    const view = describeHealth(node({}), long);
    expect(view.word).toBe("starting");
    expect(view.styleClass).toBe("state-starting");
    expect(view.line).toBe("");
    expect(enabled(view)).toEqual(["stop", "disable"]);
    expect(view.actions.restart.why).toBe("starts by itself");
    const resting = describeHealth(node({ wanted: "stopped" }), long);
    expect(resting.line).toBe("will rest stopped");
    expect(enabled(resting)).toEqual(["start", "disable"]);
    const waiting = describeHealth(node({ waitingFor: [{ kind: "older-node", name: "desk" }] }), long);
    expect(waiting.line).toBe("waiting for the previous desk to stop");
  });

  it("configuring: its cause after 250 ms, or what it waits for at once; restart enabled", () => {
    const configuring = node({ phase: "configuring", cause: { kind: "action" } });
    expect(describeHealth(configuring, at + startingLineDelayMs - 1).line).toBe("");
    expect(describeHealth(configuring, at + startingLineDelayMs).line).toBe("configuring: restart invoked");
    expect(enabled(describeHealth(configuring, long))).toEqual(["stop", "restart", "disable"]);
    const waits = node({ phase: "configuring", waitingFor: [{ kind: "companion", name: "desk.playback" }] });
    expect(describeHealth(waits, at).line).toBe("waiting for desk.playback");
    expect(describeHealth(node({ phase: "configuring" }), long).line).toBe("configuring");
    expect(describeHealth(node({ phase: "configuring", cause: { kind: "enabling" } }), long).line).toBe(
      "configuring: enabled",
    );
    expect(describeHealth(node({ phase: "configuring", cause: { kind: "retry" } }), long).line).toBe(
      "configuring: trying again",
    );
  });

  it("configured and starting: waiting for the engine, after 250 ms", () => {
    for (const phase of ["configured", "starting"]) {
      expect(describeHealth(node({ phase }), at + 10).line).toBe("");
      const view = describeHealth(node({ phase }), long);
      expect(view.line).toBe("waiting for the engine");
      expect(view.word).toBe("starting");
    }
  });

  it("running: Stop, restart and Disable", () => {
    const view = describeHealth(node({ state: "running", phase: "running" }), long);
    expect(view.styleClass).toBe("state-running");
    expect(view.line).toBe("");
    expect(enabled(view)).toEqual(["stop", "restart", "disable"]);
  });

  it("stopping: every action disabled, and why it stops in words", () => {
    const cases: [GraphNode["cause"], string][] = [
      [{ kind: "stop" }, "stopping"],
      [{ kind: "disabling" }, "disabling"],
      [{ kind: "removing" }, "removing"],
      [{ kind: "restarting", restart: { kind: "action" } }, "restarting: restart invoked"],
      [{ kind: "restarting", restart: { kind: "settings", keys: ["card"] } }, "restarting: settings changed (card)"],
    ];
    for (const [cause, line] of cases) {
      const view = describeHealth(node({ state: "stopping", phase: "stopping", cause }), long);
      expect(view.line).toBe(line);
      expect(enabled(view)).toEqual([]);
    }
  });

  it("stopped: quiet, Start, restart and Disable; not when the block is lost", () => {
    const view = describeHealth(node({ state: "stopped", phase: "stopped", wanted: "stopped" }), long);
    expect(view.styleClass).toBe("state-stopped");
    expect(view.line).toBe("stopped until Start or a restart of ear");
    expect(enabled(view)).toEqual(["start", "restart", "disable"]);
    const lost = describeHealth(node({ state: "stopped", phase: "stopped", blockLost: true }), long);
    expect(enabled(lost)).toEqual(["disable"]);
  });

  it("disabled: Enable alone, and why the others are not", () => {
    const view = describeHealth(node({ state: "disabled", phase: "disabled", wanted: "disabled", disabled: true }), long);
    expect(view.styleClass).toBe("state-disabled");
    expect(view.line).toBe("disabled in ear.conf");
    expect(enabled(view)).toEqual(["enable"]);
    expect(view.actions.start.why).toBe("disabled in ear.conf: enable it first");
  });

  it("failed: the failure, Start (try again) and restart unless the block is lost", () => {
    const reason = { code: "engine-refused", detail: "cannot open hw:CARD=MOTU" };
    const view = describeHealth(node({ state: "failed", phase: "failed", reason }), long);
    expect(view.styleClass).toBe("state-failed");
    expect(view.line).toBe("cannot open hw:CARD=MOTU");
    expect(enabled(view)).toEqual(["start", "stop", "restart", "disable"]);
    expect(view.actions.start.why).toBe("try again");
    const lost = describeHealth(node({ state: "failed", phase: "failed", reason, blockLost: true }), long);
    expect(enabled(lost)).toEqual(["stop", "disable"]);
  });

  it("an unknown state and phase are drawn by their word, neutrally", () => {
    const view = describeHealth(node({ state: "recovering", phase: "recovering" }), long);
    expect(view.word).toBe("recovering");
    expect(view.styleClass).toBe("state-unknown");
    const phaseOnly = describeHealth(node({ state: "mystery", phase: "reconfiguring" }), long);
    expect(phaseOnly.word).toBe("reconfiguring");
    expect(phaseOnly.styleClass).toBe("state-unknown");
    // An unknown phase of a known state keeps the state's word and style.
    expect(describeHealth(node({ state: "running", phase: "reconfiguring" }), long).styleClass).toBe("state-running");
  });
});
