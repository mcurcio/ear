// A node's health in words, style and enabled buttons (docs/design/control-plane.md 8.6, the home of
// what each phase shows; docs/design/node-lifecycle-modules.md 5.11): the one place that turns the
// wire's health members into what the editor draws. Silence the operator asked for (stopped,
// disabled) is drawn quiet, never as a fault; a value this editor does not know is drawn by its
// word, neutrally (control-plane.md 4.1).
import type { CauseKind, GraphNode, NodeCause, WaitingFor } from "./protocol";

export interface ActionAvailability {
  enabled: boolean;
  /** Why it is disabled, or what it does (a tooltip). */
  why?: string;
}

export interface HealthView {
  /** The state word, or the phase's word for an unknown state. */
  word: string;
  /** "state-<state>" for known states, "state-unknown" otherwise. */
  styleClass: string;
  /** The line under the word: why the node is silent, what it waits for, why it stopped. */
  line: string;
  actions: {
    start: ActionAvailability;
    stop: ActionAvailability;
    restart: ActionAvailability;
    disable: ActionAvailability;
    enable: ActionAvailability;
  };
}

/** A starting phase's line shows only once the phase has lasted this long (8.6: fast phases do not flicker). */
export const startingLineDelayMs = 250;

const knownStates = new Set(["starting", "running", "stopping", "stopped", "disabled", "failed"]);

const on: ActionAvailability = { enabled: true };
const off = (why: string): ActionAvailability => ({ enabled: false, why });

const disableWhy = "release the device and keep it off across restarts; adds disabled true; to its block in ear.conf";
const enableWhy = "removes disabled true; from its block in ear.conf";

/** A restart's own kind in words (control-plane.md 9.1's sentences). */
function restartWords(kind: CauseKind, keys: string[] | undefined): string {
  switch (kind) {
    case "settings":
      return keys !== undefined && keys.length > 0 ? `settings changed (${keys.join(", ")})` : "settings changed";
    case "action":
      return "restart invoked";
    case "enabling":
      return "enabled";
    default:
      return kind;
  }
}

/** A cause in words: stopping's ("disabling", "restarting: restart invoked") or configuring's. */
function causeWords(cause: NodeCause): string {
  switch (cause.kind) {
    case "stop":
      return "stopping";
    case "removing":
      return "removing";
    case "disabling":
      return "disabling";
    case "restarting":
      return cause.restart !== undefined
        ? `restarting: ${restartWords(cause.restart.kind, cause.restart.keys)}`
        : "restarting";
    case "settings":
      return `configuring: ${restartWords("settings", cause.keys)}`;
    case "action":
      return "configuring: restart invoked";
    case "retry":
      return "configuring: trying again";
    case "enabling":
      return "configuring: enabled";
    default:
      return cause.kind;
  }
}

/** What a node waits for, in words ("waiting for fpp.rtp", "waiting for the previous tape to stop"). */
function waitingWords(waits: WaitingFor[]): string {
  const each = waits.map((wait) => {
    switch (wait.kind) {
      case "older-node":
        return `the previous ${wait.name ?? "node"} to stop`;
      case "block":
        return "the engine";
      case "companion":
        return wait.name ?? "a companion";
      case "consumer":
        return "its consumer";
      default:
        return wait.name !== undefined ? `${wait.kind} ${wait.name}` : wait.kind;
    }
  });
  return `waiting for ${each.join(", ")}`;
}

/** How long the phase has lasted at `now`; Infinity when `since` is absent or unreadable (show the line). */
function lasted(since: string | undefined, now: number): number {
  if (since === undefined) return Infinity;
  const began = Date.parse(since);
  return Number.isNaN(began) ? Infinity : now - began;
}

/** `now` (ms since the epoch) hides a starting phase's line for its first 250 ms. */
export function describeHealth(node: GraphNode, now: number): HealthView {
  const known = knownStates.has(node.state);
  const word = known ? node.state : node.phase;
  const styleClass = known ? `state-${node.state}` : "state-unknown";
  const waits = node.waitingFor ?? [];
  const lost = node.blockLost;
  const disabledWhy = "disabled in ear.conf: enable it first";
  const lostWhy = "the engine's block is lost: restart ear";
  const willRest = node.wanted === "stopped";
  // Stop or Start by the node's stop, while it is not yet configured or configuring.
  const stopOrStart = {
    start: willRest ? on : off("it will start by itself"),
    stop: willRest ? off("it will rest stopped") : on,
  };
  const disable = { enabled: true, why: disableWhy };
  const enable = off("it is enabled");

  switch (node.phase) {
    case "added": {
      const line = waits.length > 0 ? waitingWords(waits) : willRest ? "will rest stopped" : "";
      return {
        word,
        styleClass,
        line,
        actions: { ...stopOrStart, restart: off("starts by itself"), disable, enable },
      };
    }
    case "configuring":
    case "configured":
    case "starting": {
      let line: string;
      if (waits.length > 0) line = waitingWords(waits);
      else if (lasted(node.since, now) < startingLineDelayMs) line = "";
      else if (node.phase === "configuring")
        line = node.cause !== undefined ? causeWords(node.cause) : "configuring";
      else line = "waiting for the engine";
      return { word, styleClass, line, actions: { ...stopOrStart, restart: on, disable, enable } };
    }
    case "running":
      return {
        word,
        styleClass,
        line: "",
        actions: { start: off("it runs"), stop: on, restart: on, disable, enable },
      };
    case "stopping": {
      const all = off("it is stopping");
      return {
        word,
        styleClass,
        line: node.cause !== undefined ? causeWords(node.cause) : "stopping",
        actions: { start: all, stop: all, restart: all, disable: all, enable: all },
      };
    }
    case "stopped":
      return {
        word,
        styleClass,
        line: "stopped until Start or a restart of ear",
        actions: {
          start: lost ? off(lostWhy) : on,
          stop: off("it is stopped"),
          restart: lost ? off(lostWhy) : on,
          disable,
          enable,
        },
      };
    case "disabled":
      return {
        word,
        styleClass,
        line: "disabled in ear.conf",
        actions: {
          start: off(disabledWhy),
          stop: off(disabledWhy),
          restart: off(disabledWhy),
          disable: off("it is disabled"),
          enable: { enabled: true, why: enableWhy },
        },
      };
    case "failed":
      return {
        word,
        styleClass,
        line: node.reason?.detail ?? "failed",
        actions: {
          start: lost ? off(lostWhy) : { enabled: true, why: "try again" },
          stop: willRest ? off("it will stay failed until Start") : on,
          restart: lost ? off(lostWhy) : on,
          disable,
          enable,
        },
      };
    default:
      // A phase this editor does not know: its word, neutral, and only what cannot harm.
      return {
        word,
        styleClass,
        line: node.cause !== undefined ? causeWords(node.cause) : "",
        actions: { start: on, stop: on, restart: lost ? off(lostWhy) : on, disable, enable },
      };
  }
}
