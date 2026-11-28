// What the pane, the node box and the add form send (docs/editor.md#highlighting): the text as typed, and a CRLF
// file's text unedited as the file holds it, non-ASCII text included. The boxes only show it highlighted.
import { describe, expect, it } from "vitest";
import { addFrom, addReduce, addRequest } from "./addText";
import { boxFrom, boxReduce, saveRequest as boxSave } from "./nodeText";
import { paneFrom, paneReduce, saveRequest as paneSave } from "./pane";
import type { GraphFile } from "./protocol";

const texts = [
  'node file rec {\r\n  path "take1.wav"; # the take\r\n}\r\n',
  'node white bruit { level -6 dB; }\n# café, naïve, 日本語, 🎧\nnode file élan { path "çà/🎧.wav"; }\n',
  "graph { rate 48 kHz; }\r\nnode card $ch {\r\n  @use card_out MOTU;\r",
  '"unterminated\r\n\\q $ ` stray \u{1F3A7}\uD800',
  "\r\n\r\n",
  "",
];

const file = (text: string): GraphFile =>
  ({ version: "v1", text, diagnostics: [], pendingRestart: [] }) as unknown as GraphFile;

describe("the knobly boxes' text", () => {
  it("never changes what the pane, the node box or the add form send", () => {
    for (const text of texts) {
      // The pane: the draft as the file holds it (a CRLF file's text, unedited), and as typed.
      expect(paneSave(paneFrom(file(text))).text).toBe(text);
      expect(paneSave(paneReduce(paneFrom(file("x")), { kind: "edit", text })).text).toBe(text);
      // The node box and the add form send their drafts as typed.
      expect(boxSave(boxReduce(boxFrom(file("x"), "rec"), { kind: "edit", text })).text).toBe(text);
      expect(addRequest(addReduce(addFrom("file", []), { kind: "edit", text }), file("x")).text).toBe(text);
    }
  });
});
