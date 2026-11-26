// The ear.conf drawer (docs/editor.md#the-drawer): full width along the bottom, a header that never goes away and says
// in patchbay's StatusWords what the file needs (drawer.ts), its toggle and Save (Saving… while its request is on its way,
// docs/editor.md#requests-in-flight), and the pane as its body, resized by its top edge.
// Closed, the body is hidden but stays mounted, so the draft, the caret and the scroll are kept.
import type { CSSProperties } from "react";
import { drawerBounds } from "../sizes";
import { StatusWords } from "@mcurcio/patchbay/composites";
import { Button, Splitter } from "@mcurcio/patchbay/primitives";
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
          {...drawerBounds(window.innerHeight)}
          edge="start"
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
        <StatusWords
          className="drawer-status"
          items={status.map((word) => ({ tone: word.tone, label: word.text, title: word.title }))}
        />
        <Button
          size="sm"
          className="drawer-save"
          busy={drawer.saving}
          busyLabel="Saving…"
          disabled={!drawer.saving && !drawer.canSave}
          onClick={drawer.save}
        >
          Save
        </Button>
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
