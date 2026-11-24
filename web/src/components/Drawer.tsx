// The ear.conf drawer (docs/editor.md#the-drawer): full width along the bottom, a header that never goes away and says
// in words what the file needs (drawer.ts), its toggle and Save (Saving… while its request is on its way,
// docs/editor.md#requests-in-flight), and the pane as its body, resized by its top edge.
// Closed, the body is hidden but stays mounted, so the draft, the caret and the scroll are kept.
import type { CSSProperties } from "react";
import { drawerBounds } from "../sizes";
import { Spinner } from "./Spinner";
import { Splitter } from "./Splitter";
import { TextPane } from "./TextPane";
import type { DrawerModel } from "./useDrawer";

export function Drawer({ drawer }: { drawer: DrawerModel }) {
  const { open, setOpen, status, height } = drawer;
  return (
    <section className="drawer" data-open={open} aria-label="ear.conf">
      {open && (
        <Splitter
          orientation="horizontal"
          label="Resize ear.conf"
          value={height}
          bounds={drawerBounds(window.innerHeight)}
          growKey="ArrowUp"
          onChange={drawer.setHeight}
        />
      )}
      <header className="drawer-header">
        <button
          type="button"
          className="drawer-toggle"
          aria-expanded={open}
          aria-controls="drawer-body"
          onClick={() => setOpen(!open)}
        >
          <span aria-hidden="true">{open ? "▾" : "▴"}</span> ear.conf
        </button>
        <ul className="drawer-status" aria-live="polite">
          {status.map((word) => (
            <li key={word.key} className={`drawer-word tone-${word.tone}`} title={word.title}>
              {word.text}
            </li>
          ))}
        </ul>
        <button
          type="button"
          className="drawer-save"
          disabled={!drawer.canSave}
          aria-busy={drawer.saving || undefined}
          onClick={drawer.save}
        >
          {drawer.saving && <Spinner />}
          {drawer.saving ? "Saving…" : "Save"}
        </button>
      </header>
      <div
        id="drawer-body"
        className="drawer-body"
        hidden={!open}
        style={{ "--drawer-height": `${height}px` } as CSSProperties}
      >
        <TextPane drawer={drawer} />
      </div>
    </section>
  );
}
