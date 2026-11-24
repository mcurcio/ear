import { describe, expect, it } from "vitest";
import { AutoFit, fitKey } from "./fit";

describe("fitKey", () => {
  it("changes with the size and with the nodes, not with sub-pixel sizes", () => {
    const key = fitKey(1000, 700, ["a", "b"]);
    expect(fitKey(1000.2, 699.8, ["a", "b"])).toBe(key);
    expect(fitKey(900, 700, ["a", "b"])).not.toBe(key);
    expect(fitKey(1000, 700, ["a"])).not.toBe(key);
    expect(fitKey(1000, 700, ["a", "c"])).not.toBe(key);
  });
});

describe("AutoFit", () => {
  it("fits once per new key", () => {
    const fit = new AutoFit();
    expect(fit.due("k1")).toBe(true);
    expect(fit.due("k1")).toBe(false);
    expect(fit.due("k2")).toBe(true);
  });

  it("stops fitting once the person moves the view", () => {
    const fit = new AutoFit();
    expect(fit.due("k1")).toBe(true);
    fit.hold();
    expect(fit.due("k2")).toBe(false);
    expect(fit.due("k3")).toBe(false);
  });

  it("fits again after the person asks for a fit, from the next change", () => {
    const fit = new AutoFit();
    fit.due("k1");
    fit.hold();
    expect(fit.due("k2")).toBe(false);
    fit.release();
    expect(fit.due("k3")).toBe(true);
  });

  it("a key seen while held is still new once released", () => {
    const fit = new AutoFit();
    fit.due("k1");
    fit.hold();
    fit.due("k2");
    fit.release();
    expect(fit.due("k2")).toBe(true);
  });
});
