# UserService — OriginDB C# module example

A minimal OriginDB WASM module: a `users` table with `CreateUser` /
`GetUsers` reducers and an `OnlyUserInserts` subscription filter. Uses the
single-file SDK at `../../../sdk/csharp/OriginDB.cs` (compiled in via
`<Compile Include>` — there is no NuGet package yet).

## Prerequisites

- .NET SDK **8.0.4xx** (pinned by `global.json`; .NET 9+ toolchains emit
  WASI p2 components which the OriginDB server cannot run)
- The WASI workload: `dotnet workload install wasi-experimental`

## Build

```bash
dotnet publish -c Release
# → bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm
```

(To compile-check on a machine without the wasi workload:
`dotnet build -p:SkipWasi=true`.)

## Deploy

```bash
# Via the CLI:
origindb publish --server=http://localhost:9090 \
  bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm

# Or directly over gRPC:
grpcurl -plaintext -import-path ../../../proto -proto origindb.proto \
  -d '{
    "name": "user_service",
    "version": "1.0.0",
    "bytecode": "'$(base64 < bin/Release/net8.0/wasi-wasm/AppBundle/UserService.wasm)'"
  }' \
  localhost:50051 origindb.grpc.WasmService.DeployModule
```

## Call the reducers

```bash
# Create a user → result payload {"id": "..."}
grpcurl -plaintext -import-path ../../../proto -proto origindb.proto \
  -d '{
    "module_name": "user_service",
    "reducer_name": "CreateUser",
    "args": [
      {"string_value": "Alice"},
      {"string_value": "alice@example.com"}
    ]
  }' \
  localhost:50051 origindb.grpc.WasmService.ExecuteReducer

# List users → result payload [ {"id": ..., "name": ..., ...} ]
grpcurl -plaintext -import-path ../../../proto -proto origindb.proto \
  -d '{"module_name": "user_service", "reducer_name": "GetUsers"}' \
  localhost:50051 origindb.grpc.WasmService.ExecuteReducer
```

Validation failures (empty name, malformed email) return a negative status
(`-3`, invalid argument) and discard all staged writes.

## Use the filter

Attach `OnlyUserInserts` as the filter function when subscribing to the
changefeed; only `INSERT` events on the `users` table are forwarded
(the module returns ABI status `1` to include, `0` to exclude).
