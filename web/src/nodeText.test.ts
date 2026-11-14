// The node panel's knobly box against node-text.json: each node's text is cut from the file where its nodeTexts say
// (a comment it owns included, a snippet's node shared); a change elsewhere moves a draft onto the new version, a
// change to the node's own text under a dirty draft makes it stale; file-changed makes it stale, invalid-text sets the
// errors; Save carries the version the text was cut from.
import { describe, expect, it } from "vitest";
import { boxFrom, boxNote, boxReduce, canSave, dirty, nodeTextIn, saveRequest, type BoxState } from "./nodeText";
import type { GraphFile, RpcErrorObject, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const messages = messagesOf(fixtures.nodeText.messages);
const v1 = (messages.find((m) => "result" in m && m.id === 1) as { result: unknown }).result as GraphFile;
const v2 = fixtures.nodeText.file!;

function reply(id: number): RpcErrorObject {
  const found = messages.find((m: ServerMessage) => "error" in m && m.id === id);
  if (found === undefined || !("error" in found)) throw new Error(`no error reply ${id}`);
  return found.error;
}

/** `file` with another version and text, its nodeTexts moved by nothing (an edit after the nodes, same length). */
function elsewhere(file: GraphFile, version: string): GraphFile {
  const text = file.text.replace("rec:record.left;", "rec:record.right;");
  return { ...file, version, text };
}

const edited = (state: BoxState, text: string) => boxReduce(state, { kind: "edit", text });

describe("a node's text in the file", () => {
  it("is cut from the file's text where nodeTexts say, a comment it owns included", () => {
    expect(nodeTextIn(v1, "rec")).toStrictEqual({
      kind: "own",
      text: '# the take\nnode file rec { path "take1.wav"; }',
    });
    expect(nodeTextIn(v1, "noise")).toStrictEqual({ kind: "own", text: "node white noise;" });
    expect(nodeTextIn(v2, "rec")).toStrictEqual({
      kind: "own",
      text: '# the take, now its own block\nnode file rec {\n    path "take2.wav";\n}',
    });
  });

  it("is shared for a node written in a snippet, with where", () => {
    expect(nodeTextIn(v1, "t1")).toStrictEqual({ kind: "shared", text: "node white $n;", at: { line: 6, column: 5 } });
  });

  it("is none for a file with errors (no nodeTexts) or a name it does not hold", () => {
    const { nodeTexts: _gone, ...broken } = v1;
    expect(nodeTextIn(broken, "rec")).toStrictEqual({ kind: "none" });
    expect(nodeTextIn(v1, "ghost")).toStrictEqual({ kind: "none" });
  });
});

describe("the node panel's knobly box", () => {
  it("starts clean on the node's text; an edit makes it dirty and offers Save", () => {
    const box = boxFrom(v1, "rec");
    expect(box.draft).toBe('# the take\nnode file rec { path "take1.wav"; }');
    expect(dirty(box)).toBe(false);
    expect(canSave(box)).toBe(false);
    const typed = edited(box, box.draft.replace("take1", "take2"));
    expect(canSave(typed)).toBe(true);
    expect(saveRequest(typed)).toStrictEqual({ name: "rec", text: typed.draft, version: "v1" });
  });

  it("moves a dirty draft onto a new version when the change was elsewhere", () => {
    const typed = edited(boxFrom(v1, "rec"), "node file rec { path x; }");
    const moved = boxReduce(typed, { kind: "file", file: elsewhere(v1, "v1b") });
    expect(moved.stale).toBe(false);
    expect(moved.draft).toBe("node file rec { path x; }");
    expect(saveRequest(moved).version).toBe("v1b");
  });

  it("goes stale when the node's own text changed under a dirty draft, keeping the draft, until reload", () => {
    const typed = edited(boxFrom(v1, "rec"), "node file rec { path x; }");
    const stale = boxReduce(typed, { kind: "file", file: v2 });
    expect(stale.stale).toBe(true);
    expect(stale.draft).toBe("node file rec { path x; }");
    expect(saveRequest(stale).version).toBe("v1");
    expect(canSave(stale)).toBe(false);
    expect(boxReduce(stale, { kind: "reload" })).toStrictEqual(boxFrom(v2, "rec"));
  });

  it("takes the new text when the draft is clean, or when ear now holds the draft (its own Save)", () => {
    expect(boxReduce(boxFrom(v1, "rec"), { kind: "file", file: v2 })).toStrictEqual(boxFrom(v2, "rec"));
    const saved = edited(boxFrom(v1, "rec"), textOfV2Rec());
    expect(boxReduce(saved, { kind: "file", file: v2 })).toStrictEqual(boxFrom(v2, "rec"));
  });

  it("goes stale on file-changed and lists invalid-text's diagnostics, placed in its text", () => {
    const typed = edited(boxFrom(v2, "rec"), 'node file rec {\n    path "take3.wav";\n    path "take4.wav";\n}');
    expect(boxReduce(typed, { kind: "refused", error: reply(4) }).stale).toBe(true);
    const refused = boxReduce(typed, { kind: "refused", error: reply(5) });
    expect(refused.errors).toStrictEqual([
      { severity: "error", code: "ear/bad-directive", message: "setting path is given twice", line: 3, column: 5 },
    ]);
    const renamed = boxReduce(edited(boxFrom(v2, "noise"), "node white hiss;"), { kind: "refused", error: reply(6) });
    expect(renamed.errors[0]).toMatchObject({ code: "ear/node-renamed", line: 1, column: 12 });
  });

  it("offers no Save for a shared node or a file with errors, and says why", () => {
    const shared = edited(boxFrom(v1, "t1"), "node white t1;");
    expect(canSave(shared)).toBe(false);
    expect(boxNote(shared)).toBe(
      "t1 is written in a snippet at 6:5 of ear.conf, with every node it makes: edit it in ear.conf below",
    );
    const { nodeTexts: _gone, ...broken } = v1;
    expect(canSave(edited(boxFrom(broken, "rec"), "x"))).toBe(false);
    expect(boxNote(boxFrom(broken, "rec"))).toBe(
      "ear.conf has errors: fix them in ear.conf below, then edit this node here",
    );
    expect(boxNote(boxFrom(v1, "rec"))).toBe("");
  });
});

function textOfV2Rec(): string {
  const found = nodeTextIn(v2, "rec");
  if (found.kind !== "own") throw new Error("rec has its own text in v2");
  return found.text;
}
