// Vite builds the graph editor into dist/, which ear serves (the run image's /opt/ear/web);
// Vitest runs its tests (no browser: the logic is tested outside React).
import react from "@vitejs/plugin-react";
import { defineConfig } from "vitest/config";

export default defineConfig({
  plugins: [react()],
  server: {
    // "npm run dev" serves the editor with hot reload and forwards its control-port socket to
    // a running ear. changeOrigin sends Host "localhost:7777", a host ear's Host check accepts.
    proxy: {
      "/rpc": { target: "http://localhost:7777", ws: true, changeOrigin: true },
    },
  },
  test: {
    // Until the first test file lands (web/src/protocol.test.ts), an empty run passes.
    passWithNoTests: true,
    // Vitest blanks every stylesheet by default, even one imported "?raw"; theme.test.ts reads editor.css and
    // patchbay's tokens.css as text (docs/editor.md#tokens-and-themes).
    css: { include: [/\/src\/editor\.css/, /\/@mcurcio\/patchbay\/dist\/tokens\.css/] },
    // patchbay's component modules import their own stylesheets, which Node cannot load; inlined, Vite takes the
    // imports (and blanks them), so the logic tests can call patchbay's pure helpers (rankCommands, findShortcut).
    server: { deps: { inline: [/@mcurcio\/patchbay/] } },
  },
});
