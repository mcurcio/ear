// The toasts at the canvas's bottom right (docs/editor.md#toasts): patchbay's Toaster over the session's queue
// (notices.ts), inline in a React Flow panel so Show can move the view. Each says what was asked and why it was
// refused, with the code in monospace, and has Show for a node or route ear holds; the Toaster draws the newest four
// with "and n more", the count, Dismiss, Escape on a focused toast, the hold while hovered or focused, and the two live
// regions that read each toast once (a refusal at once, anything else when the reader is free).
import { Toaster, type ToasterToast } from "@mcurcio/patchbay/composites";
import { Panel, useReactFlow } from "@xyflow/react";
import { useCallback, useEffect } from "react";
import { showOf } from "../notices";
import type { Selection } from "../selection";
import { useEditor, useExpiry, useGraph, useNotices } from "./hooks";

export function Toasts({ onShow }: { onShow: (selection: Selection) => void }) {
  const { notices } = useEditor();
  const view = useNotices();
  const graph = useGraph();
  const { fitView } = useReactFlow();

  const expire = useCallback(() => notices.expire(), [notices]);
  useExpiry(notices.nextExpiry(), expire);
  useEffect(() => () => notices.hold(false), [notices]);

  const toasts = view.shown.map((notice): ToasterToast => {
    const target = showOf(graph, notice.subject);
    const toast: ToasterToast = {
      id: notice.id,
      tone: notice.tone,
      count: notice.count,
      text: (
        <>
          {notice.what} {notice.verb}: <code className="toast-code">{notice.code}</code>
          {notice.detail !== "" && ` ${notice.detail}`}
        </>
      ),
    };
    if (target !== null) {
      toast.action = {
        label: "Show",
        run: () => {
          onShow(target.selection);
          void fitView({ nodes: target.nodes.map((id) => ({ id })), padding: 0.4, duration: 180 });
        },
      };
    }
    return toast;
  });

  return (
    <Panel position="bottom-right" className="toasts">
      <Toaster
        placement="inline"
        toasts={toasts}
        more={view.more}
        onDismiss={(id) => notices.dismiss(id)}
        onHold={(held) => notices.hold(held)}
      />
    </Panel>
  );
}
