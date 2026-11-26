// health.ts against docs/editor.md#a-nodes-health: every phase's word, style, line and enabled actions; a starting
// phase's line hidden for the first 250 ms the editor shows it; an unknown state and phase drawn by their word.
import { describe, expect, it } from "vitest";
import { describeHealth, startingLineDelayMs, statePulses, stateTone } from "./health";
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
    const loop = node({ phase: "configuring", waitingFor: [{ kind: "loop", name: "sendspin" }, { kind: "consumer" }] });
    expect(describeHealth(loop, at).line).toBe("waiting for its loop sendspin, its consumer");
    const store = node({ phase: "configuring", waitingFor: [{ kind: "store", name: "sendspin-fpp.key" }] });
    expect(describeHealth(store, at).line).toBe("waiting for its store sendspin-fpp.key");
    const recording = node({ phase: "configuring", waitingFor: [{ kind: "recording", name: "take1.wav" }] });
    expect(describeHealth(recording, at).line).toBe("waiting for its recording take1.wav");
    const claim = node({ phase: "configuring", waitingFor: [{ kind: "claim", name: "udp:5004" }] });
    expect(describeHealth(claim, at).line).toBe("waiting for its claim udp:5004");
    const program = node({ phase: "configuring", waitingFor: [{ kind: "program", name: "shairport-sync" }] });
    expect(describeHealth(program, at).line).toBe("waiting for its program shairport-sync");
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
    const phaseOnly = describeHealth(node({ state: "mystery", phase: "retuning" }), long);
    expect(phaseOnly.word).toBe("retuning");
    expect(phaseOnly.styleClass).toBe("state-unknown");
    // An unknown phase of a known state keeps the state's word and style.
    expect(describeHealth(node({ state: "running", phase: "retuning" }), long).styleClass).toBe("state-running");
  });

  it("reconfiguring has its own style and a line from its keys and next", () => {
    const view = describeHealth(
      node({ state: "reconfiguring", phase: "reconfiguring", keys: ["port"], next: ["latencyMs"] }),
      long,
    );
    expect(view.word).toBe("reconfiguring");
    expect(view.styleClass).toBe("state-reconfiguring");
    expect(view.line).toBe("reconfiguring port; latencyMs next");
    expect(view.actions.start.enabled).toBe(false);
    expect(view.actions.stop).toEqual({ enabled: true, why: "ends the change and stops" });
    expect(view.actions.restart).toEqual({ enabled: true, why: "ends the change and restarts" });
    expect(view.actions.disable).toEqual({ enabled: true, why: "ends the change and disables" });
  });

  it("a change of ports says so, restarting and configuring", () => {
    const reshape = { kind: "reshape", keys: ["playback"], added: [], removed: ["play.2"] };
    expect(describeHealth(node({ state: "stopping", phase: "stopping", cause: { kind: "restarting", restart: reshape } }), long).line).toBe(
      "restarting: ports changed (playback)",
    );
    expect(describeHealth(node({ state: "starting", phase: "configuring", cause: reshape }), long).line).toBe(
      "configuring: ports changed (playback)",
    );
  });

  it("a fallback's restart says what could not be applied in place", () => {
    const fellBack = { kind: "fell-back", keys: ["port"], reason: "deadline", detail: "deadline 10s passed" };
    expect(describeHealth(node({ state: "stopping", phase: "stopping", cause: { kind: "restarting", restart: fellBack } }), long).line).toBe(
      "restarting: could not apply port in place",
    );
  });
});

describe("stateTone and statePulses", () => {
  it("give each state its pill tone: silence asked for quiet or off, a change in place the accent", () => {
    expect(stateTone(node({ state: "running", phase: "running" }))).toBe("ok");
    expect(stateTone(node({ state: "starting" }))).toBe("warn");
    expect(stateTone(node({ state: "stopping", phase: "stopping" }))).toBe("warn");
    expect(stateTone(node({ state: "failed", phase: "failed" }))).toBe("err");
    expect(stateTone(node({ state: "stopped", phase: "stopped" }))).toBe("quiet");
    expect(stateTone(node({ state: "disabled", phase: "disabled" }))).toBe("off");
    expect(stateTone(node({ state: "reconfiguring", phase: "reconfiguring" }))).toBe("accent");
  });

  it("draw a state this editor does not know in warn, as its style class does", () => {
    expect(stateTone(node({ state: "hibernating" }))).toBe("warn");
    expect(stateTone(node({ state: "toString" }))).toBe("warn");
  });

  it("pulse the dot only while configuring", () => {
    expect(statePulses(node({ phase: "configuring" }))).toBe(true);
    expect(statePulses(node({ phase: "added" }))).toBe(false);
    expect(statePulses(node({ state: "running", phase: "running" }))).toBe(false);
  });
});
