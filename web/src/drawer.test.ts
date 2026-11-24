// The ear.conf drawer's header and when it opens (docs/editor.md#the-drawer), replaying graph-file.json's files through
// the graph file reducer: the words and their order, the chip, opening on a refusal or a write failure only, and the
// pane's reducer before the first file.
import { describe, expect, it } from "vitest";
import { drawerChip, drawerOpens, drawerReduce, drawerStatus, type DrawerWord } from "./drawer";
import { reduceFile, type GraphFileState } from "./graphFile";
import { paneFrom, paneReduce, type PaneState } from "./pane";
import type { Diagnostic, GraphFile, RpcErrorObject } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const messages = messagesOf(fixtures.graphFile.messages);

/** Every file the reducer holds as graph-file.json's messages arrive, in order, one per change. */
const files: GraphFile[] = (() => {
  const seen: GraphFile[] = [];
  let state: GraphFileState | null = null;
  for (const message of messages) {
    const next = reduceFile(state, message);
    if (next !== null && next !== state) seen.push(next.file);
    state = next;
  }
  return seen;
})();
const byVersion = (version: string, match: (file: GraphFile) => boolean = () => true): GraphFile => {
  const found = files.find((file) => file.version === version && match(file));
  if (found === undefined) throw new Error(`no ${version} in graph-file.json`);
  return found;
};

const clean = byVersion("v1");
const withErrors = byVersion("v3"); // a hand edit with @include: an error diagnostic
const failedWrite = byVersion("v7", (file) => file.writeFailed !== undefined);
const refused = byVersion("v9"); // a hand edit ear refused, which dropped an editor edit
const restart = byVersion("v10"); // pendingRestart [graphFrequency]

const keys = (words: DrawerWord[]) => words.map((word) => word.key);
const edited = (pane: PaneState, text: string) => paneReduce(pane, { kind: "edit", text });
const invalidText = (): RpcErrorObject => {
  const found = messages.find((message) => "error" in message && message.error.data?.code === "invalid-text");
  if (found === undefined || !("error" in found)) throw new Error("no invalid-text in graph-file.json");
  return found.error;
};
const error = (line: number): Diagnostic => ({ severity: "error", code: "E1", message: "bad", line, column: 1 });

describe("the drawer's header", () => {
  it("says nothing for a clean file and an untouched pane", () => {
    expect(drawerStatus(clean, paneFrom(clean))).toStrictEqual([]);
    expect(drawerChip([])).toBeUndefined();
  });

  it("names what the file needs, most urgent first", () => {
    const refusal = refused.refusal!;
    expect(refusal).toBeDefined();
    const refusedWords = drawerStatus(refused, paneFrom(refused));
    expect(refusedWords[0]).toStrictEqual({
      key: "refused",
      text: "refused",
      tone: "err",
      title: `ear refused this file at ${refusal.line}:${refusal.column}: ${refusal.message}`,
    });
    expect(drawerStatus(failedWrite, paneFrom(failedWrite))).toStrictEqual([
      {
        key: "write-failed",
        text: "write failed",
        tone: "err",
        title: `ear could not write ear.conf: ${failedWrite.writeFailed}`,
      },
    ]);
    const all: GraphFile = {
      ...restart,
      refusal: refused.refusal,
      writeFailed: failedWrite.writeFailed,
      pendingRestart: ["graphFrequency", "blockSize"],
      diagnostics: [error(1), error(2), { ...error(3), severity: "warning" }],
    };
    const stale = paneReduce(edited(paneFrom(clean), "mine"), { kind: "file", file: all });
    expect(stale.stale).toBe(true);
    const staleWords = drawerStatus(all, stale);
    expect(keys(staleWords)).toEqual(["refused", "write-failed", "stale", "restart"]);
    expect(staleWords.find((word) => word.key === "stale")!.text).toBe("stale: reload");
    const unsaved = edited(paneFrom(all), "mine");
    const words = drawerStatus(all, unsaved);
    expect(keys(words)).toEqual(["refused", "write-failed", "unsaved", "errors", "restart"]);
    expect(words.find((word) => word.key === "errors")).toMatchObject({ text: "2 errors", tone: "err" });
    expect(words.find((word) => word.key === "restart")).toStrictEqual({
      key: "restart",
      text: "applies at restart",
      tone: "muted",
      title: "graph rate and block size change applies at restart",
    });
  });

  it("counts errors only, a refused Save's over the file's", () => {
    const words = drawerStatus(withErrors, paneFrom(withErrors));
    expect(words.find((word) => word.key === "errors")!.text).toBe("1 error");
    const invalid = invalidText();
    const pane = paneReduce(edited(paneFrom(clean), "typo"), { kind: "refused", error: invalid });
    const errors = (invalid.data!.diagnostics ?? []).filter((d) => d.severity === "error").length;
    expect(errors).toBeGreaterThan(0);
    const expected = errors === 1 ? "1 error" : `${errors} errors`;
    expect(drawerStatus(clean, pane).find((word) => word.key === "errors")!.text).toBe(expected);
  });

  it("gives the top bar's chip in the first word's tone", () => {
    const words = drawerStatus(restart, edited(paneFrom(restart), "mine"));
    expect(drawerChip(words)).toStrictEqual({ text: "unsaved · applies at restart", tone: "warn" });
    expect(drawerChip(drawerStatus(restart, paneFrom(restart)))).toStrictEqual({
      text: "applies at restart",
      tone: "muted",
    });
  });
});

describe("when the drawer opens by itself", () => {
  it("opens for the first file only when it is refused", () => {
    expect(drawerOpens(null, refused)).toBe(true);
    expect(drawerOpens(null, clean)).toBe(false);
  });

  it("opens for a refusal the person has not been shown", () => {
    const before = files[files.indexOf(refused) - 1]!;
    expect(before.refusal).toBeUndefined();
    expect(drawerOpens(before, refused)).toBe(true);
    // The same refusal on the same version again: a write state change, a reconnect's snapshot.
    expect(drawerOpens(refused, { ...refused })).toBe(false);
    expect(drawerOpens(refused, { ...refused, version: "v99" })).toBe(true);
    expect(drawerOpens(refused, { ...refused, refusal: { ...refused.refusal!, message: "other" } })).toBe(true);
    expect(drawerOpens(refused, { ...refused, refusal: { ...refused.refusal!, line: 99 } })).toBe(true);
  });

  it("opens when a write fails or fails for a new reason, not when it clears", () => {
    const before = files[files.indexOf(failedWrite) - 1]!;
    expect(drawerOpens(before, failedWrite)).toBe(true);
    expect(drawerOpens(failedWrite, { ...failedWrite })).toBe(false);
    expect(drawerOpens(failedWrite, { ...failedWrite, writeFailed: "another reason" })).toBe(true);
    expect(drawerOpens(failedWrite, files[files.indexOf(failedWrite) + 1]!)).toBe(false);
  });

  it("never opens for an edit alone: hand edits, dropped edits, pendingRestart, diagnostics", () => {
    const opened = files.slice(1).filter((file, index) => drawerOpens(files[index]!, file));
    expect(opened).toStrictEqual([failedWrite, refused]);
    expect(drawerOpens(clean, withErrors)).toBe(false);
    expect(drawerOpens(clean, restart)).toBe(false);
  });
});

describe("the drawer's pane", () => {
  it("is null until the first file, then the pane's reducer", () => {
    expect(drawerReduce(null, { kind: "edit", text: "x" })).toBeNull();
    expect(drawerReduce(null, { kind: "reload" })).toBeNull();
    expect(drawerReduce(null, { kind: "refused", error: invalidText() })).toBeNull();
    expect(drawerReduce(null, { kind: "file", file: clean })).toStrictEqual(paneFrom(clean));
    const pane = paneFrom(clean);
    expect(drawerReduce(pane, { kind: "edit", text: "x" })).toStrictEqual(edited(pane, "x"));
  });
});
