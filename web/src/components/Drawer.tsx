// The ear.conf drawer (docs/editor.md#the-drawer), as the slots of patchbay's AppShell drawer: a header that never goes
// away and says in patchbay's StatusWords what the file needs (drawer.ts), with Save (patchbay's small Button, showing
// Saving… while its request is on its way, docs/editor.md#requests-in-flight), and the pane as its body. The shell
// draws the show and hide button and the splitter on its top edge, and keeps the body mounted while closed, so the
// draft, the caret and the scroll are kept.
import { StatusWords } from "@mcurcio/patchbay/composites";
import { Button } from "@mcurcio/patchbay/primitives";
import type { AppShellDrawer } from "@mcurcio/patchbay/shell";
import { TextPane } from "./TextPane";
import type { DrawerModel } from "./useDrawer";

/** The drawer's slots for AppShell. */
export function drawerSlots(drawer: DrawerModel): AppShellDrawer {
  return {
    header: (
      <>
        <span className="drawer-title">ear.conf</span>
        <StatusWords
          className="drawer-status"
          items={drawer.status.map((word) => ({ tone: word.tone, label: word.text, title: word.title }))}
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
      </>
    ),
    // The top bar's ear.conf toggle names this body in its aria-controls.
    content: (
      <div id="drawer-body" className="drawer-body">
        <TextPane drawer={drawer} />
      </div>
    ),
    open: drawer.open,
    onOpen: drawer.setOpen,
    height: drawer.height,
    onHeight: drawer.setHeight,
  };
}
