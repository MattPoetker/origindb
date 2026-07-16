# InstantDB C# SDK

Write InstantDB server-side WASM modules in C#. The SDK is a single source
file — `InstantDB.cs` — that you compile into your module project. It
implements the [InstantDB WASM ABI v1](../../docs/WASM_ABI.md):

- the four guest exports (`instantdb_alloc`, `instantdb_free`,
  `instantdb_describe`, `instantdb_invoke`) via `[UnmanagedCallersOnly]`
- all host imports (module `"env"`) via `[WasmImportLinkage]` +
  `[DllImport("env", ...)]` with raw pointer signatures and manual UTF-8
  marshalling
- a reducer registry that `instantdb_invoke` dispatches through
  (unregistered names return `-404` per the ABI)

## Toolchain — read this first

The server runs **core WASI preview 1 modules only**. WASI preview 2
*components* are rejected at deploy.

| Requirement | Value |
|---|---|
| .NET SDK | **8.0.4xx** (pin it — see `global.json` below) |
| Workload | `dotnet workload install wasi-experimental` |
| Target framework | `net8.0` |
| Runtime identifier | `wasi-wasm` (NOT `wasm-wasi`) |

> **Do not use .NET 9+ / componentize-dotnet for modules.** The
> `wasi-experimental` workload was removed in .NET 9; the replacement
> toolchain (componentize-dotnet / NativeAOT-LLVM) emits WASI **preview 2
> components**, which the InstantDB server cannot run. Pin .NET 8 with a
> `global.json` in every module project.
>
> **Known .NET 8 limitation:** `wasi-experimental` in .NET 8 builds WASI
> *command* modules and its support for `[UnmanagedCallersOnly]` function
> exports is incomplete (reactor/library mode landed later upstream — see
> dotnet/runtime#96869 and discussion #98888). If your published `.wasm` does
> not export `instantdb_invoke` (check with `wasm-tools print module.wasm |
> grep export`), your installed workload build predates export support; the
> AssemblyScript SDK (`sdk/typescript/`) is the fully verified path today.

### Project file

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <RuntimeIdentifier>wasi-wasm</RuntimeIdentifier>
    <WasmSingleFileBundle>true</WasmSingleFileBundle>
    <InvariantGlobalization>true</InvariantGlobalization>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <Nullable>enable</Nullable>
  </PropertyGroup>
  <ItemGroup>
    <!-- No NuGet package yet — compile the SDK source directly. -->
    <Compile Include="../../sdk/csharp/InstantDB.cs" />
  </ItemGroup>
</Project>
```

### global.json (pin .NET 8)

```json
{
  "sdk": {
    "version": "8.0.400",
    "rollForward": "latestFeature",
    "allowPrerelease": false
  }
}
```

### Build

```bash
dotnet workload install wasi-experimental   # once
dotnet publish -c Release
# → bin/Release/net8.0/wasi-wasm/AppBundle/<ProjectName>.wasm
```

Deploy the `.wasm` from the `AppBundle` directory with `instantdb publish`
(or the gRPC `DeployModule` call).

## Writing a module

```csharp
using System.Runtime.CompilerServices;
using System.Text.Json;
using InstantDB;

public static class Program
{
    // WASI command entry point — may stay empty. Do NOT register here:
    // the host may never call _start.
    public static void Main() { }

    // Guaranteed to run before any SDK export executes, whether the host
    // calls _initialize, _start, or invokes an export directly.
    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("my_module", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", args =>
        {
            string name = args[0].GetString()!;
            long id = Host.GenerateId();
            Db.Write("users", id.ToString(), JsonSerializer.Serialize(new
            {
                id = id.ToString(),
                name,
                created_at = Host.NowMs(),
            }));
            return new { id = id.ToString() };   // → host_set_result, status 0
        }, "name");

        Reducers.RegisterFilter("OnlyInserts",
            ev => ev.GetProperty("operation").GetString() == "INSERT");
    }
}
```

### Registry

| Call | Purpose |
|---|---|
| `Reducers.Register(name, Func<JsonElement[], object?> fn, params string[] paramNames)` | Register a reducer. Args arrive as the invocation's JSON array. A non-null return value is serialized and returned via `host_set_result`; the call returns status `0`. |
| `Reducers.RegisterFilter(name, Func<JsonElement, bool>)` | Subscription filter sugar — return value maps to ABI status `1` (include) / `0` (exclude). The event arrives parsed: `{table, operation, offset, transaction_id, key, new_value, old_value}`. |
| `Reducers.SetModuleInfo(name, version)` / `Reducers.DeclareTable(table)` | Metadata reported by `instantdb_describe`. |

Lifecycle hooks are plain registrations under the reserved names `__init`,
`__client_connected`, `__client_disconnected`, `__get_initial_data`
(unregistered reserved names return `-404`, which the host treats as a
harmless no-op).

Return values:

- `null` — status 0, no result payload
- any object — serialized with `System.Text.Json` (prefer simple
  types/`Dictionary<string, object?>`; reflection serialization of exotic
  types can break under trimming)
- `JsonElement` / `RawJson("…")` — passed through as-is
- throw `ReducerException(msg, code)` — call fails with that negative status;
  all staged writes/events are discarded

### Host API

| API | ABI import |
|---|---|
| `Db.Read(table, key)` → `string?` (row JSON or null) | `host_table_read` |
| `Db.Write(table, key, jsonObject)` | `host_table_write` |
| `Db.Delete(table, key)` | `host_table_delete` |
| `Db.Scan(table, prefix = "", limit = 0)` → `[{"key","value"}]` JSON | `host_table_scan` |
| `Host.EmitEvent(topic, key, payloadJson)` | `host_emit_event` |
| `Host.NowMs()` (fixed per call) / `Host.GenerateId()` | `host_now_ms` / `host_generate_id` |
| `Host.Log(level, msg)` + `LogTrace..LogError` | `host_log` |
| `Host.Abort(msg)` — records message and traps; never returns | `host_abort` |
| `Host.SetResult(json)` — advanced; the dispatcher normally does this | `host_set_result` |

All writes/events are staged and commit atomically only when the call
returns a non-negative status without trapping. Module memory does not
survive traps or redeploys — keep durable state in tables.

## Examples

- `Examples/CounterModule.cs` — reducers, filter, lifecycle hook, scans.
- `../../examples/csharp/UserService/` — a complete buildable project
  (csproj + global.json + README with deploy/call instructions).

## Notes & gotchas

- **Binary arguments** arrive as `{"$bytes": "<base64>"}` inside the JSON
  args array.
- **Exceptions** never escape the exports: unhandled exceptions are logged
  via `host_log` (error) and the call returns `-1`.
- **Buffers**: results of `host_table_read`/`host_table_scan` are written
  into buffers the host obtains from this module's `instantdb_alloc`
  (`NativeMemory.Alloc`); the SDK frees them with the same allocator — never
  `host_free`.
- `InvariantGlobalization` is required; there is no filesystem, environment,
  network, or thread support inside modules.
- Determinism: use `Host.NowMs()` / `Host.GenerateId()` instead of
  `DateTime.Now` / `Guid.NewGuid()`.
