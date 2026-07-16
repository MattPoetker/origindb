#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include <cstdlib>
#include <sstream>

namespace fs = std::filesystem;

// ANSI color codes
const std::string RESET = "\033[0m";
const std::string BOLD = "\033[1m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
const std::string CYAN = "\033[36m";

// File templates
struct FileTemplate {
    std::string path;
    std::string content;
};

// Project templates
std::map<std::string, std::vector<FileTemplate>> templates = {
    {"csharp", {
        {"Program.cs", R"CSPROG(using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Text.Json;
using InstantDB;

namespace {{PROJECT_NAME}};

public static class Program
{
    // WASI command entry point (required by OutputType=Exe). Leave it empty —
    // the host may never call _start, so registration lives in Init() below.
    public static void Main() { }

    // Guaranteed to run before any SDK export executes, whether the host
    // calls _initialize, _start, or invokes an export directly.
    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("{{PROJECT_NAME_LOWER}}", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", CreateUser, "name", "email");
        Reducers.Register("GetUsers", GetUsers);

        // Subscription filter: only forward INSERT events on the users table.
        Reducers.RegisterFilter("OnlyUserInserts", ev =>
            ev.ValueKind == JsonValueKind.Object
            && ev.TryGetProperty("table", out JsonElement t) && t.GetString() == "users"
            && ev.TryGetProperty("operation", out JsonElement op) && op.GetString() == "INSERT");

        // Lifecycle hooks are ordinary registrations under reserved names.
        Reducers.Register("__init", _ =>
        {
            Host.LogInfo("{{PROJECT_NAME_LOWER}} module initialized");
            return null;
        });
    }

    /// <summary>CreateUser(name, email) — validates, writes a users row, returns {"id": id}.</summary>
    private static object? CreateUser(JsonElement[] args)
    {
        string name = RequireString(args, 0, "name");
        string email = RequireString(args, 1, "email");
        int at = email.IndexOf('@');
        if (at <= 0 || at == email.Length - 1)
            throw new ReducerException($"'{email}' is not a valid email address", -3);

        long id = Host.GenerateId();
        string key = id.ToString();
        var row = new Dictionary<string, object?>
        {
            ["id"] = key,
            ["name"] = name,
            ["email"] = email,
            ["created_at"] = Host.NowMs(),
        };
        Db.Write("users", key, JsonSerializer.Serialize(row));

        Host.LogInfo($"created user {key} ({name})");
        return new Dictionary<string, object?> { ["id"] = key };
    }

    /// <summary>GetUsers() — scans the users table and returns an array of rows.</summary>
    private static object? GetUsers(JsonElement[] args)
    {
        // Db.Scan returns [{"key": str, "value": obj}, ...]; project the rows.
        using JsonDocument doc = JsonDocument.Parse(Db.Scan("users"));
        var users = new List<JsonElement>();
        foreach (JsonElement entry in doc.RootElement.EnumerateArray())
        {
            if (entry.TryGetProperty("value", out JsonElement value))
                users.Add(value.Clone());
        }
        return users;
    }

    private static string RequireString(JsonElement[] args, int index, string name)
    {
        if (args.Length <= index || args[index].ValueKind != JsonValueKind.String)
            throw new ReducerException($"argument {index} ('{name}') must be a string", -3);
        string value = (args[index].GetString() ?? "").Trim();
        if (value.Length == 0)
            throw new ReducerException($"argument '{name}' must not be empty", -3);
        return value;
    }
}
)CSPROG"},
        {"{{PROJECT_NAME}}.csproj", R"CSPROJ(<Project Sdk="Microsoft.NET.Sdk">

  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <RootNamespace>{{PROJECT_NAME}}</RootNamespace>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <InvariantGlobalization>true</InvariantGlobalization>
    <!-- InstantDB runs core WASI p1 modules: net8.0 + wasi-wasm + the
         wasi-experimental workload. Do NOT upgrade to .NET 9+/componentize-dotnet:
         those emit WASI p2 components, which the server rejects. -->
    <RuntimeIdentifier>wasi-wasm</RuntimeIdentifier>
    <WasmSingleFileBundle>true</WasmSingleFileBundle>
  </PropertyGroup>

  <!-- The InstantDB C# SDK is a single source file (no NuGet package yet).
       Either copy sdk/csharp/InstantDB.cs from the InstantDB repository into
       this directory (it is compiled automatically), or point at your
       checkout: dotnet publish -c Release -p:InstantDBSdkDir=/path/to/instant_db/sdk/csharp -->
  <ItemGroup Condition="'$(InstantDBSdkDir)' != ''">
    <Compile Include="$(InstantDBSdkDir)/InstantDB.cs" />
  </ItemGroup>

</Project>
)CSPROJ"},
        {"global.json", R"CSGLOBAL({
  "sdk": {
    "version": "8.0.400",
    "rollForward": "latestFeature",
    "allowPrerelease": false
  }
}
)CSGLOBAL"},
        {"README.md", R"CSREADME(# {{PROJECT_NAME}}

An InstantDB WASM module written in C#. Reducers run inside the InstantDB
server with transactional table access (WASM ABI v1 — see docs/WASM_ABI.md
in the InstantDB repository).

## Toolchain

- .NET SDK **8.0.4xx** — pinned by `global.json`. Do not build with
  .NET 9+/componentize-dotnet: those emit WASI p2 *components*, which the
  server cannot run.
- The WASI workload: `dotnet workload install wasi-experimental`

## Setup

The SDK is a single source file (no NuGet package yet). Copy it into the
project directory:

```bash
cp /path/to/instant_db/sdk/csharp/InstantDB.cs .
```

(or build with `-p:InstantDBSdkDir=/path/to/instant_db/sdk/csharp`).

## Build & publish

```bash
./build.sh                       # or: dotnet publish -c Release
# → bin/Release/net8.0/wasi-wasm/AppBundle/{{PROJECT_NAME}}.wasm

instantdb publish --server=localhost:50051
```

## Call your reducers

```bash
instantdb client call {{PROJECT_NAME_LOWER}} CreateUser '["Alice", "alice@example.com"]'
instantdb client call {{PROJECT_NAME_LOWER}} GetUsers '[]'
```

## How it works

- `Program.cs` registers reducers with `Reducers.Register` from a
  `[ModuleInitializer]` method — it runs before any ABI export executes.
- A non-null reducer return value is serialized to JSON and returned via
  `host_set_result` (call status 0).
- Throw `ReducerException(msg, code)` to fail a call; all staged table
  writes and events are discarded on negative status.
- Table writes commit atomically when the call succeeds, and the changefeed
  notifies subscribed clients automatically.

See `sdk/csharp/README.md` in the InstantDB repository for the full API.
)CSREADME"},
        {".gitignore", R"CSGIT(bin/
obj/
.vs/
*.user
*.suo
.DS_Store
)CSGIT"},
        {"build.sh", R"CSBUILD(#!/bin/bash
set -e

echo "Building {{PROJECT_NAME}} WASM module..."

if ! command -v dotnet &> /dev/null; then
    echo "error: .NET SDK not found. Install .NET 8 (8.0.4xx)." >&2
    exit 1
fi

if [ ! -f "InstantDB.cs" ] && [ -z "$INSTANTDB_SDK_DIR" ]; then
    echo "warning: InstantDB.cs not found in this directory." >&2
    echo "  Copy it from the InstantDB repo:  cp /path/to/instant_db/sdk/csharp/InstantDB.cs ." >&2
    echo "  Or set INSTANTDB_SDK_DIR=/path/to/instant_db/sdk/csharp" >&2
fi

if ! dotnet workload list | grep -q "wasi-experimental"; then
    echo "Installing wasi-experimental workload..."
    dotnet workload install wasi-experimental
fi

EXTRA_ARGS=""
if [ -n "$INSTANTDB_SDK_DIR" ]; then
    EXTRA_ARGS="-p:InstantDBSdkDir=$INSTANTDB_SDK_DIR"
fi

dotnet publish --configuration Release $EXTRA_ARGS

WASM_FILE="bin/Release/net8.0/wasi-wasm/AppBundle/{{PROJECT_NAME}}.wasm"
if [ -f "$WASM_FILE" ]; then
    echo "WASM module built: $WASM_FILE ($(ls -lh "$WASM_FILE" | awk '{print $5}'))"
    echo "Deploy with: instantdb publish"
else
    echo "error: build succeeded but $WASM_FILE not found" >&2
    exit 1
fi
)CSBUILD"}
    }},

    // AssemblyScript module project. assembly/env.ts and assembly/index.ts
    // are verbatim copies of sdk/typescript/assembly/* — keep them in sync.
    {"typescript", {
        {"package.json", R"TSPKG({
  "name": "{{PROJECT_NAME_LOWER}}",
  "version": "1.0.0",
  "private": true,
  "description": "An InstantDB WASM module written in AssemblyScript",
  "type": "module",
  "scripts": {
    "asbuild": "asc --target release",
    "asbuild:debug": "asc --target debug",
    "build": "npm run asbuild"
  },
  "devDependencies": {
    "assemblyscript": "^0.27.0"
  }
}
)TSPKG"},
        {"asconfig.json", R"TSCONF({
  "entries": ["./assembly/module.ts"],
  "targets": {
    "debug": {
      "outFile": "build/module.debug.wasm",
      "textFile": "build/module.debug.wat",
      "sourceMap": false,
      "debug": true
    },
    "release": {
      "outFile": "build/module.wasm",
      "optimizeLevel": 3,
      "shrinkLevel": 1,
      "noAssert": false
    }
  },
  "options": {
    "runtime": "minimal",
    "exportRuntime": false,
    "importMemory": false,
    "use": ["abort=assembly/index/__instantdb_abort"]
  }
}
)TSCONF"},
        {"assembly/env.ts", R"TSENV(// =============================================================================
// InstantDB host imports (WASM ABI v1, module "env").
// See docs/WASM_ABI.md for the normative contract.
//
// All pointers are byte offsets into this module's exported linear memory
// (usize == u32 on wasm32). `table`, `topic` and message pointers are
// NUL-terminated UTF-8; keys/values/payloads are pointer+length.
// =============================================================================

// ---- tables -----------------------------------------------------------------

// 1 = found (result written to a buffer allocated via our instantdb_alloc;
// offset stored at *outPtr, length at *outLen), 0 = not found, <0 = error.
@external("env", "host_table_read")
export declare function host_table_read(
  table: usize, key: usize, keyLen: i32, outPtr: usize, outLen: usize): i32;

// Stages an upsert; value must be a UTF-8 JSON object. 0 = ok.
@external("env", "host_table_write")
export declare function host_table_write(
  table: usize, key: usize, keyLen: i32, value: usize, valueLen: i32): i32;

// Stages a delete. 0 = ok.
@external("env", "host_table_delete")
export declare function host_table_delete(
  table: usize, key: usize, keyLen: i32): i32;

// Prefix scan; result JSON array written via instantdb_alloc. Returns row
// count or <0. limit <= 0 -> 1000.
@external("env", "host_table_scan")
export declare function host_table_scan(
  table: usize, prefix: usize, prefixLen: i32, limit: i32,
  outPtr: usize, outLen: usize): i32;

// ---- events -----------------------------------------------------------------

// Stages a custom changefeed event (operation "EVENT").
@external("env", "host_emit_event")
export declare function host_emit_event(
  topic: usize, key: usize, keyLen: i32, payload: usize, payloadLen: i32): i32;

// ---- utilities ----------------------------------------------------------------

// Wall clock (ms), fixed for the duration of the current call.
@external("env", "host_now_ms")
export declare function host_now_ms(): i64;

// Unique id: (now_ms << 20) | counter.
@external("env", "host_generate_id")
export declare function host_generate_id(): i64;

// level 0..4 = trace/debug/info/warn/error; msg NUL-terminated (max 4 KiB).
@external("env", "host_log")
export declare function host_log(level: i32, msg: usize): void;

// Records the message and traps; the call fails and staged work is discarded.
@external("env", "host_abort")
export declare function host_abort(msg: usize): void;

// ---- memory / results ---------------------------------------------------------

// Host-managed scratch arena inside guest memory; valid until the call returns.
@external("env", "host_alloc")
export declare function host_alloc(size: i32): usize;

// No-op (arena reclaimed with the instance).
@external("env", "host_free")
export declare function host_free(ptr: usize): void;

// Copies the call's result payload to the host (max 16 MiB, last call wins).
@external("env", "host_set_result")
export declare function host_set_result(ptr: usize, len: i32): void;
)TSENV"},
        {"assembly/index.ts", R"TSSDK(// =============================================================================
// InstantDB AssemblyScript SDK
//
// Implements the InstantDB WASM ABI v1 (docs/WASM_ABI.md):
//   * the guest exports instantdb_alloc / instantdb_free / instantdb_describe /
//     instantdb_invoke (re-export them from your module's entry file!)
//   * typed wrappers over the "env" host imports
//   * a reducer/filter registry that instantdb_invoke dispatches through
//     (unregistered names return -404 per the ABI)
//   * a hand-rolled minimal JSON value type (JsonValue) — chosen over json-as
//     to keep the SDK dependency-free and predictable under the "minimal"
//     runtime
//
// Module entry files must re-export the ABI surface:
//
//   export {
//     instantdb_alloc, instantdb_free, instantdb_describe, instantdb_invoke,
//     __instantdb_abort,
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

// Decode + free a guest buffer that the host filled via our instantdb_alloc
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

/** Module name/version reported by instantdb_describe. */
export function setModuleInfo(name: string, version: string = "1.0.0"): void {
  moduleName = name;
  moduleVersion = version;
}

/** Declare a table for instantdb_describe metadata (informational). */
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
 * with heap.free / instantdb_free. Returns 0 on failure per the ABI
 * (allocation failure traps in AS, which the host also treats as an error).
 */
export function instantdb_alloc(size: i32): usize {
  return heap.alloc(<usize>(size > 0 ? size : 1));
}

export function instantdb_free(ptr: usize): void {
  if (ptr != 0) heap.free(ptr);
}

// Pins the most recent describe blob so it stays valid after returning.
let describePin: ArrayBuffer | null = null;

/** Returns (ptr << 32) | len of the module metadata JSON. */
export function instantdb_describe(): i64 {
  const buf = utf8(buildDescribeJson());
  describePin = buf;
  return (<i64>ptrOf(buf) << 32) | (<i64>buf.byteLength & 0xffffffff);
}

/**
 * Single dispatch entry point for reducers, lifecycle hooks and subscription
 * filters. Status: 0 ok, 1/0 filter include/exclude, <0 error, -404 no
 * handler registered for this name.
 */
export function instantdb_invoke(namePtr: usize, nameLen: i32, argsPtr: usize, argsLen: i32): i32 {
  const name = nameLen > 0 ? String.UTF8.decodeUnsafe(namePtr, <usize>nameLen) : "";
  const argsJson = argsLen > 0 ? String.UTF8.decodeUnsafe(argsPtr, <usize>argsLen) : "[]";

  // The host wrote name/args via instantdb_alloc; the guest owns (and now
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
 * ("use": ["abort=assembly/index/__instantdb_abort"]). Routes assertion
 * failures and `throw` to host_abort so the failure message reaches the
 * server log instead of requiring an env.abort import the host rejects.
 */
export function __instantdb_abort(
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
)TSSDK"},
        {"assembly/module.ts", R"TSMOD(// =============================================================================
// {{PROJECT_NAME}} — InstantDB WASM module (AssemblyScript).
//
// assembly/env.ts and assembly/index.ts are the InstantDB SDK (vendored from
// sdk/typescript in the InstantDB repository) — edit this file, not those.
//
// Build:   npm install && npm run asbuild   → build/module.wasm
// Publish: instantdb publish
// =============================================================================

import {
  JsonValue,
  JsonType,
  abortCall,
  declareTable,
  generateId,
  logInfo,
  nowMs,
  registerFilter,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "./index";

// REQUIRED: re-export the ABI surface implemented by the SDK.
export {
  instantdb_alloc,
  instantdb_free,
  instantdb_describe,
  instantdb_invoke,
  __instantdb_abort,
} from "./index";

// Top-level statements run at instantiation (wasm start section), before the
// host invokes anything. Register everything here; do not touch tables here —
// use the "__init" reducer for that.
setModuleInfo("{{PROJECT_NAME_LOWER}}", "1.0.0");
declareTable("users");

registerReducer(
  "CreateUser",
  (args: Array<JsonValue>): JsonValue | null => {
    const name = args.length > 0 ? args[0].asString() : "";
    const email = args.length > 1 ? args[1].asString() : "";
    if (name.length == 0) {
      abortCall("CreateUser: 'name' must be a non-empty string");
    }
    if (email.indexOf("@") <= 0) {
      abortCall("CreateUser: '" + email + "' is not a valid email address");
    }

    const id = generateId().toString();
    const row = JsonValue.newObject()
      .setString("id", id)
      .setString("name", name)
      .setString("email", email)
      .setNumber("created_at", <f64>nowMs());
    writeTable("users", id, row.toString());

    logInfo("created user " + id);
    return JsonValue.newObject().setString("id", id);
  },
  ["name", "email"]
);

registerReducer("GetUsers", (args: Array<JsonValue>): JsonValue | null => {
  // scanTable returns [{"key": str, "value": obj}, ...]; project the rows.
  const rows = JsonValue.parse(scanTable("users"));
  const users = JsonValue.newArray();
  for (let i = 0; i < rows.length; i++) {
    users.push(rows.at(i).get("value"));
  }
  return users;
});

// Lifecycle hooks are ordinary registrations under reserved names.
registerReducer("__init", (args: Array<JsonValue>): JsonValue | null => {
  logInfo("{{PROJECT_NAME_LOWER}} module initialized");
  return null;
});

// Subscription filter: only forward INSERT events on the users table.
registerFilter("OnlyUserInserts", (event: JsonValue): bool => {
  if (event.kind != JsonType.Object) return true;
  return event.getString("table") == "users" && event.getString("operation") == "INSERT";
});
)TSMOD"},
        {"README.md", R"TSREADME(# {{PROJECT_NAME}}

An InstantDB WASM module written in AssemblyScript (TypeScript-syntax
language compiled to WebAssembly). Reducers run inside the InstantDB server
with transactional table access (WASM ABI v1).

## Layout

- `assembly/module.ts` — **your module** (reducers, filters). Edit this.
- `assembly/index.ts`, `assembly/env.ts` — the InstantDB SDK, vendored from
  `sdk/typescript` in the InstantDB repository. Don't edit; re-copy to upgrade.

## Build & publish

```bash
npm install
npm run asbuild            # → build/module.wasm
instantdb publish --server=localhost:50051
```

## Call your reducers

```bash
instantdb client call {{PROJECT_NAME_LOWER}} CreateUser '["Alice", "alice@example.com"]'
instantdb client call {{PROJECT_NAME_LOWER}} GetUsers '[]'
```

## Notes

- AssemblyScript is a strict TS subset: no `any`/union types (except
  `T | null`), no closures over local variables, no `try`/`catch` (`throw`
  aborts the call via `host_abort`), and no `JSON` global — use the SDK's
  `JsonValue`.
- A non-null reducer return value is serialized and returned via
  `host_set_result` (status 0); `abortCall(msg)` fails the call and discards
  staged writes.
- See `sdk/typescript/README.md` in the InstantDB repository for the full
  API reference.
)TSREADME"},
        {".gitignore", R"TSGIT(node_modules/
build/
.DS_Store
)TSGIT"}
    }},

    {"unity", {
        {"Assets/Scripts/GameManager.cs", R"(using UnityEngine;
using InstantDB.Unity;
using InstantDB.Client;
using System.Threading.Tasks;

public class GameManager : MonoBehaviour
{
    [SerializeField] private InstantDBConfig config;
    private IInstantDBConnection connection;

    async void Start()
    {
        Debug.Log("Initializing InstantDB connection...");

        // Get or create network manager
        if (InstantDBNetworkManager.Instance == null)
        {
            var managerObj = new GameObject("InstantDB Network Manager");
            managerObj.AddComponent<InstantDBNetworkManager>();
        }

        // Create connection
        connection = InstantDBNetworkManager.Instance.CreateDefaultConnection();

        // Subscribe to events
        connection.OnConnected += OnConnected;
        connection.OnDisconnected += OnDisconnected;
        connection.OnPlayerInsert += OnPlayerJoined;
        connection.OnPlayerUpdate += OnPlayerUpdated;
        connection.OnPlayerDelete += OnPlayerLeft;

        // Connect
        await ConnectToServer();
    }

    async Task ConnectToServer()
    {
        try
        {
            await connection.ConnectAsync();
            await connection.SubscribeToTable("players");
            Debug.Log("Connected to InstantDB!");
        }
        catch (System.Exception ex)
        {
            Debug.LogError($"Failed to connect: {ex.Message}");
        }
    }

    void OnConnected()
    {
        Debug.Log("✅ Connected to InstantDB");
    }

    void OnDisconnected(System.Exception ex)
    {
        Debug.LogWarning($"Disconnected from InstantDB: {ex?.Message}");
    }

    void OnPlayerJoined(Player player)
    {
        Debug.Log($"Player joined: {player.Name}");
        // Spawn player GameObject
    }

    void OnPlayerUpdated(Player oldPlayer, Player newPlayer)
    {
        Debug.Log($"Player updated: {newPlayer.Name}");
        // Update player GameObject
    }

    void OnPlayerLeft(Player player)
    {
        Debug.Log($"Player left: {player.Name}");
        // Remove player GameObject
    }

    void OnDestroy()
    {
        connection?.Disconnect();
    }
}
)"},
        {"Assets/InstantDBConfig.asset.meta", R"(fileFormatVersion: 2
guid: YOUR_GUID_HERE
NativeFormatImporter:
  externalObjects: {}
  mainObjectFileID: 11400000
  userData:
  assetBundleName:
  assetBundleVariant:
)"},
        {"instantdb.config.json", R"({
  "serverUrl": "http://localhost:8080",
  "moduleName": "{{PROJECT_NAME_LOWER}}",
  "unity": {
    "autoConnect": true,
    "debugLogging": true
  },
  "tables": [
    {
      "name": "players",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "position_x", "type": "REAL"},
        {"name": "position_y", "type": "REAL"},
        {"name": "position_z", "type": "REAL"},
        {"name": "health", "type": "INTEGER"},
        {"name": "score", "type": "INTEGER"}
      ]
    },
    {
      "name": "game_sessions",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "max_players", "type": "INTEGER"},
        {"name": "current_players", "type": "INTEGER"},
        {"name": "status", "type": "TEXT"}
      ]
    }
  ]
}
)"},
        {"README.md", R"(# {{PROJECT_NAME}}

An InstantDB Unity multiplayer game project.

## Getting Started

1. Install the InstantDB Unity package:
   ```bash
   # In Unity Package Manager, add from git URL:
   https://github.com/instantdb/instantdb-unity.git
   ```

2. Start the InstantDB server:
   ```bash
   instantdb server
   ```

3. Open the project in Unity

4. Configure the InstantDB settings in the Inspector

5. Play the scene!

## Project Structure

- `Assets/Scripts/GameManager.cs` - Main game manager with InstantDB integration
- `Assets/InstantDBConfig.asset` - Connection configuration (create via menu)
- `instantdb.config.json` - Server-side configuration

## Creating the Config Asset

1. Right-click in Project window
2. Create → InstantDB → Configuration
3. Configure your server settings
4. Assign to GameManager

## Documentation

- [InstantDB Unity Guide](https://docs.instantdb.com/unity)
- [API Reference](https://docs.instantdb.com/api)
)"},
        {".gitignore", R"(# Unity
[Ll]ibrary/
[Tt]emp/
[Oo]bj/
[Bb]uild/
[Bb]uilds/
[Ll]ogs/
[Uu]ser[Ss]ettings/

# Visual Studio / MonoDevelop
.vs/
*.csproj
*.sln
*.suo
*.user
*.userprefs
*.pidb
*.booproj

# OS
.DS_Store
Thumbs.db

# InstantDB
instantdb_data/
logs/
)"}
    }},

    {"nodejs", {
        {"index.js", R"DELIMITER(const { InstantDBClient } = require('@instantdb/client');

async function main() {
  console.log('🚀 Connecting to InstantDB...');

  // Create client
  const client = new InstantDBClient({
    serverUrl: 'http://localhost:8080',
    moduleName: '{{PROJECT_NAME_LOWER}}'
  });

  // Connect to server
  await client.connect();
  console.log('✅ Connected to InstantDB!');

  // Subscribe to real-time updates
  await client.subscribeToTable('users');

  client.on('dataChange', (event) => {
    console.log(`Data changed: ${event.table} - ${event.operation}`);
    console.log('Data:', event.data);
  });

  // Execute a query
  const result = await client.sql('SELECT * FROM users');
  console.log(`Query returned ${result.rows.length} rows`);

  // Insert data
  await client.sql("INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')");

  // Keep the process alive
  process.stdin.resume();
  console.log('Press Ctrl+C to exit...');
}

main().catch(console.error);
)DELIMITER"},
        {"package.json", R"({
  "name": "{{PROJECT_NAME_LOWER}}",
  "version": "1.0.0",
  "description": "An InstantDB Node.js project",
  "main": "index.js",
  "scripts": {
    "start": "node index.js",
    "dev": "nodemon index.js"
  },
  "dependencies": {
    "@instantdb/client": "^1.0.0"
  },
  "devDependencies": {
    "nodemon": "^3.0.0"
  }
}
)"},
        {"instantdb.config.json", R"({
  "serverUrl": "http://localhost:8080",
  "moduleName": "{{PROJECT_NAME_LOWER}}",
  "tables": [
    {
      "name": "users",
      "columns": [
        {"name": "id", "type": "INTEGER", "primaryKey": true},
        {"name": "name", "type": "TEXT"},
        {"name": "email", "type": "TEXT"},
        {"name": "created_at", "type": "TIMESTAMP"}
      ]
    }
  ]
}
)"},
        {"README.md", R"(# {{PROJECT_NAME}}

An InstantDB Node.js project.

## Getting Started

1. Install dependencies:
   ```bash
   npm install
   ```

2. Start the InstantDB server:
   ```bash
   instantdb server
   ```

3. Run the application:
   ```bash
   npm start
   ```

## Documentation

- [InstantDB Documentation](https://docs.instantdb.com)
- [Node.js Client SDK](https://docs.instantdb.com/nodejs)
)"},
        {".gitignore", R"(node_modules/
.env
.DS_Store
instantdb_data/
logs/
)"}
    }}
};

void printUsage() {
    std::cout << BOLD << "Usage:" << RESET << " instantdb init [PROJECT_NAME] [OPTIONS]\n\n";
    std::cout << BOLD << "Options:\n" << RESET;
    std::cout << "  --template TEMPLATE  Project template (default: csharp)\n";
    std::cout << "                       Available: csharp, typescript, unity, nodejs\n";
    std::cout << "  --force             Overwrite existing files\n";
    std::cout << "  --no-git            Don't initialize git repository\n";
    std::cout << "  -h, --help          Show this help message\n\n";
    std::cout << BOLD << "Templates:\n" << RESET;
    std::cout << "  csharp      WASM module in C# (.NET 8 + wasi-experimental workload)\n";
    std::cout << "  typescript  WASM module in AssemblyScript (npm + asc)\n";
    std::cout << "  unity       Unity multiplayer client project\n";
    std::cout << "  nodejs      Node.js client application\n\n";
    std::cout << BOLD << "Examples:\n" << RESET;
    std::cout << "  instantdb init myproject                       # C# WASM module\n";
    std::cout << "  instantdb init mymodule --template typescript  # AssemblyScript WASM module\n";
    std::cout << "  instantdb init mygame --template unity         # Unity project\n";
    std::cout << "  instantdb init myapp --template nodejs         # Node.js project\n";
}

std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

bool createFile(const fs::path& filepath, const std::string& content, bool force = false) {
    // Create parent directories
    fs::create_directories(filepath.parent_path());

    // Check if file exists
    if (fs::exists(filepath) && !force) {
        std::cout << YELLOW << "  [SKIP]" << RESET << " " << filepath.string() << " (already exists)\n";
        return true;
    }

    // Write file
    std::ofstream file(filepath);
    if (!file) {
        std::cerr << RED << "  [ERROR]" << RESET << " Failed to create " << filepath.string() << "\n";
        return false;
    }

    file << content;
    file.close();

    std::cout << GREEN << "  [CREATE]" << RESET << " " << filepath.string() << "\n";
    return true;
}

bool initGitRepo(const fs::path& projectPath) {
    std::cout << "\n" << CYAN << "Initializing git repository..." << RESET << "\n";

    std::string cmd = "cd \"" + projectPath.string() + "\" && git init -q && git add . && git commit -q -m \"Initial commit\"";
    int result = std::system(cmd.c_str());

    if (result == 0) {
        std::cout << GREEN << "  ✓ Git repository initialized" << RESET << "\n";
        return true;
    } else {
        std::cout << YELLOW << "  ⚠ Could not initialize git repository" << RESET << "\n";
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Parse arguments
    if (argc < 2) {
        std::cerr << RED << "Error: Project name required" << RESET << "\n\n";
        printUsage();
        return 1;
    }

    std::string projectName;
    std::string templateName = "csharp";
    bool force = false;
    bool initGit = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        } else if (arg == "--template") {
            if (i + 1 < argc) {
                templateName = argv[++i];
            }
        } else if (arg.substr(0, 11) == "--template=") {
            templateName = arg.substr(11);
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--no-git") {
            initGit = false;
        } else if (projectName.empty()) {
            projectName = arg;
        }
    }

    // Validate template
    if (templates.find(templateName) == templates.end()) {
        std::cerr << RED << "Error: Unknown template '" << templateName << "'" << RESET << "\n";
        std::cerr << "Available templates: csharp, typescript, unity, nodejs\n";
        return 1;
    }

    // Create project directory
    fs::path projectPath = fs::current_path() / projectName;

    if (fs::exists(projectPath) && !force) {
        std::cerr << RED << "Error: Directory '" << projectName << "' already exists" << RESET << "\n";
        std::cerr << "Use --force to overwrite existing files\n";
        return 1;
    }

    std::cout << CYAN << BOLD << "\n🚀 Initializing InstantDB " << templateName << " project: "
              << projectName << RESET << "\n\n";

    // Create project directory
    fs::create_directories(projectPath);

    // Get template files
    const auto& templateFiles = templates[templateName];
    std::string projectNameLower = toLower(projectName);

    // Create files from template
    for (const auto& fileTemplate : templateFiles) {
        // Replace placeholders in path and content
        std::string filePath = replaceAll(fileTemplate.path, "{{PROJECT_NAME}}", projectName);
        std::string content = replaceAll(fileTemplate.content, "{{PROJECT_NAME}}", projectName);
        content = replaceAll(content, "{{PROJECT_NAME_LOWER}}", projectNameLower);

        // Create file
        fs::path fullPath = projectPath / filePath;
        if (!createFile(fullPath, content, force)) {
            std::cerr << RED << "Failed to create project files" << RESET << "\n";
            return 1;
        }
    }

    // Create additional directories
    if (templateName == "unity") {
        fs::create_directories(projectPath / "Assets" / "Scripts");
        fs::create_directories(projectPath / "Assets" / "Prefabs");
        fs::create_directories(projectPath / "Assets" / "Materials");
    }

    // Initialize git repository
    if (initGit) {
        initGitRepo(projectPath);
    }

    // Success message
    std::cout << "\n" << GREEN << BOLD << "✅ Project created successfully!" << RESET << "\n\n";

    std::cout << BOLD << "Next steps:" << RESET << "\n";
    std::cout << "  1. Navigate to your project:\n";
    std::cout << "     " << BLUE << "cd " << projectName << RESET << "\n";

    if (templateName == "csharp") {
        std::cout << "  2. Copy the InstantDB C# SDK into the project (no NuGet package yet):\n";
        std::cout << "     " << BLUE << "cp /path/to/instant_db/sdk/csharp/InstantDB.cs ." << RESET << "\n";
        std::cout << "  3. Install the WASI workload (requires .NET 8 SDK, once):\n";
        std::cout << "     " << BLUE << "dotnet workload install wasi-experimental" << RESET << "\n";
        std::cout << "  4. Build the WASM module:\n";
        std::cout << "     " << BLUE << "./build.sh" << RESET << "  (or: dotnet publish -c Release)\n";
        std::cout << "  5. Publish to a running InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb publish" << RESET << "\n";
    } else if (templateName == "typescript") {
        std::cout << "  2. Install dependencies:\n";
        std::cout << "     " << BLUE << "npm install" << RESET << "\n";
        std::cout << "  3. Build the WASM module:\n";
        std::cout << "     " << BLUE << "npm run asbuild" << RESET << "\n";
        std::cout << "  4. Publish to a running InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb publish" << RESET << "\n";
    } else if (templateName == "unity") {
        std::cout << "  2. Open the project in Unity\n";
        std::cout << "  3. Install the InstantDB Unity package\n";
        std::cout << "  4. Start the InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb server" << RESET << "\n";
        std::cout << "  5. Play the scene in Unity\n";
    } else if (templateName == "nodejs") {
        std::cout << "  2. Install dependencies:\n";
        std::cout << "     " << BLUE << "npm install" << RESET << "\n";
        std::cout << "  3. Start the InstantDB server:\n";
        std::cout << "     " << BLUE << "instantdb server" << RESET << "\n";
        std::cout << "  4. Run your application:\n";
        std::cout << "     " << BLUE << "npm start" << RESET << "\n";
    }

    std::cout << "\n" << BOLD << "Happy coding! 🎉" << RESET << "\n\n";

    return 0;
}