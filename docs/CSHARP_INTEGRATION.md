# OriginDB C# Integration Guide

C# fits into OriginDB in two distinct ways:

1. **Server-side WASM modules** — write reducers in C#, compile to
   WebAssembly, deploy into the server. This is supported today via the
   single-file SDK in `sdk/csharp/` (with an important .NET 8 toolchain
   caveat, below).
2. **Client applications** (Unity, WPF, ASP.NET, ...) — there is currently
   **no official OriginDB C# client library, NuGet package, or Unity
   package**. Clients talk to the server directly over its WebSocket JSON
   protocol and/or gRPC. Earlier versions of this document described an
   `OriginDB.Client`/Unity SDK that does not exist; treat any such API as
   unimplemented.

## 1. Writing server-side modules in C#

The authoritative guide is [../sdk/csharp/README.md](../sdk/csharp/README.md).
Summary:

- **Toolchain**: .NET SDK **8.0.4xx** (pin with `global.json`) + the
  `wasi-experimental` workload. Do not use .NET 9+/componentize-dotnet —
  those emit WASI preview 2 *components*, which the server rejects.
- **Known limitation**: .NET 8 `wasi-experimental` support for
  `[UnmanagedCallersOnly]` exports is incomplete upstream. If your
  published module doesn't export `origindb_invoke`, your workload build
  predates export support — the AssemblyScript SDK (`sdk/typescript/`) is
  the fully verified module path today.

```csharp
using System.Runtime.CompilerServices;
using System.Text.Json;
using OriginDB;

public static class Program
{
    public static void Main() { }

    [ModuleInitializer]
    internal static void Init()
    {
        Reducers.SetModuleInfo("user_service", "1.0.0");
        Reducers.DeclareTable("users");

        Reducers.Register("CreateUser", args =>
        {
            string name = args[0].GetString()!;
            long id = Host.GenerateId();
            Db.Write("users", id.ToString(), JsonSerializer.Serialize(new
            {
                id = id.ToString(), name, created_at = Host.NowMs(),
            }));
            return new { id = id.ToString() };
        }, "name");
    }
}
```

Build and deploy:

```bash
dotnet workload install wasi-experimental   # once
dotnet publish -c Release
# → bin/Release/net8.0/wasi-wasm/AppBundle/<ProjectName>.wasm

# One-step build + deploy from the project directory:
origindb publish --server=localhost:50051

# Or deploy a prebuilt .wasm:
origindb_client deploy user_service bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm 1.0.0
origindb_client call user_service CreateUser '["Alice"]'
```

A complete buildable example lives at `examples/csharp/UserService/`.
`origindb init myproject` (the default `csharp` template) scaffolds a new
module project with the SDK wired in.

## 2. Building C# clients

### Real-time data: WebSocket JSON protocol

Use any .NET WebSocket client (`System.Net.WebSockets.ClientWebSocket`)
against `ws://<host>:8080`:

```csharp
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;

var ws = new ClientWebSocket();
await ws.ConnectAsync(new Uri("ws://localhost:8080"), CancellationToken.None);

// Subscribe to a table with a server-evaluated WHERE clause
var sub = JsonSerializer.Serialize(new
{
    type = "sql_subscribe",
    sql = "SELECT * FROM players WHERE team = 'red'"
});
await ws.SendAsync(Encoding.UTF8.GetBytes(sub),
    WebSocketMessageType.Text, true, CancellationToken.None);

// Then read messages: sql_subscription_created, initial_state,
// and sql_changefeed_event frames (see docs/API.md for formats).
```

Available subscription messages: `sql_subscribe` (per-event WHERE
evaluation), `subscribe_to_all_tables`, and `wasm_subscribe`
(module-backed filter/transform — see
[WASM_SUBSCRIPTIONS.md](WASM_SUBSCRIPTIONS.md)).

### Commands and queries: gRPC

Generate a C# client from `proto/origindb.proto` with `Grpc.Tools`:

```xml
<ItemGroup>
  <PackageReference Include="Google.Protobuf" Version="3.*" />
  <PackageReference Include="Grpc.Net.Client" Version="2.*" />
  <PackageReference Include="Grpc.Tools" Version="2.*" PrivateAssets="All" />
  <Protobuf Include="proto/origindb.proto" GrpcServices="Client" />
</ItemGroup>
```

```csharp
using Grpc.Net.Client;
using Origindb.Grpc;

var channel = GrpcChannel.ForAddress("http://localhost:50051");
var sql = new SQLService.SQLServiceClient(channel);

var resp = await sql.ExecuteAsync(new SQLRequest
{
    Sql = "INSERT INTO players VALUES (1, 'Alice')"
});

var wasm = new WasmService.WasmServiceClient(channel);
var call = await wasm.ExecuteReducerAsync(new ExecuteReducerRequest
{
    ModuleName = "user_service",
    ReducerName = "CreateUser",
    Args = { new WasmValue { StringValue = "Alice" } }
});
```

Note: the gRPC endpoint is plaintext and unauthenticated in the current
version — keep it on a trusted network.

### Unity

`origindb init mygame --template unity` scaffolds a Unity project with
example scripts, but those scripts reference a client wrapper
(`OriginDB.Unity`) that is not shipped yet — treat them as a starting
sketch. A working Unity integration today means using
`ClientWebSocket` (as above) from your own MonoBehaviours and marshalling
events onto the main thread yourself.

## Current limitations (client side)

- No official C#/Unity client package (no NuGet, no `.unitypackage`)
- No authentication/TLS on WebSocket or gRPC yet
- SQL layer is minimal: `SELECT` has no WHERE/projection (use
  subscriptions for filtered reads), `DELETE` is unimplemented

## See also

- [../WASM_MODULES.md](../WASM_MODULES.md) — module system guide
- [WASM_ABI.md](WASM_ABI.md) — the module ABI
- [GRPC_API.md](GRPC_API.md) — gRPC service reference
- [API.md](API.md) — WebSocket message formats
