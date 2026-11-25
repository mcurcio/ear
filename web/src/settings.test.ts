// The JSON settings form's parse (docs/editor.md#adding-a-node): blank is no settings, an object is taken as typed, and
// anything else says why it is not settings.
import { describe, expect, it } from "vitest";
import { parseSettings } from "./settings";

describe("JSON settings", () => {
  it("takes blank text as no settings and an object as typed", () => {
    expect(parseSettings("")).toStrictEqual({});
    expect(parseSettings("  ")).toStrictEqual({});
    expect(parseSettings('{"level": -6}')).toStrictEqual({ level: -6 });
  });

  it("says why other text is not settings", () => {
    expect(parseSettings("[1]")).toBe("settings must be a JSON object");
    expect(parseSettings("3")).toBe("settings must be a JSON object");
    expect(parseSettings("null")).toBe("settings must be a JSON object");
    const broken = parseSettings("{");
    expect(typeof broken).toBe("string");
    expect(broken as string).toMatch(/^settings are not JSON: /);
  });
});
