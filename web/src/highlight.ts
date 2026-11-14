// The knobly boxes' highlighting, pure (docs/editor.md#highlighting). Display only: it colours the text a textarea
// shows and nothing else. ear's reader is the one judge of a text and the source of every diagnostic, so nothing here
// accepts, refuses or diagnoses an edit, and nothing here reaches what Save or Add sends.
import { highlight } from "knobly/highlight";

/** One run of text drawn in one highlight class (knobly's `HighlightClass`, kept as a string: an open enum). */
export interface Segment {
  text: string;
  /** The span's class, or "" for none. */
  cls: string;
}

/**
 * The text as a textarea shows it: a textarea's value holds "\n" only, so a draft cut from a CRLF file (or holding
 * a lone "\r") is drawn with "\n" where the textarea draws its line breaks. The draft itself is never changed.
 */
export function shownText(text: string): string {
  return text.replace(/\r\n?/g, "\n");
}

/** The shown text cut into runs by knobly's highlighter; joined, they are `shownText(text)` exactly. */
export function segments(text: string): Segment[] {
  const shown = shownText(text);
  const runs: Segment[] = [];
  for (const span of highlight(shown)) {
    const cls = span.class === "space" ? "" : span.class;
    const piece = shown.slice(span.start, span.end);
    const last = runs[runs.length - 1];
    if (last !== undefined && last.cls === cls) last.text += piece;
    else runs.push({ text: piece, cls });
  }
  return runs;
}
