// The ear.conf drawer's state (docs/editor.md#the-drawer), held by App so the top bar's toggle and the drawer's
// header read one status: the pane (pane.ts, over null until the first file), whether the drawer is open, its body's
// height, Save (saving while its request is on its way, docs/editor.md#requests-in-flight), and opening by itself only
// on a refusal or a write failure the person has not been shown (drawer.ts). Whether it is open and its height are
// kept per ear (prefs.ts, docs/editor.md#what-the-browser-keeps).
import { usePersistentState } from "@mcurcio/patchbay/hooks";
import { useCallback, useEffect, useMemo, useReducer, useRef, type Dispatch } from "react";
import { drawerOpens, drawerReduce, drawerStatus, type DrawerWord } from "../drawer";
import type { GraphFileState } from "../graphFile";
import { placedRefusal } from "../notices";
import { dirty, paneFrom, saveRequest, type PaneEvent, type PaneState } from "../pane";
import type { GraphFile, RpcErrorCode } from "../protocol";
import { isAsking } from "../requests";
import { RpcError } from "../rpc";
import { drawerInitialHeight } from "../sizes";
import { checkDrawer, type Prefs } from "../prefs";
import { useEditor, useRequests } from "./hooks";

export interface DrawerModel {
  /** The file and its notices. */
  state: GraphFileState;
  pane: PaneState;
  dispatch: Dispatch<PaneEvent>;
  status: DrawerWord[];
  open: boolean;
  setOpen: (open: boolean) => void;
  /** The drawer's height in px while open, its header included; AppShell draws it within its bounds. */
  height: number;
  setHeight: (px: number) => void;
  saving: boolean;
  canSave: boolean;
  save: () => void;
}

/** The drawer's state; null until ear has sent a graph file (an ear without one shows no drawer). */
export function useDrawer(state: GraphFileState | null, prefs: Prefs): DrawerModel | null {
  const editor = useEditor();
  const saving = isAsking(useRequests(), "graphFile.set");
  const file = state?.file ?? null;
  const [pane, dispatch] = useReducer(drawerReduce, file, (first: GraphFile | null) =>
    first === null ? null : paneFrom(first),
  );
  // Kept per ear by patchbay's usePersistentState, under prefs.ts's key (docs/editor.md#what-the-browser-keeps).
  const [kept, setKept] = usePersistentState(
    prefs.key("drawer"),
    { open: false, height: drawerInitialHeight },
    checkDrawer,
    { storage: prefs.storage },
  );
  const { open, height } = kept;
  const setOpen = useCallback((next: boolean) => setKept((was) => ({ ...was, open: next })), [setKept]);
  const previous = useRef<GraphFile | null>(null);

  useEffect(() => {
    if (file !== null) dispatch({ kind: "file", file });
  }, [file]);

  // Opens only for what the person has not been shown; a hand edit never opens it.
  useEffect(() => {
    if (file !== null && drawerOpens(previous.current, file)) setOpen(true);
    previous.current = file;
  }, [file]);

  // Kept as the person set it: AppShell draws it within the bounds of the window as it is, so a height kept from a
  // taller window comes back there.
  const setHeight = useCallback((px: number) => setKept((was) => ({ ...was, height: px })), [setKept]);

  const save = useCallback(() => {
    if (pane === null) return;
    // The drawer shows file-changed and invalid-text itself (stale, the diagnostics); any other is a toast
    // (docs/editor.md#where-a-refusal-lands).
    editor.ask("graphFile.set", saveRequest(pane), { placed: placedRefusal }).catch((error: unknown) => {
      if (!(error instanceof RpcError)) return;
      const refused = { code: error.code as RpcErrorCode, message: error.message, data: error.data };
      dispatch({ kind: "refused", error: refused });
      // A refused Save opens the drawer, so its diagnostics or the stale banner are in view.
      setOpen(true);
    });
  }, [editor, pane, setOpen]);

  return useMemo(() => {
    if (state === null || pane === null) return null;
    return {
      state,
      pane,
      dispatch,
      status: drawerStatus(state.file, pane),
      open,
      setOpen,
      height,
      setHeight,
      saving,
      canSave: !saving && !pane.stale && dirty(pane),
      save,
    };
  }, [state, pane, open, setOpen, height, setHeight, saving, save]);
}
