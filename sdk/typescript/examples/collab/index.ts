// =============================================================================
// InstantDB example module: collab — shared realtime workspace
//
// Reducers:
//   join(user, color)                  -> presence row + activity entry
//   moveCursor(user, color, x, y)      -> upsert live cursor position
//   addNote(user, color, x, y, text)   -> pinned note + activity entry
//   removeNote(id, user)               -> delete a note + activity entry
//   clearBoard(user)                   -> delete all notes + activity entry
//
// Tables: presence, cursors, notes, activity.
// Rows keyed "__*" are seed rows created by __init so subscriptions get a
// valid initial_state; clients must skip them.
//
// Build (from sdk/typescript):
//   npx asc examples/collab/index.ts --outFile build/collab.wasm \
//     --runtime minimal --exportRuntime false \
//     --use abort=assembly/index/__instantdb_abort -O3 --shrinkLevel 1
// =============================================================================

import {
  JsonValue,
  abortCall,
  declareTable,
  deleteTable,
  generateId,
  logInfo,
  nowMs,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "../../assembly/index";

export {
  instantdb_alloc,
  instantdb_free,
  instantdb_describe,
  instantdb_invoke,
  __instantdb_abort,
} from "../../assembly/index";

setModuleInfo("collab", "1.0.0");
declareTable("presence");
declareTable("cursors");
declareTable("notes");
declareTable("activity");

function requireString(args: Array<JsonValue>, index: i32, name: string): string {
  const v = index < args.length ? args[index].asString() : "";
  if (v.length == 0) {
    abortCall("missing required argument: " + name);
  }
  return v;
}

function logActivity(user: string, color: string, action: string, detail: string): void {
  const id = generateId().toString();
  const row = JsonValue.newObject()
    .setString("id", id)
    .setString("user", user)
    .setString("color", color)
    .setString("action", action)
    .setString("detail", detail)
    .setNumber("ts", <f64>nowMs());
  writeTable("activity", id, row.toString());
}

registerReducer(
  "__init",
  (args: Array<JsonValue>): JsonValue | null => {
    // Seed each table so subscriptions get a valid initial_state snapshot
    // before the first real event. Clients skip "__*" keys.
    writeTable("presence", "__seed",
      JsonValue.newObject().setString("user", "__seed").toString());
    writeTable("cursors", "__seed",
      JsonValue.newObject().setString("user", "__seed").toString());
    writeTable("notes", "__seed",
      JsonValue.newObject().setString("id", "__seed").toString());
    writeTable("activity", "__seed",
      JsonValue.newObject().setString("id", "__seed").toString());
    logInfo("collab board initialized");
    return null;
  }
);

registerReducer(
  "join",
  (args: Array<JsonValue>): JsonValue | null => {
    const user = requireString(args, 0, "user");
    const color = requireString(args, 1, "color");
    const row = JsonValue.newObject()
      .setString("user", user)
      .setString("color", color)
      .setNumber("joined_at", <f64>nowMs());
    writeTable("presence", user, row.toString());
    logActivity(user, color, "joined", "");
    return JsonValue.newObject().setString("user", user);
  },
  ["user", "color"]
);

registerReducer(
  "moveCursor",
  (args: Array<JsonValue>): JsonValue | null => {
    const user = requireString(args, 0, "user");
    const color = requireString(args, 1, "color");
    const x = args.length > 2 ? args[2].asNumber() : 0;
    const y = args.length > 3 ? args[3].asNumber() : 0;
    const row = JsonValue.newObject()
      .setString("user", user)
      .setString("color", color)
      .setNumber("x", x)
      .setNumber("y", y)
      .setNumber("ts", <f64>nowMs());
    writeTable("cursors", user, row.toString());
    return null;
  },
  ["user", "color", "x", "y"]
);

registerReducer(
  "addNote",
  (args: Array<JsonValue>): JsonValue | null => {
    const user = requireString(args, 0, "user");
    const color = requireString(args, 1, "color");
    const x = args.length > 2 ? args[2].asNumber() : 0.5;
    const y = args.length > 3 ? args[3].asNumber() : 0.5;
    const text = requireString(args, 4, "text");
    if (text.length > 280) {
      abortCall("note text exceeds 280 characters");
    }

    const id = generateId().toString();
    const row = JsonValue.newObject()
      .setString("id", id)
      .setString("user", user)
      .setString("color", color)
      .setNumber("x", x)
      .setNumber("y", y)
      .setString("text", text)
      .setNumber("created_at", <f64>nowMs());
    writeTable("notes", id, row.toString());
    logActivity(user, color, "pinned", text.length > 40 ? text.substring(0, 40) + "…" : text);
    return JsonValue.newObject().setString("id", id);
  },
  ["user", "color", "x", "y", "text"]
);

registerReducer(
  "removeNote",
  (args: Array<JsonValue>): JsonValue | null => {
    const id = requireString(args, 0, "id");
    const user = args.length > 1 ? args[1].asString() : "someone";
    if (id.startsWith("__")) {
      abortCall("cannot remove seed rows");
    }
    deleteTable("notes", id);
    logActivity(user, "", "removed a note", "");
    return null;
  },
  ["id", "user"]
);

registerReducer(
  "clearBoard",
  (args: Array<JsonValue>): JsonValue | null => {
    const user = args.length > 0 ? args[0].asString() : "someone";
    const rows = JsonValue.parse(scanTable("notes"));
    let cleared = 0;
    for (let i = 0; i < rows.length; i++) {
      const key = rows.at(i).getString("key");
      if (key.length == 0 || key.startsWith("__")) continue;
      deleteTable("notes", key);
      cleared++;
    }
    logActivity(user, "", "cleared the board", cleared.toString() + " notes");
    return JsonValue.newObject().setNumber("cleared", <f64>cleared);
  },
  ["user"]
);
