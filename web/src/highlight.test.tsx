// The highlighting is display only (docs/editor.md#highlighting): it never changes the text a box holds or what Save
// and Add send, CRLF and non-ASCII text included.
import { renderToStaticMarkup } from "react-dom/server";
import { describe, expect, it } from "vitest";
import { addFrom, addReduce, addRequest } from "./addText";
import { KnoblyText } from "./components/KnoblyText";
import { segments, shownText } from "./highlight";
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

/** What the browser's HTML parser makes of a text node's markup (React escapes these five). */
const unescape = (html: string) =>
  html
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&quot;", '"')
    .replaceAll("&#x27;", "'")
    .replaceAll("&amp;", "&");

describe("knobly highlighting", () => {
  it("draws exactly the text the textarea shows, in one piece per run", () => {
    for (const text of texts) {
      const runs = segments(text);
      expect(runs.map((run) => run.text).join("")).toBe(shownText(text));
      expect(shownText(text)).not.toContain("\r");
      for (const run of runs) expect(run.text).not.toBe("");
    }
  });

  it("classes the tokens by knobly's classes", () => {
    const runs = segments('node white w { level 3; } # c\n@set $x "s";');
    const cls = (text: string) => runs.find((run) => run.text === text)?.cls;
    expect(cls("node")).toBe("directive");
    expect(cls("# c")).toBe("comment");
    expect(cls('"s"')).toBe("string");
    expect(cls("@set")).toBe("keyword");
    expect(cls("3")).toBe("number");
  });

  it("hands the textarea the caller's text unchanged", () => {
    for (const text of texts) {
      const html = renderToStaticMarkup(<KnoblyText rows={3} value={text} onEdit={() => {}} />);
      const area = /<textarea[^>]*>([\s\S]*)<\/textarea>/.exec(html);
      expect(area).not.toBeNull();
      // React writes a textarea's value as its content, after one leading "\n" a parser drops.
      expect(unescape(area![1]).replace(/^\n/, "")).toBe(text);
    }
  });

  it("never changes what the pane, the node box or the add form send", () => {
    for (const text of texts) {
      segments(text); // highlighted as it is drawn
      // The pane: the draft as the file holds it (a CRLF file's text, unedited), and as typed.
      expect(paneSave(paneFrom(file(text))).text).toBe(text);
      expect(paneSave(paneReduce(paneFrom(file("x")), { kind: "edit", text })).text).toBe(text);
      // The node box and the add form send their drafts as typed.
      expect(boxSave(boxReduce(boxFrom(file("x"), "rec"), { kind: "edit", text })).text).toBe(text);
      expect(addRequest(addReduce(addFrom("file", []), { kind: "edit", text }), file("x")).text).toBe(text);
    }
  });
});
