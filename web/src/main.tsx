// The graph editor's entry point (docs/editor.md): one editor session on the
// page's own host (ear serves the page and its control socket on the same port), then the canvas.
// React Flow's styles first, then the tokens (theme.css), then the styles that use them.
import "@xyflow/react/dist/style.css";
import "./theme.css";
import "./editor.css";
import { ReactFlowProvider } from "@xyflow/react";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { EditorContext } from "./components/hooks";
import { Editor } from "./editor";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("index.html has no #root element");
}

const scheme = window.location.protocol === "https:" ? "wss" : "ws";
const editor = new Editor({ url: `${scheme}://${window.location.host}/rpc` });
editor.client.start();

createRoot(root).render(
  <StrictMode>
    <EditorContext.Provider value={editor}>
      <ReactFlowProvider>
        <App />
      </ReactFlowProvider>
    </EditorContext.Provider>
  </StrictMode>,
);
