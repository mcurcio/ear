// A knobly textarea with highlighting (docs/editor.md#highlighting): the textarea holds the text, the caret, the
// selection and every edit; a highlighted copy of what it shows is drawn behind it and follows its scroll. Display
// only: the textarea's value is the caller's, and onEdit gets what the person typed exactly as the textarea gives it.
// The drawer's gutter is a second pre beside the box, never inside it, following the textarea's vertical scroll
// (docs/editor.md#the-drawers-gutter); without one the markup is the box alone.
import { type CSSProperties, Fragment, type Ref, useLayoutEffect, useMemo, useRef } from "react";
import { gutterDigits, type GutterLine } from "../gutter";
import { segments } from "../highlight";

export function KnoblyText({
  id,
  ref,
  rows,
  readOnly = false,
  value,
  onEdit,
  gutter,
  onGutterGo,
}: {
  id?: string;
  ref?: Ref<HTMLTextAreaElement>;
  rows: number;
  readOnly?: boolean;
  value: string;
  onEdit: (text: string) => void;
  /** The gutter's rows (gutter.ts), one per shown line; none draws no gutter. */
  gutter?: readonly GutterLine[];
  /** A click on a gutter dot: put the caret there. */
  onGutterGo?: (line: number, column: number) => void;
}) {
  const shown = useRef<HTMLPreElement>(null);
  const gutterRef = useRef<HTMLPreElement>(null);
  const runs = useMemo(() => segments(value), [value]);

  const follow = (area: Element | null) => {
    const pre = shown.current;
    if (pre === null || area === null) return;
    pre.scrollTop = area.scrollTop;
    pre.scrollLeft = area.scrollLeft;
    if (gutterRef.current !== null) gutterRef.current.scrollTop = area.scrollTop;
  };

  // After a render (an edit, a new file), the copy holds the new text: put it where the textarea is scrolled.
  useLayoutEffect(() => follow(shown.current?.nextElementSibling ?? null));

  const box = (
    <div className={readOnly ? "knobly-text knobly-text-readonly" : "knobly-text"}>
      <pre ref={shown} className="knobly-text-shown" aria-hidden="true">
        <code>
          {runs.map((run, index) =>
            run.cls === "" ? (
              run.text
            ) : (
              <span key={index} className={`hl hl-${run.cls}`}>
                {run.text}
              </span>
            ),
          )}
          {/* A last empty line takes its height, as the textarea gives it. */}
          {"\n"}
        </code>
      </pre>
      <textarea
        id={id}
        ref={ref}
        className="text-pane-text knobly-text-input"
        rows={rows}
        spellCheck={false}
        readOnly={readOnly}
        value={value}
        onChange={(event) => onEdit(event.target.value)}
        onScroll={(event) => follow(event.currentTarget)}
      />
    </div>
  );
  if (gutter === undefined) return box;

  return (
    <div className="knobly-text-row">
      <pre
        ref={gutterRef}
        className="knobly-gutter"
        aria-hidden="true"
        style={{ "--gutter-digits": gutterDigits(gutter.length) } as CSSProperties}
      >
        <code>
          {gutter.map((row) => (
            <Fragment key={row.line}>
              {row.line}
              {row.mark !== undefined ? (
                <span
                  className={`gutter-dot gutter-dot-${row.mark}`}
                  title={row.messages.join("\n")}
                  onClick={() => onGutterGo?.(row.line, row.column)}
                />
              ) : (
                <span className="gutter-dot gutter-dot-none" />
              )}
              {"\n"}
            </Fragment>
          ))}
        </code>
      </pre>
      {box}
    </div>
  );
}
