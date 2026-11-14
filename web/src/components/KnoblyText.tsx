// A knobly textarea with highlighting (docs/editor.md#highlighting): the textarea holds the text, the caret, the
// selection and every edit; a highlighted copy of what it shows is drawn behind it and follows its scroll. Display
// only: the textarea's value is the caller's, and onEdit gets what the person typed exactly as the textarea gives it.
import { type Ref, useLayoutEffect, useMemo, useRef } from "react";
import { segments } from "../highlight";

export function KnoblyText({
  id,
  ref,
  rows,
  readOnly = false,
  value,
  onEdit,
}: {
  id?: string;
  ref?: Ref<HTMLTextAreaElement>;
  rows: number;
  readOnly?: boolean;
  value: string;
  onEdit: (text: string) => void;
}) {
  const shown = useRef<HTMLPreElement>(null);
  const runs = useMemo(() => segments(value), [value]);

  const follow = (area: Element | null) => {
    const pre = shown.current;
    if (pre === null || area === null) return;
    pre.scrollTop = area.scrollTop;
    pre.scrollLeft = area.scrollLeft;
  };

  // After a render (an edit, a new file), the copy holds the new text: put it where the textarea is scrolled.
  useLayoutEffect(() => follow(shown.current?.nextElementSibling ?? null));

  return (
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
}
