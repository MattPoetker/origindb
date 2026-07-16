// =============================================================================
// Todo — example InstantDB module in AssemblyScript.
//
// Reducers:
//   addTodo(text)     -> {"id": "..."}
//   listTodos()       -> [ {todo row}, ... ]
//   completeTodo(id)  -> {"id": "...", "done": true}
// Filter:
//   onlyPending       -> forwards only events whose new value is not done
//
// Build:  npm run asbuild   (from sdk/typescript)
// Output: build/module.wasm
// =============================================================================

import {
  JsonValue,
  JsonType,
  abortCall,
  declareTable,
  generateId,
  logInfo,
  nowMs,
  readTable,
  registerFilter,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "../../assembly/index";

// Every module entry file must re-export the ABI surface implemented by the SDK.
export {
  instantdb_alloc,
  instantdb_free,
  instantdb_describe,
  instantdb_invoke,
  __instantdb_abort,
} from "../../assembly/index";

// Top-level statements run in the wasm start section — at instantiation,
// before the host invokes anything. Register everything here; do NOT touch
// tables here (use an "__init" reducer for that).
setModuleInfo("todo", "1.0.0");
declareTable("todos");

registerReducer(
  "addTodo",
  (args: Array<JsonValue>): JsonValue | null => {
    const text = args.length > 0 ? args[0].asString() : "";
    if (text.length == 0) {
      abortCall("addTodo: 'text' must be a non-empty string");
    }

    const id = generateId().toString();
    const row = JsonValue.newObject()
      .setString("id", id)
      .setString("text", text)
      .setBool("done", false)
      .setNumber("created_at", <f64>nowMs());
    writeTable("todos", id, row.toString());

    logInfo("added todo " + id);
    return JsonValue.newObject().setString("id", id);
  },
  ["text"]
);

registerReducer("listTodos", (args: Array<JsonValue>): JsonValue | null => {
  // scanTable returns [{"key": str, "value": obj}, ...]; project the rows.
  const rows = JsonValue.parse(scanTable("todos"));
  const todos = JsonValue.newArray();
  for (let i = 0; i < rows.length; i++) {
    todos.push(rows.at(i).get("value"));
  }
  return todos;
});

registerReducer(
  "completeTodo",
  (args: Array<JsonValue>): JsonValue | null => {
    const id = args.length > 0 ? args[0].asString() : "";
    const json = readTable("todos", id);
    if (json === null) {
      abortCall("completeTodo: no todo with id '" + id + "'");
    }
    const row = JsonValue.parse(json!);
    row.setBool("done", true);
    row.setNumber("completed_at", <f64>nowMs());
    writeTable("todos", id, row.toString());
    return JsonValue.newObject().setString("id", id).setBool("done", true);
  },
  ["id"]
);

// Lifecycle hooks are ordinary registrations under reserved names.
registerReducer("__init", (args: Array<JsonValue>): JsonValue | null => {
  logInfo("todo module initialized");
  return null;
});

// Subscription filter: include only changefeed events whose new value is a
// not-yet-done todo. Event fields: table, operation, offset, transaction_id,
// key, new_value, old_value.
registerFilter("onlyPending", (event: JsonValue): bool => {
  if (event.kind != JsonType.Object) return true;
  let nv = event.get("new_value");
  if (nv.kind != JsonType.Object) return true;
  // Storage-emitted events wrap the row as {"key": ..., "columns": {...}}.
  const columns = nv.get("columns");
  if (columns.kind == JsonType.Object) nv = columns;
  return !nv.getBool("done", false);
});
