// =============================================================================
// OriginDB AssemblyScript SDK
//
// Implements the OriginDB WASM ABI v1 (docs/WASM_ABI.md):
//   * the guest exports origindb_alloc / origindb_free / origindb_describe /
//     origindb_invoke (re-export them from your module's entry file!)
//   * typed wrappers over the "env" host imports
//   * a reducer/filter registry that origindb_invoke dispatches through
//     (unregistered names return -404 per the ABI)
//   * a hand-rolled minimal JSON value type (JsonValue) — chosen over json-as
//     to keep the SDK dependency-free and predictable under the "minimal"
//     runtime
//
// Module entry files must re-export the ABI surface:
//
//   export {
//     origindb_alloc, origindb_free, origindb_describe, origindb_invoke,
//     __origindb_abort,
//   } from "<path>/assembly/index";
//
// and register reducers in top-level statements (they run in the wasm start
// section, i.e. at instantiation — before the host calls anything):
//
//   setModuleInfo("todo", "1.0.0");
//   registerReducer("addTodo", (args) => { ... }, ["text"]);
//
// Do not perform table operations at top level — use an "__init" reducer.
// =============================================================================

import * as env from "./env";

// =============================================================================
// Minimal JSON
// =============================================================================

export enum JsonType {
  Null = 0,
  Bool = 1,
  Number = 2,
  String = 3,
  Array = 4,
  Object = 5,
}

/** A parsed JSON value. Construct with the JsonValue.new* factories. */
export class JsonValue {
  kind: JsonType;
  private boolVal: bool = false;
  private numVal: f64 = 0;
  private strVal: string = "";
  arr: Array<JsonValue> = new Array<JsonValue>();
  private keys: Array<string> = new Array<string>();
  private map: Map<string, JsonValue> = new Map<string, JsonValue>();

  private constructor(kind: JsonType) {
    this.kind = kind;
  }

  static newNull(): JsonValue {
    return new JsonValue(JsonType.Null);
  }

  static newBool(v: bool): JsonValue {
    const j = new JsonValue(JsonType.Bool);
    j.boolVal = v;
    return j;
  }

  static newNumber(v: f64): JsonValue {
    const j = new JsonValue(JsonType.Number);
    j.numVal = v;
    return j;
  }

  static newString(v: string): JsonValue {
    const j = new JsonValue(JsonType.String);
    j.strVal = v;
    return j;
  }

  static newArray(): JsonValue {
    return new JsonValue(JsonType.Array);
  }

  static newObject(): JsonValue {
    return new JsonValue(JsonType.Object);
  }

  // ---- accessors ------------------------------------------------------------

  isNull(): bool {
    return this.kind == JsonType.Null;
  }

  asBool(): bool {
    return this.kind == JsonType.Bool ? this.boolVal : false;
  }

  asNumber(): f64 {
    return this.kind == JsonType.Number ? this.numVal : 0;
  }

  asInt(): i64 {
    return <i64>this.asNumber();
  }

  asString(): string {
    return this.kind == JsonType.String ? this.strVal : "";
  }

  /** Array length (0 for non-arrays). */
  get length(): i32 {
    return this.kind == JsonType.Array ? this.arr.length : 0;
  }

  /** Array element, or a null value when out of bounds. */
  at(index: i32): JsonValue {
    if (this.kind != JsonType.Array || index < 0 || index >= this.arr.length) {
      return JsonValue.newNull();
    }
    return this.arr[index];
  }

  has(key: string): bool {
    return this.kind == JsonType.Object && this.map.has(key);
  }

  /** Object member, or a null value when missing. */
  get(key: string): JsonValue {
    if (this.kind == JsonType.Object && this.map.has(key)) {
      return this.map.get(key);
    }
    return JsonValue.newNull();
  }

  getString(key: string, fallback: string = ""): string {
    const v = this.get(key);
    return v.kind == JsonType.String ? v.asString() : fallback;
  }

  getNumber(key: string, fallback: f64 = 0): f64 {
    const v = this.get(key);
    return v.kind == JsonType.Number ? v.asNumber() : fallback;
  }

  getBool(key: string, fallback: bool = false): bool {
    const v = this.get(key);
    return v.kind == JsonType.Bool ? v.asBool() : fallback;
  }

  // ---- builders ---------------------------------------------------------------

  set(key: string, value: JsonValue): JsonValue {
    if (this.kind == JsonType.Object) {
      if (!this.map.has(key)) this.keys.push(key);
      this.map.set(key, value);
    }
    return this;
  }

  setString(key: string, value: string): JsonValue {
    return this.set(key, JsonValue.newString(value));
  }

  setNumber(key: string, value: f64): JsonValue {
    return this.set(key, JsonValue.newNumber(value));
  }

  setBool(key: string, value: bool): JsonValue {
    return this.set(key, JsonValue.newBool(value));
  }

  setNull(key: string): JsonValue {
    return this.set(key, JsonValue.newNull());
  }

  push(value: JsonValue): JsonValue {
    if (this.kind == JsonType.Array) this.arr.push(value);
    return this;
  }

  // ---- serialization ------------------------------------------------------------

  toString(): string {
    switch (this.kind) {
      case JsonType.Null:
        return "null";
      case JsonType.Bool:
        return this.boolVal ? "true" : "false";
      case JsonType.Number: {
        const n = this.numVal;
        if (isNaN(n) || !isFinite(n)) return "null";
        // Integral values print without a trailing ".0".
        if (n == Math.trunc(n) && Math.abs(n) < 9007199254740992.0) {
          return (<i64>n).toString();
        }
        return n.toString();
      }
      case JsonType.String:
        return JsonValue.quote(this.strVal);
      case JsonType.Array: {
        let out = "[";
        for (let i = 0; i < this.arr.length; i++) {
          if (i > 0) out += ",";
          out += this.arr[i].toString();
        }
        return out + "]";
      }
      case JsonType.Object: {
        let out = "{";
        for (let i = 0; i < this.keys.length; i++) {
          if (i > 0) out += ",";
          const key = this.keys[i];
          out += JsonValue.quote(key) + ":" + this.map.get(key).toString();
        }
        return out + "}";
      }
      default:
        return "null";
    }
  }

  private static quote(s: string): string {
    let out = '"';
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c == 0x22) out += '\\"';
      else if (c == 0x5c) out += "\\\\";
      else if (c == 0x08) out += "\\b";
      else if (c == 0x0c) out += "\\f";
      else if (c == 0x0a) out += "\\n";
      else if (c == 0x0d) out += "\\r";
      else if (c == 0x09) out += "\\t";
      else if (c < 0x20) {
        const hex = c.toString(16);
        out += "\\u0000".slice(0, 6 - hex.length) + hex;
      } else {
        out += String.fromCharCode(c);
      }
    }
    return out + '"';
  }

  // ---- parsing ---------------------------------------------------------------

  /** Parse JSON. Returns null on malformed input. */
  static tryParse(text: string): JsonValue | null {
    const p = new JsonParser(text);
    const v = p.parseValue();
    if (v === null) return null;
    p.skipWs();
    if (p.pos != text.length) return null; // trailing garbage
    return v;
  }

  /** Parse JSON; aborts the current call on malformed input. */
  static parse(text: string): JsonValue {
    const v = JsonValue.tryParse(text);
    if (v === null) {
      abortCall("JsonValue.parse: malformed JSON");
    }
    return v!;
  }
}

class JsonParser {
  text: string;
  pos: i32 = 0;

  constructor(text: string) {
    this.text = text;
  }

  skipWs(): void {
    while (this.pos < this.text.length) {
      const c = this.text.charCodeAt(this.pos);
      if (c == 0x20 || c == 0x09 || c == 0x0a || c == 0x0d) this.pos++;
      else break;
    }
  }

  parseValue(): JsonValue | null {
    this.skipWs();
    if (this.pos >= this.text.length) return null;
    const c = this.text.charCodeAt(this.pos);
    if (c == 0x7b) return this.parseObject(); // {
    if (c == 0x5b) return this.parseArray(); // [
    if (c == 0x22) {
      // "
      const s = this.parseString();
      return s === null ? null : JsonValue.newString(s!);
    }
    if (c == 0x74) return this.literal("true") ? JsonValue.newBool(true) : null;
    if (c == 0x66) return this.literal("false") ? JsonValue.newBool(false) : null;
    if (c == 0x6e) return this.literal("null") ? JsonValue.newNull() : null;
    return this.parseNumber();
  }

  private literal(word: string): bool {
    if (this.pos + word.length > this.text.length) return false;
    for (let i = 0; i < word.length; i++) {
      if (this.text.charCodeAt(this.pos + i) != word.charCodeAt(i)) return false;
    }
    this.pos += word.length;
    return true;
  }

  private parseNumber(): JsonValue | null {
    const start = this.pos;
    if (this.pos < this.text.length && this.text.charCodeAt(this.pos) == 0x2d) this.pos++; // -
    let digits = false;
    while (this.pos < this.text.length) {
      const c = this.text.charCodeAt(this.pos);
      if (c >= 0x30 && c <= 0x39) {
        digits = true;
        this.pos++;
      } else break;
    }
    if (!digits) return null;
    if (this.pos < this.text.length && this.text.charCodeAt(this.pos) == 0x2e) {
      // .
      this.pos++;
      let frac = false;
      while (this.pos < this.text.length) {
        const c = this.text.charCodeAt(this.pos);
        if (c >= 0x30 && c <= 0x39) {
          frac = true;
          this.pos++;
        } else break;
      }
      if (!frac) return null;
    }
    if (this.pos < this.text.length) {
      const c = this.text.charCodeAt(this.pos);
      if (c == 0x65 || c == 0x45) {
        // e | E
        this.pos++;
        if (this.pos < this.text.length) {
          const s = this.text.charCodeAt(this.pos);
          if (s == 0x2b || s == 0x2d) this.pos++;
        }
        let exp = false;
        while (this.pos < this.text.length) {
          const c2 = this.text.charCodeAt(this.pos);
          if (c2 >= 0x30 && c2 <= 0x39) {
            exp = true;
            this.pos++;
          } else break;
        }
        if (!exp) return null;
      }
    }
    return JsonValue.newNumber(parseFloat(this.text.substring(start, this.pos)));
  }

  private parseString(): string | null {
    // caller ensured charCodeAt(pos) == '"'
    this.pos++;
    let out = "";
    while (this.pos < this.text.length) {
      const c = this.text.charCodeAt(this.pos);
      if (c == 0x22) {
        // closing "
        this.pos++;
        return out;
      }
      if (c == 0x5c) {
        // backslash
        this.pos++;
        if (this.pos >= this.text.length) return null;
        const e = this.text.charCodeAt(this.pos);
        if (e == 0x22) out += '"';
        else if (e == 0x5c) out += "\\";
        else if (e == 0x2f) out += "/";
        else if (e == 0x62) out += "\b";
        else if (e == 0x66) out += "\f";
        else if (e == 0x6e) out += "\n";
        else if (e == 0x72) out += "\r";
        else if (e == 0x74) out += "\t";
        else if (e == 0x75) {
          // \uXXXX
          if (this.pos + 4 >= this.text.length) return null;
          let code = 0;
          for (let i = 1; i <= 4; i++) {
            const h = this.text.charCodeAt(this.pos + i);
            let d = 0;
            if (h >= 0x30 && h <= 0x39) d = h - 0x30;
            else if (h >= 0x61 && h <= 0x66) d = h - 0x61 + 10;
            else if (h >= 0x41 && h <= 0x46) d = h - 0x41 + 10;
            else return null;
            code = (code << 4) | d;
          }
          this.pos += 4;
          out += String.fromCharCode(code); // surrogate pairs pass through
        } else {
          return null;
        }
        this.pos++;
      } else {
        out += String.fromCharCode(c);
        this.pos++;
      }
    }
    return null; // unterminated
  }

  private parseArray(): JsonValue | null {
    this.pos++; // [
    const arr = JsonValue.newArray();
    this.skipWs();
    if (this.pos < this.text.length && this.text.charCodeAt(this.pos) == 0x5d) {
      this.pos++;
      return arr;
    }
    while (true) {
      const v = this.parseValue();
      if (v === null) return null;
      arr.push(v!);
      this.skipWs();
      if (this.pos >= this.text.length) return null;
      const c = this.text.charCodeAt(this.pos);
      if (c == 0x2c) {
        // ,
        this.pos++;
        continue;
      }
      if (c == 0x5d) {
        // ]
        this.pos++;
        return arr;
      }
      return null;
    }
  }

  private parseObject(): JsonValue | null {
    this.pos++; // {
    const obj = JsonValue.newObject();
    this.skipWs();
    if (this.pos < this.text.length && this.text.charCodeAt(this.pos) == 0x7d) {
      this.pos++;
      return obj;
    }
    while (true) {
      this.skipWs();
      if (this.pos >= this.text.length || this.text.charCodeAt(this.pos) != 0x22) return null;
      const key = this.parseString();
      if (key === null) return null;
      this.skipWs();
      if (this.pos >= this.text.length || this.text.charCodeAt(this.pos) != 0x3a) return null; // :
      this.pos++;
      const v = this.parseValue();
      if (v === null) return null;
      obj.set(key!, v!);
      this.skipWs();
      if (this.pos >= this.text.length) return null;
      const c = this.text.charCodeAt(this.pos);
      if (c == 0x2c) {
        // ,
        this.pos++;
        continue;
      }
      if (c == 0x7d) {
        // }
        this.pos++;
        return obj;
      }
      return null;
    }
  }
}

// =============================================================================
// UTF-8 / memory helpers
// =============================================================================

// NUL-terminated UTF-8 copy (for table/topic/message pointers).
function cstr(s: string): ArrayBuffer {
  return String.UTF8.encode(s, true);
}

// Non-terminated UTF-8 copy (for keys/values, ptr+len style).
function utf8(s: string): ArrayBuffer {
  return String.UTF8.encode(s, false);
}

function ptrOf(buf: ArrayBuffer): usize {
  return changetype<usize>(buf);
}

// Decode + free a guest buffer that the host filled via our origindb_alloc
// (heap.alloc — same allocator, NOT host_free).
function takeGuestBuffer(ptr: usize, len: i32): string {
  if (ptr == 0) return "";
  const s = len > 0 ? String.UTF8.decodeUnsafe(ptr, <usize>len) : "";
  heap.free(ptr);
  return s;
}

// =============================================================================
// High-level host API
// =============================================================================

/** Wall clock in ms — fixed for the duration of the current call. */
export function nowMs(): i64 {
  return env.host_now_ms();
}

/** Unique id: (now_ms << 20) | counter. */
export function generateId(): i64 {
  return env.host_generate_id();
}

/**
 * The caller's server-set identity, derived from the authenticated connection.
 * Empty string when anonymous. Use THIS for authorization (row ownership,
 * admin checks) — never trust an identity passed as a reducer argument.
 */
export function senderIdentity(): string {
  const out = heap.alloc(8);
  store<u32>(out, 0);
  store<u32>(out + 4, 0);
  const rc = env.host_sender(out, out + 4);
  const bufPtr = <usize>load<u32>(out);
  const bufLen = <i32>load<u32>(out + 4);
  heap.free(out);
  if (rc <= 0) return "";
  return takeGuestBuffer(bufPtr, bufLen);
}

export function log(level: i32, message: string): void {
  const m = cstr(message);
  env.host_log(level, ptrOf(m));
}

export function logTrace(message: string): void {
  log(0, message);
}
export function logDebug(message: string): void {
  log(1, message);
}
export function logInfo(message: string): void {
  log(2, message);
}
export function logWarn(message: string): void {
  log(3, message);
}
export function logError(message: string): void {
  log(4, message);
}

/**
 * Fail the current call: records the message via host_abort and traps.
 * All staged writes/events are discarded. Never returns.
 */
export function abortCall(message: string): void {
  const m = cstr(message);
  env.host_abort(ptrOf(m));
  unreachable(); // host_abort traps; never reached
}

/**
 * Read a row. Returns the row's JSON object string, or null if not found.
 * Sees the current call's own staged writes first.
 */
export function readTable(table: string, key: string): string | null {
  const t = cstr(table);
  const k = utf8(key);
  const out = heap.alloc(8);
  store<u32>(out, 0);
  store<u32>(out + 4, 0);
  const rc = env.host_table_read(ptrOf(t), ptrOf(k), k.byteLength, out, out + 4);
  const bufPtr = <usize>load<u32>(out);
  const bufLen = <i32>load<u32>(out + 4);
  heap.free(out);
  if (rc < 0) {
    logError("host_table_read('" + table + "') failed with status " + rc.toString());
    return null;
  }
  if (rc == 0) return null;
  return takeGuestBuffer(bufPtr, bufLen);
}

/** Stage an upsert. `jsonValue` must be a JSON object string (column -> value). */
export function writeTable(table: string, key: string, jsonValue: string): bool {
  const t = cstr(table);
  const k = utf8(key);
  const v = utf8(jsonValue);
  const rc = env.host_table_write(ptrOf(t), ptrOf(k), k.byteLength, ptrOf(v), v.byteLength);
  if (rc < 0) {
    logError("host_table_write('" + table + "') failed with status " + rc.toString());
    return false;
  }
  return true;
}

/** Stage a delete. */
export function deleteTable(table: string, key: string): bool {
  const t = cstr(table);
  const k = utf8(key);
  const rc = env.host_table_delete(ptrOf(t), ptrOf(k), k.byteLength);
  if (rc < 0) {
    logError("host_table_delete('" + table + "') failed with status " + rc.toString());
    return false;
  }
  return true;
}

/**
 * Prefix scan (committed rows merged with staged writes). Returns a JSON
 * array string: [{"key": str, "value": obj}, ...], key-ordered, at most
 * `limit` rows (limit <= 0 -> 1000). Returns "[]" on error.
 */
export function scanTable(table: string, prefix: string = "", limit: i32 = 0): string {
  const t = cstr(table);
  const p = utf8(prefix);
  const out = heap.alloc(8);
  store<u32>(out, 0);
  store<u32>(out + 4, 0);
  const rc = env.host_table_scan(ptrOf(t), ptrOf(p), p.byteLength, limit, out, out + 4);
  const bufPtr = <usize>load<u32>(out);
  const bufLen = <i32>load<u32>(out + 4);
  heap.free(out);
  if (rc < 0) {
    logError("host_table_scan('" + table + "') failed with status " + rc.toString());
    return "[]";
  }
  const json = takeGuestBuffer(bufPtr, bufLen);
  return json.length > 0 ? json : "[]";
}

/**
 * Stage a custom changefeed event (operation "EVENT"), published only after
 * a successful commit. `payloadJson` must be JSON.
 */
export function emitEvent(topic: string, key: string, payloadJson: string): bool {
  const t = cstr(topic);
  const k = utf8(key);
  const p = utf8(payloadJson);
  const rc = env.host_emit_event(ptrOf(t), ptrOf(k), k.byteLength, ptrOf(p), p.byteLength);
  if (rc < 0) {
    logError("host_emit_event('" + topic + "') failed with status " + rc.toString());
    return false;
  }
  return true;
}

/**
 * Set the call's result payload (max 16 MiB, last call wins). The dispatcher
 * calls this automatically for non-null reducer return values.
 */
export function setResult(json: string): void {
  const b = utf8(json);
  env.host_set_result(ptrOf(b), b.byteLength);
}

// =============================================================================
// Binary arguments — {"$bytes": "<base64>"} per the ABI
// =============================================================================

const B64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

export function base64Encode(bytes: Uint8Array): string {
  let out = "";
  let i = 0;
  const n = bytes.length;
  while (i + 2 < n) {
    const x = (<u32>bytes[i] << 16) | (<u32>bytes[i + 1] << 8) | <u32>bytes[i + 2];
    out += B64_ALPHABET.charAt(<i32>((x >> 18) & 63));
    out += B64_ALPHABET.charAt(<i32>((x >> 12) & 63));
    out += B64_ALPHABET.charAt(<i32>((x >> 6) & 63));
    out += B64_ALPHABET.charAt(<i32>(x & 63));
    i += 3;
  }
  const rem = n - i;
  if (rem == 1) {
    const x = <u32>bytes[i] << 16;
    out += B64_ALPHABET.charAt(<i32>((x >> 18) & 63));
    out += B64_ALPHABET.charAt(<i32>((x >> 12) & 63));
    out += "==";
  } else if (rem == 2) {
    const x = (<u32>bytes[i] << 16) | (<u32>bytes[i + 1] << 8);
    out += B64_ALPHABET.charAt(<i32>((x >> 18) & 63));
    out += B64_ALPHABET.charAt(<i32>((x >> 12) & 63));
    out += B64_ALPHABET.charAt(<i32>((x >> 6) & 63));
    out += "=";
  }
  return out;
}

export function base64Decode(s: string): Uint8Array {
  let len = s.length;
  while (len > 0 && s.charCodeAt(len - 1) == 0x3d) len--; // strip '='
  const outLen = (len * 3) / 4;
  const out = new Uint8Array(outLen);
  let acc: u32 = 0;
  let bits = 0;
  let o = 0;
  for (let i = 0; i < len; i++) {
    const c = s.charCodeAt(i);
    let d = -1;
    if (c >= 0x41 && c <= 0x5a) d = c - 0x41; // A-Z
    else if (c >= 0x61 && c <= 0x7a) d = c - 0x61 + 26; // a-z
    else if (c >= 0x30 && c <= 0x39) d = c - 0x30 + 52; // 0-9
    else if (c == 0x2b) d = 62; // +
    else if (c == 0x2f) d = 63; // /
    else continue; // skip whitespace/invalid
    acc = (acc << 6) | <u32>d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o < outLen) out[o++] = <u8>((acc >> bits) & 0xff);
    }
  }
  return out;
}

/** Extract binary data from a {"$bytes": "<base64>"} argument, or null. */
export function bytesArg(value: JsonValue): Uint8Array | null {
  if (value.kind == JsonType.Object && value.has("$bytes")) {
    return base64Decode(value.get("$bytes").asString());
  }
  return null;
}

/** Wrap binary data as a {"$bytes": "<base64>"} JSON value. */
export function bytesValue(bytes: Uint8Array): JsonValue {
  return JsonValue.newObject().setString("$bytes", base64Encode(bytes));
}

// =============================================================================
// Reducer registry
// =============================================================================

export type ReducerFn = (args: Array<JsonValue>) => JsonValue | null;
export type FilterFn = (event: JsonValue) => bool;

class ReducerEntry {
  fn: ReducerFn;
  params: Array<string>;

  constructor(fn: ReducerFn, params: Array<string>) {
    this.fn = fn;
    this.params = params;
  }
}

const reducerNames = new Array<string>();
const reducers = new Map<string, ReducerEntry>();
const filterNames = new Array<string>();
const filters = new Map<string, FilterFn>();
const declaredTables = new Array<string>();
let moduleName = "module";
let moduleVersion = "1.0.0";

/** Module name/version reported by origindb_describe. */
export function setModuleInfo(name: string, version: string = "1.0.0"): void {
  moduleName = name;
  moduleVersion = version;
}

/** Declare a table for origindb_describe metadata (informational). */
export function declareTable(table: string): void {
  if (!declaredTables.includes(table)) declaredTables.push(table);
}

/**
 * Register a reducer. `fn` receives the invocation's JSON args array. A
 * non-null return value is serialized and returned via host_set_result; the
 * call returns status 0. Call abortCall(...) to fail the call (discarding
 * staged writes). Reserved lifecycle names: __init, __client_connected,
 * __client_disconnected, __get_initial_data.
 */
export function registerReducer(name: string, fn: ReducerFn, params: Array<string> = []): void {
  if (!reducers.has(name)) reducerNames.push(name);
  reducers.set(name, new ReducerEntry(fn, params));
}

/**
 * Register a subscription filter. The filter receives the changefeed event
 * as a parsed JSON object: {table, operation, offset, transaction_id, key,
 * new_value, old_value}. Return true to include the event, false to exclude
 * it (ABI status 1 / 0).
 */
export function registerFilter(name: string, fn: FilterFn): void {
  if (!filters.has(name)) filterNames.push(name);
  filters.set(name, fn);
}

function buildDescribeJson(): string {
  const root = JsonValue.newObject();
  root.setString("name", moduleName);
  root.setString("version", moduleVersion);
  const reds = JsonValue.newArray();
  for (let i = 0; i < reducerNames.length; i++) {
    const name = reducerNames[i];
    const entry = reducers.get(name);
    const r = JsonValue.newObject();
    r.setString("name", name);
    const ps = JsonValue.newArray();
    for (let j = 0; j < entry.params.length; j++) {
      ps.push(JsonValue.newString(entry.params[j]));
    }
    r.set("params", ps);
    reds.push(r);
  }
  for (let i = 0; i < filterNames.length; i++) {
    const r = JsonValue.newObject();
    r.setString("name", filterNames[i]);
    const ps = JsonValue.newArray();
    ps.push(JsonValue.newString("event"));
    r.set("params", ps);
    reds.push(r);
  }
  root.set("reducers", reds);
  const tabs = JsonValue.newArray();
  for (let i = 0; i < declaredTables.length; i++) {
    tabs.push(JsonValue.newString(declaredTables[i]));
  }
  root.set("tables", tabs);
  return root.toString();
}

// Filters receive one argument: the changefeed event. The host passes it as
// a JSON *string* containing the event object, so unwrap one level.
function unwrapEvent(args: Array<JsonValue>): JsonValue {
  if (args.length == 0) return JsonValue.newNull();
  const first = args[0];
  if (first.kind == JsonType.String) {
    const inner = JsonValue.tryParse(first.asString());
    if (inner !== null) return inner!;
  }
  return first;
}

// =============================================================================
// ABI exports — re-export these from your module's entry file.
// =============================================================================

/**
 * Guest allocator. The host uses it for every host->guest buffer (invoke
 * name/args, read/scan results). Buffers are owned by the guest and freed
 * with heap.free / origindb_free. Returns 0 on failure per the ABI
 * (allocation failure traps in AS, which the host also treats as an error).
 */
export function origindb_alloc(size: i32): usize {
  return heap.alloc(<usize>(size > 0 ? size : 1));
}

export function origindb_free(ptr: usize): void {
  if (ptr != 0) heap.free(ptr);
}

// Pins the most recent describe blob so it stays valid after returning.
let describePin: ArrayBuffer | null = null;

/** Returns (ptr << 32) | len of the module metadata JSON. */
export function origindb_describe(): i64 {
  const buf = utf8(buildDescribeJson());
  describePin = buf;
  return (<i64>ptrOf(buf) << 32) | (<i64>buf.byteLength & 0xffffffff);
}

/**
 * Single dispatch entry point for reducers, lifecycle hooks and subscription
 * filters. Status: 0 ok, 1/0 filter include/exclude, <0 error, -404 no
 * handler registered for this name.
 */
export function origindb_invoke(namePtr: usize, nameLen: i32, argsPtr: usize, argsLen: i32): i32 {
  const name = nameLen > 0 ? String.UTF8.decodeUnsafe(namePtr, <usize>nameLen) : "";
  const argsJson = argsLen > 0 ? String.UTF8.decodeUnsafe(argsPtr, <usize>argsLen) : "[]";

  // The host wrote name/args via origindb_alloc; the guest owns (and now
  // frees) those buffers.
  if (namePtr != 0) heap.free(namePtr);
  if (argsPtr != 0) heap.free(argsPtr);

  if (reducers.has(name)) {
    const parsed = JsonValue.tryParse(argsJson);
    const args =
      parsed !== null && parsed!.kind == JsonType.Array ? parsed!.arr : new Array<JsonValue>();
    const result = reducers.get(name).fn(args);
    if (result !== null) setResult(result!.toString());
    return 0;
  }

  if (filters.has(name)) {
    const parsed = JsonValue.tryParse(argsJson);
    const args =
      parsed !== null && parsed!.kind == JsonType.Array ? parsed!.arr : new Array<JsonValue>();
    return filters.get(name)(unwrapEvent(args)) ? 1 : 0;
  }

  return -404;
}

/**
 * Custom AssemblyScript abort handler, wired via asconfig
 * ("use": ["abort=assembly/index/__origindb_abort"]). Routes assertion
 * failures and `throw` to host_abort so the failure message reaches the
 * server log instead of requiring an env.abort import the host rejects.
 */
export function __origindb_abort(
  message: string | null,
  fileName: string | null,
  lineNumber: u32,
  columnNumber: u32
): void {
  let msg = message !== null ? message! : "abort";
  if (fileName !== null) {
    msg += " (" + fileName! + ":" + lineNumber.toString() + ":" + columnNumber.toString() + ")";
  }
  const m = String.UTF8.encode(msg, true);
  env.host_abort(changetype<usize>(m));
  unreachable();
}
