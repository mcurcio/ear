// The warning mark (docs/editor.md#recent-marks): an "!" in a circle while something rose in the last minute, the one
// look companions' badges and the counters share. The glyph is hidden from screen readers beside its words.
export function WarnMark({ text, title }: { text: string; title?: string }) {
  return (
    <>
      <span className="warn-mark" aria-hidden="true" title={title}>
        !
      </span>
      <span className="visually-hidden">{text}</span>
    </>
  );
}
