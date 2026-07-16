# InstantDB CLI Tool Guide

The InstantDB CLI (`instantdb`) is the front door to the toolchain: it
initializes module projects, starts/stops the server, tails logs, builds and
publishes WASM modules, and dispatches to the bundled gRPC client. Most
subcommands are thin wrappers that locate and exec the corresponding binary
(`instantdb_server`, `instantdb_client`, `instantdb_init`, ...) from the same
directory or `PATH`.

## Installation

Build the CLI and its companion binaries:

```bash
cmake -B build -S .
cmake --build build --target instantdb instantdb_server instantdb_client instantdb_init
```

The binaries land in `./build/`.

## Command Overview

| Command | What it does |
|---|---|
| `init` | Initialize a new module/client project (via `instantdb_init`) |
| `server` / `start` | Start the InstantDB server (via `instantdb_server`) |
| `stop` | Stop a running server (`pkill instantdb_server`) |
| `logs` | View/follow the server log file |
| `publish` | Build the module project in the current directory and deploy it |
| `client` | gRPC client: SQL, status, and WASM module management |
| `sql` | One-shot local SQL executor (demo tool, in-process — not a server client) |
| `demo` | Run the demo application |
| `migrate` / `backup` / `restore` | **Not yet implemented** (print a placeholder and exit) |

Global options: `-h/--help`, `-v/--version`, `--verbose`.

## Command Reference

### Project Initialization

```bash
instantdb init myproject                       # C# WASM module (default)
instantdb init mymodule --template typescript  # AssemblyScript WASM module
instantdb init mygame --template unity         # Unity client project
instantdb init myapp --template nodejs         # Node.js client application
```

**Options:**
- `--template TEMPLATE` — `csharp` (default), `typescript`, `unity`, `nodejs`
- `--force` — overwrite existing files
- `--no-git` — don't initialize a git repository

The `csharp` and `typescript` templates produce WASM module projects that
`instantdb publish` can build and deploy directly. The C# template requires
the .NET 8 SDK with the `wasi-experimental` workload; the TypeScript
template requires npm (AssemblyScript). Note the C# toolchain caveat in
[sdk/csharp/README.md](sdk/csharp/README.md) — AssemblyScript is the fully
verified module path today.

### Server Management

```bash
# Start server (foreground)
instantdb server

# Custom ports and data directory
instantdb server -p 9090 -g 50052 -d ./mydata -l debug

# Stop server
instantdb stop
```

**Server options** (passed through to `instantdb_server`):
- `-p, --port PORT` — WebSocket port (default: 8080)
- `-g, --grpc-port PORT` — gRPC port (default: 50051)
- `-d, --data-dir DIR` — data directory (default: `./instantdb_data`)
- `-l, --log-level LEVEL` — `trace`, `debug`, `info`, `warn`, `error`
- `-c, --config FILE` — config file path

**Environment variables** (read by the server):
- `INSTANTDB_WS_PORT`, `INSTANTDB_GRPC_PORT`, `INSTANTDB_DATA_DIR`,
  `INSTANTDB_LOG_LEVEL`

There is no built-in daemon mode; use your shell (`... &`), `systemd`, or a
process manager for background operation. `instantdb stop` simply kills any
running `instantdb_server` process.

### Log Viewing

```bash
instantdb logs                 # last 50 lines of ./logs/instantdb.log
instantdb logs -n 100          # last 100 lines
instantdb logs --follow        # follow (tail -f)
instantdb logs --file ./path/to/other.log
```

### Publishing WASM Modules

`instantdb publish` builds the module project in the target directory and
deploys the resulting `.wasm` to a running server over gRPC. Deployment goes
through the bundled `instantdb_client` — **no `grpcurl` required**.

```bash
instantdb publish
instantdb publish --path=./my-module --server=prod.example.com:50051 --version=1.2.0
```

**Options:**
- `--server HOST:PORT` — InstantDB gRPC endpoint (default: `localhost:50051`)
- `--path PATH` — project path (default: current directory)
- `--version VERSION` — module version (default: `1.0.0`)

**Project detection:**

| Marker file | Project type | Build command | Expected output |
|---|---|---|---|
| `asconfig.json` | AssemblyScript | `npm run asbuild` | `build/module.wasm` or `build/release.wasm` |
| `*.csproj` | C# (.NET 8 + `wasi-experimental`) | `dotnet publish --configuration Release` | `bin/Release/net8.0/wasi-wasm/AppBundle/<Name>.wasm` (or `publish/`) |

The module is deployed under the project name (the `.csproj` stem, or the
directory name for AssemblyScript projects). After a successful publish,
test it with:

```bash
instantdb client -s localhost:50051 modules
instantdb client -s localhost:50051 call <module> <Reducer> '["arg1", 2]'
```

### gRPC Client (`instantdb client`)

`instantdb client` dispatches to `instantdb_client`, which talks to the
server's gRPC API (SQL + WASM services).

```bash
instantdb client [-s HOST:PORT] COMMAND [ARGS]
```

**Options:**
- `-s, --server ADDRESS` — server address (default: `localhost:50051`)

**Commands:**

| Command | Purpose |
|---|---|
| `status` | Server status: version, uptime, storage/network/gRPC stats |
| `exec "SQL"` | Execute a SQL statement |
| `interactive` | Interactive SQL shell against the server |
| `deploy NAME FILE.wasm [VERSION]` | Deploy a WASM module |
| `undeploy NAME` | Remove a WASM module (also deletes its persisted files) |
| `modules` | List deployed modules (name, version, sha256) |
| `call MODULE REDUCER [JSON_ARGS]` | Execute a reducer; args are a JSON array |

**Examples:**

```bash
instantdb client status
instantdb client exec "CREATE TABLE users (id INT64 PRIMARY KEY, name STRING)"
instantdb client exec "INSERT INTO users VALUES (1, 'Alice')"
instantdb client exec "SELECT * FROM users"

instantdb client deploy user_service ./UserService.wasm 1.0.0
instantdb client call user_service CreateUser '["Alice", "alice@example.com"]'
instantdb client modules
instantdb client undeploy user_service
```

JSON argument mapping: booleans, integers, floats, and strings map to the
corresponding reducer argument types; anything else is passed as its JSON
string form. Binary arguments use `{"$bytes": "<base64>"}` per the
[WASM ABI](docs/WASM_ABI.md).

### SQL Notes

The SQL layer is intentionally minimal (regex-based) right now:

- `CREATE TABLE`, `INSERT`, `SELECT * FROM table`, and simplified `UPDATE
  ... WHERE id=value` work.
- `SELECT` does **not** support WHERE clauses or column projection at the
  SQL layer (subscription-side WHERE filtering on the WebSocket API *is*
  supported); `DELETE` is unimplemented; `CREATE TABLE` currently ignores
  the column definitions you write.
- Identifiers preserve case.

`instantdb sql "STATEMENT"` runs a single statement against a throwaway
in-process engine — useful for syntax experiments only. Use
`instantdb client exec` / `instantdb client interactive` to talk to a real
server.

## Typical Development Flow

```bash
# 1. Create a module project
./build/instantdb init todo --template typescript
cd todo && npm install

# 2. Start the server (separate terminal)
./build/instantdb server -d ./data

# 3. Build + deploy the module
../build/instantdb publish

# 4. Call reducers and inspect state
../build/instantdb client call todo addTodo '["buy milk"]'
../build/instantdb client exec "SELECT * FROM todos"

# 5. Watch logs
../build/instantdb logs --follow
```

For real-time subscriptions, connect a WebSocket client to
`ws://localhost:8080` and send `sql_subscribe` / `subscribe_to_all_tables` /
`wasm_subscribe` messages (see `WASM_MODULES.md` and
`QA_TESTING_GUIDE.md`).

## Troubleshooting

1. **`Could not find 'instantdb_server' binary`** — the CLI looks in its own
   directory, `PATH`, and a few common install locations. Build the missing
   target or run the CLI from the build directory.
2. **Publish build fails (C#)** — requires .NET 8 SDK with
   `dotnet workload install wasi-experimental`. See
   [sdk/csharp/README.md](sdk/csharp/README.md) for the .NET 8 pinning and
   export-support caveat.
3. **Publish build fails (AssemblyScript)** — run `npm install` in the
   project first.
4. **Deployment fails** — is the server running, and is `--server` pointing
   at the gRPC port (default 50051, not the WebSocket port)?
5. **Log file not found** — `instantdb logs` defaults to
   `./logs/instantdb.log` relative to the current directory; pass `--file`
   if your server logs elsewhere.

---

For per-command help, use `instantdb <command> --help`.
