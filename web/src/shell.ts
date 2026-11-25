// The top bar's words and the inspector's engine facts, pure (docs/editor.md#the-top-bar,
// docs/editor.md#the-inspector): the connection pill with the seconds until the client's next socket, the engine
// chip, the chip shown while ear speaks a newer protocol than this editor, and the Graph subject's full facts.
import type { ConnectionView } from "./editor";
import { rateLabel } from "./graph";
import type { Graph } from "./protocol";

/** The newest protocol this editor knows (docs/control-port.md#protocol-and-features): a higher one shows a chip. */
export const knownProtocol = 1;

/** A state token's tone, as editor.css's tone-* classes draw it. */
export type Tone = "ok" | "warn" | "err" | "muted";

export interface Pill {
  word: string;
  tone: Tone;
  title: string;
}

const closedTitle =
  "the connection to ear closed; the canvas shows the graph as ear last sent it, and the editor starts over from a " +
  "fresh snapshot when it reconnects";

/** The whole seconds shown until `retryAt`, never 0: at the last second the retry itself changes the status. */
function secondsLeft(retryAt: number, now: number): number {
  return Math.max(1, Math.ceil((retryAt - now) / 1000));
}

/** The connection pill: a word beside its dot, the word always shown. */
export function connectionPill(view: ConnectionView, now: number): Pill {
  switch (view.status) {
    case "open":
      return { word: "live", tone: "ok", title: "connected to ear's control port" };
    case "connecting":
      return { word: "connecting…", tone: "warn", title: "opening the control port's socket" };
    case "closed":
      return {
        word: view.retryAt === undefined ? "lost" : `lost: retrying in ${secondsLeft(view.retryAt, now)} s`,
        tone: "err",
        title: closedTitle,
      };
  }
}

/**
 * When the pill's words next change: while closed and counting down, the moment the shown seconds drop; undefined
 * otherwise, including at 1 s left, where the retry itself changes the status.
 */
export function nextPillChange(view: ConnectionView, now: number): number | undefined {
  if (view.status !== "closed" || view.retryAt === undefined) return undefined;
  const shown = secondsLeft(view.retryAt, now);
  if (shown <= 1) return undefined;
  return view.retryAt - (shown - 1) * 1000;
}

export interface EngineChip {
  text: string;
  realtime: boolean;
  title: string;
}

/** The top bar's engine chip: the rate and block size, whether the audio thread is realtime, its scheduling. */
export function engineChip(graph: Graph): EngineChip {
  return {
    text: `${rateLabel(graph.graphFrequency)} · ${graph.blockSize} frames`,
    realtime: graph.realtime,
    title: `how ear's audio thread is scheduled: ${graph.scheduling}`,
  };
}

export interface ProtocolChip {
  text: string;
  title: string;
}

/** The chip shown while ear speaks a newer protocol than this editor knows; undefined otherwise. */
export function protocolChip(graph: Graph): ProtocolChip | undefined {
  const protocol = graph.protocol ?? 0;
  if (protocol <= knownProtocol) return undefined;
  return {
    text: `protocol ${protocol}`,
    title: `ear speaks protocol ${protocol}, newer than this editor's ${knownProtocol}: some of it is not shown.`,
  };
}

export interface Fact {
  key: string;
  label: string;
  value: string;
  title?: string;
  tone?: Tone;
}

/** The inspector's Graph subject: the engine facts in full, in order. */
export function engineFacts(graph: Graph): Fact[] {
  const protocol = graph.protocol ?? 0;
  const newer = protocol > knownProtocol;
  const features = graph.features ?? [];
  const facts: Fact[] = [
    { key: "rate", label: "Rate", value: rateLabel(graph.graphFrequency) },
    { key: "blockSize", label: "Block size", value: `${graph.blockSize} frames` },
    {
      key: "scheduling",
      label: "Scheduling",
      value: graph.scheduling,
      title: "how ear's audio thread is scheduled",
    },
    graph.realtime
      ? { key: "realtime", label: "Realtime", value: "yes" }
      : { key: "realtime", label: "Realtime", value: "no", tone: "err" },
    newer
      ? {
          key: "protocol",
          label: "Protocol",
          value: `${protocol} (newer than this editor's ${knownProtocol})`,
          tone: "warn",
        }
      : { key: "protocol", label: "Protocol", value: String(protocol) },
    { key: "features", label: "Features", value: features.length > 0 ? features.join(", ") : "none" },
  ];
  return facts;
}
