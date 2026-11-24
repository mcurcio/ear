// Settings typed as JSON by a person, for an ear without the node-text feature (docs/editor.md#adding-a-node): the
// add-node form's and the inspector's Node subject's JSON boxes. A person edits knobly where ear can read it; JSON is
// the machine API (docs/editor.md#its-rules).
import type { JsonObject } from "./protocol";

/** Settings typed as JSON text: the object, or the reason it is not one. Blank text is no settings. */
export function parseSettings(text: string): JsonObject | string {
  if (text.trim() === "") return {};
  try {
    const value: unknown = JSON.parse(text);
    if (typeof value === "object" && value !== null && !Array.isArray(value)) return value as JsonObject;
    return "settings must be a JSON object";
  } catch (error) {
    return `settings are not JSON: ${(error as Error).message}`;
  }
}
