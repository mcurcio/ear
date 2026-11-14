// The add-node form's knobly text against node-text.json: a template per type with a free name, never written over
// what the person typed; Add sends the text and the newest version; invalid-text's diagnostics are placed in the text,
// file-changed asks for Add again, and a node added gives a fresh template.
import { describe, expect, it } from "vitest";
import { addFrom, addReduce, addRequest, canAdd, freeName, templateFor } from "./addText";
import type { GraphFile, RpcErrorObject, ServerMessage } from "./protocol";
import { fixtures, messagesOf } from "./testFixtures";

const messages = messagesOf(fixtures.nodeText.messages);
const file = fixtures.nodeText.file as GraphFile;
const names = fixtures.nodeText.graph.nodes.map((node) => node.name); // noise, rec, rec2, t1

function reply(id: number): RpcErrorObject {
  const found = messages.find((m: ServerMessage) => "error" in m && m.id === id);
  if (found === undefined || !("error" in found)) throw new Error(`no error reply ${id}`);
  return found.error;
}

describe("the add-node form's text", () => {
  it("starts from a template: the type's statement with the first free name and an empty block", () => {
    expect(freeName("white", names)).toBe("white1");
    expect(freeName("t", names)).toBe("t2"); // t1 is taken
    expect(freeName("a.b c", [])).toBe("abc1");
    expect(templateFor("file", names)).toBe("node file file1 {\n}");
    expect(addFrom("file", names).text).toBe("node file file1 {\n}");
  });

  it("follows the chosen type while untouched, and never writes over what the person typed", () => {
    const untouched = addReduce(addFrom("file", names), { kind: "type", type: "white", names });
    expect(untouched.text).toBe("node white white1 {\n}");
    const typed = addReduce(untouched, { kind: "edit", text: 'node file take { path "t.wav"; }' });
    expect(addReduce(typed, { kind: "type", type: "card", names }).text).toBe('node file take { path "t.wav"; }');
  });

  it("sends the text and the newest version it holds", () => {
    const state = addReduce(addFrom("file", names), { kind: "edit", text: "node white n3;" });
    expect(canAdd(state)).toBe(true);
    expect(addRequest(state, file)).toStrictEqual({ text: "node white n3;", version: "v3" });
    expect(canAdd(addReduce(state, { kind: "edit", text: "  \n" }))).toBe(false);
  });

  it("lists invalid-text's diagnostics placed in its text, asks again on file-changed, and resets once added", () => {
    const state = addReduce(addFrom("file", names), { kind: "edit", text: 'node file take {\n    path "t.wav" }' });
    const refused = addReduce(state, { kind: "refused", error: reply(11) });
    expect(refused.errors).toStrictEqual([
      { severity: "error", code: "E2001", message: 'unexpected token: "}"', line: 2, column: 18 },
    ]);
    expect(refused.text).toBe(state.text);
    const raced = addReduce(state, { kind: "refused", error: reply(10) });
    expect(raced.raced).toBe(true);
    expect(raced.text).toBe(state.text);
    expect(addReduce(raced, { kind: "added", type: "file", names: [...names, "take"] })).toStrictEqual(
      addFrom("file", [...names, "take"]),
    );
  });
});
