// The busy mark on a button whose request is on its way (docs/editor.md#requests-in-flight): an open ring that turns,
// and under reduced motion stands still. Hidden from screen readers: the button carries aria-busy and its words.
export function Spinner() {
  return <span className="spinner" aria-hidden="true" />;
}
