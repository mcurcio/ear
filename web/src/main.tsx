// The graph editor's entry point (docs/editor.md): one editor session on the
// page's own host (ear serves the page and its control socket on the same port), then the canvas.
// React Flow's styles first, then patchbay's tokens (which point React Flow's --xy-* variables at them), then ear's
// tokens (theme.css), then the styles that use them (docs/editor.md#tokens-and-themes).
import "@xyflow/react/dist/style.css";
import "@mcurcio/patchbay/tokens.css";
import "./theme.css";
import "./editor.css";
import { ReactFlowProvider } from "@xyflow/react";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import { EditorContext } from "./components/hooks";
import { Editor } from "./editor";
import { browserStorage, Prefs } from "./prefs";
import { applyTheme, defaultTheme } from "./theme";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("index.html has no #root element");
}

const scheme = window.location.protocol === "https:" ? "wss" : "ws";
const editor = new Editor({ url: `${scheme}://${window.location.host}/rpc` });
editor.client.start();

// What the browser keeps (docs/editor.md#what-the-browser-keeps): places and sizes per ear (this page's host), the
// theme per browser, applied before the first render so a kept dark choice never flashes light.
const prefs = new Prefs(browserStorage(), window.location.host);
applyTheme(document.documentElement, prefs.theme() ?? defaultTheme);

createRoot(root).render(
  <StrictMode>
    <EditorContext.Provider value={editor}>
      <ReactFlowProvider>
        <App prefs={prefs} />
      </ReactFlowProvider>
    </EditorContext.Provider>
  </StrictMode>,
);
