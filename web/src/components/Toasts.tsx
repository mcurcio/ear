// The toast queue drawn at the canvas's bottom right (docs/editor.md#toasts): the newest four, older ones folded into
// "and n more", each saying what was asked and why it was refused, with the code in monospace, a count while it
// coalesces, Show for a node or route ear holds, a gesture's "Set -3.5 dB again" (its node.set), and Dismiss. None expires while the toasts are hovered or focused;
// Escape on a focused toast dismisses it. The rules are notices.ts's; each toast's own role announces it (a refusal is
// an alert, anything else a status), so the region itself is not a live region and nothing is read twice.
import { Panel, useReactFlow } from "@xyflow/react";
import { useCallback, useEffect, useState } from "react";
import { showOf, shownNotices } from "../notices";
import type { Selection } from "../selection";
import { useEditor, useExpiry, useGraph, useNotices } from "./hooks";

export function Toasts({ onShow }: { onShow: (selection: Selection) => void }) {
  const editor = useEditor();
  const { notices } = editor;
  const view = useNotices();
  const graph = useGraph();
  const { fitView } = useReactFlow();
  const [hovered, setHovered] = useState(false);
  const [focused, setFocused] = useState(false);

  useEffect(() => notices.hold(hovered || focused), [hovered, focused, notices]);
  useEffect(() => () => notices.hold(false), [notices]);
  const expire = useCallback(() => notices.expire(), [notices]);
  useExpiry(notices.nextExpiry(), expire);

  const { shown, more } = shownNotices(view);
  // A toast removed under the focus or the pointer fires no blur or leave, so the hold is let go here.
  const dismiss = (id: number) => {
    setFocused(false);
    notices.dismiss(id);
  };
  useEffect(() => {
    if (shown.length > 0) return;
    setHovered(false);
    setFocused(false);
  }, [shown.length]);
  if (shown.length === 0) return null;

  return (
    <Panel position="bottom-right" className="toasts">
      <section
        aria-label="Notifications"
        onMouseEnter={() => setHovered(true)}
        onMouseLeave={() => setHovered(false)}
        onFocus={() => setFocused(true)}
        onBlur={(event) => {
          if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setFocused(false);
        }}
      >
        {more > 0 && <p className="toast-more">and {more} more</p>}
        <ol className="toast-list">
          {shown.map((notice) => {
            const target = showOf(graph, notice.subject);
            return (
              <li
                key={notice.id}
                className={`toast toast-${notice.severity}`}
                role={notice.severity === "error" ? "alert" : "status"}
                onKeyDown={(event) => {
                  // Escape here is the toast's alone, so the narrow sheet's Escape does not also close it.
                  if (event.key !== "Escape") return;
                  event.preventDefault();
                  event.stopPropagation();
                  dismiss(notice.id);
                }}
              >
                <p className="toast-text">
                  {notice.what} {notice.verb}: <code className="toast-code">{notice.code}</code>
                  {notice.detail !== "" && ` ${notice.detail}`}
                  {notice.count > 1 && (
                    <span className="toast-count">
                      <span aria-hidden="true"> ×{notice.count}</span>
                      <span className="visually-hidden">, {notice.count} times</span>
                    </span>
                  )}
                </p>
                <div className="toast-actions">
                  {target !== null && (
                    <button
                      type="button"
                      onClick={() => {
                        onShow(target.selection);
                        void fitView({ nodes: target.nodes.map((id) => ({ id })), padding: 0.4, duration: 180 });
                      }}
                    >
                      Show
                    </button>
                  )}
                  {notice.again !== undefined && (
                    <button
                      type="button"
                      onClick={() => {
                        const again = notice.again!;
                        editor.send("node.set", { name: again.name, settings: again.settings });
                        dismiss(notice.id);
                      }}
                    >
                      {notice.again.label}
                    </button>
                  )}
                  <button type="button" aria-label={`Dismiss: ${notice.what}`} onClick={() => dismiss(notice.id)}>
                    Dismiss
                  </button>
                </div>
              </li>
            );
          })}
        </ol>
      </section>
    </Panel>
  );
}
