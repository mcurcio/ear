// The ear.conf drawer's gutter, pure (docs/editor.md#the-drawers-gutter): one row per line the textarea shows, its
// number, and a dot on each line a diagnostic names, the error colour over the warning's and a neutral one for a
// severity this editor does not know (docs/control-port.md#protocol-and-features). The lines are counted on the text
// as the textarea shows it (highlight.ts's shownText), so the rows line up with the highlighted copy's.
import { shownText } from "./highlight";
import type { Diagnostic } from "./protocol";

export interface GutterLine {
  /** From 1. */
  line: number;
  mark?: "error" | "warning" | "other";
  /** Where a click on the dot puts the caret: the first listed diagnostic's column on the line, else 1. */
  column: number;
  /** Each diagnostic on the line as the list under the text words it, in list order. */
  messages: string[];
}

/** The lines the textarea shows: "" is one, and a last newline adds the empty line after it. */
export function lineCount(text: string): number {
  return shownText(text).split("\n").length;
}

/** One entry per shown line; a diagnostic past the last line marks the last. */
export function gutterLines(text: string, diagnostics: readonly Diagnostic[]): GutterLine[] {
  const count = lineCount(text);
  const lines = Array.from({ length: count }, (_, index): GutterLine => ({ line: index + 1, column: 1, messages: [] }));
  for (const diagnostic of diagnostics) {
    const at = lines[Math.min(count, Math.max(1, diagnostic.line)) - 1]!;
    const severity: string = diagnostic.severity;
    if (at.messages.length === 0) at.column = diagnostic.column;
    at.messages.push(`${diagnostic.line}:${diagnostic.column} ${diagnostic.code} ${diagnostic.message}`);
    if (severity === "error") at.mark = "error";
    else if (severity === "warning") at.mark = at.mark === "error" ? "error" : "warning";
    else at.mark ??= "other";
  }
  return lines;
}

/** How many digits the widest line number has: the gutter's width. */
export function gutterDigits(count: number): number {
  return String(count).length;
}
