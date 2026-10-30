// The graph editor's entry point. A placeholder until the editor lands
// (docs/design/batch2.md section 8): it proves the toolchain builds a React page.
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";

const root = document.getElementById("root");
if (root === null) {
  throw new Error("index.html has no #root element");
}

createRoot(root).render(
  <StrictMode>
    <h1>EAR</h1>
    <p>The graph editor is coming.</p>
  </StrictMode>,
);
