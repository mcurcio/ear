// The conformance fixtures (test/protocol, docs/design/batch2.md section 6), read as the wire
// type, for the UI's tests only. The server's tests (ear_control) run on the same files.
import edits from "../../test/protocol/edits.json";
import errors from "../../test/protocol/errors.json";
import levels from "../../test/protocol/levels.json";
import snapshot from "../../test/protocol/snapshot.json";
import type { ProtocolFixture } from "./protocol";

// JSON imports are typed by their contents (strings widened), so they are read as the wire type.
export const fixtures = { snapshot, edits, levels, errors } as unknown as {
  snapshot: ProtocolFixture;
  edits: ProtocolFixture;
  levels: ProtocolFixture;
  errors: ProtocolFixture;
};
