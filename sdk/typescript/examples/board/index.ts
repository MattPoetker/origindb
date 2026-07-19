// =============================================================================
// Board — a complete, minimal OriginDB module: a shared realtime notes wall.
//
// This is the whole backend. Deploy it, and any client can pin notes that
// appear live on every other connected screen — no server code beyond this file.
//
// Table:
//   notes   key = <id>   { id, user, text, x, y, color, created }
//
// Reducers:
//   addNote(user, text, x, y, color) -> { "id": "..." }   pin a note
//   clearNotes()                     -> { "cleared": n }   wipe the wall
//
// Build (from sdk/typescript):
//   npx asc examples/board/index.ts --config examples/board/asconfig.json --target release
//   -> build/board.wasm
// =============================================================================

import {
  JsonValue,
  abortCall,
  declareTable,
  deleteTable,
  generateId,
  nowMs,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "../../assembly/index";

// Every module entry file re-exports the ABI surface the SDK implements.
export {
  origindb_alloc,
  origindb_free,
  origindb_describe,
  origindb_invoke,
  __origindb_abort,
} from "../../assembly/index";

setModuleInfo("board", "1.0.0");
declareTable("notes");

const SCAN_LIMIT: i32 = 100000; // scan every row (the SDK default caps at 1000)

// addNote(user, text, x, y, color): pin a note. x/y are 0..1 fractions of the
// wall so any screen size renders it in the same place. Returns the new id.
function addNote(args: Array<JsonValue>): JsonValue | null {
  const user = args.length > 0 && args[0].asString().length > 0 ? args[0].asString() : "anon";
  const text = args.length > 1 ? args[1].asString() : "";
  const x = args.length > 2 ? clamp01(args[2].asNumber()) : 0.5;
  const y = args.length > 3 ? clamp01(args[3].asNumber()) : 0.5;
  const color = args.length > 4 && args[4].asString().length > 0 ? args[4].asString() : "#ffc24b";

  if (text.length == 0) abortCall("addNote: text is required"); // traps → whole call rolls back

  const id = generateId().toString();
  writeTable(
    "notes",
    id,
    JsonValue.newObject()
      .setString("id", id)
      .setString("user", user)
      .setString("text", text.length > 240 ? text.substr(0, 240) : text)
      .setNumber("x", x)
      .setNumber("y", y)
      .setString("color", color)
      .setNumber("created", <f64>nowMs())
      .toString()
  );
  // The commit emits exactly one changefeed event — every subscriber sees it.
  return JsonValue.newObject().setString("id", id);
}

// clearNotes(): delete every note. Reads its own staged writes, so one call
// removes them all atomically.
function clearNotes(args: Array<JsonValue>): JsonValue | null {
  const rows = JsonValue.parse(scanTable("notes", "", SCAN_LIMIT));
  let n = 0;
  for (let i = 0; i < rows.length; i++) {
    deleteTable("notes", rows.at(i).getString("key", ""));
    n++;
  }
  return JsonValue.newObject().setNumber("cleared", <f64>n);
}

function clamp01(v: f64): f64 {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

registerReducer("addNote", addNote, ["user", "text", "x", "y", "color"]);
registerReducer("clearNotes", clearNotes, []);
