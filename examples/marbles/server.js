// Marble Clash web server (arena-sharded, 60 Hz).
//
// A physics sim moves EVERY body EVERY tick, so unlike the RTS demo there is no
// "idle" state to skip — every marble row is dirty every frame. The scaling
// model is therefore horizontal: the SAME module is deployed once per ARENA
// (marble_<n>); each instance ticks on its own per-module mutex → its own core.
// Players are matchmade into an arena with room (cap CAP each). This bridge:
//   * drives every arena's tick concurrently at TICK_HZ, and
//   * routes each order to the module hosting that player's arena.
//
//   browser --POST /api/call {reducer,args,session}--> bridge --gRPC--> arena
//   bridge  --gRPC tick(dtMs,subSteps) x N arenas (parallel)----------> arenas
//   browser <--ws sql_changefeed_event ------------------------------- OriginDB
//
// Usage: node server.js [--port 9093] [--grpc localhost:50053] [--ws-port 8790]
//        [--tick-hz 60] [--arenas 8] [--cap 60] [--substeps 1]

import http from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

const __dirname = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };

const PORT = parseInt(flag("--port", "9093"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50053");
const WS_PORT = parseInt(flag("--ws-port", "8790"), 10);
const TICK_HZ = parseInt(flag("--tick-hz", "60"), 10);
const NARENAS = parseInt(flag("--arenas", "8"), 10);
const CAP = parseInt(flag("--cap", "60"), 10);          // marbles per arena
const SUBSTEPS = parseInt(flag("--substeps", "1"), 10); // physics steps per tick
const CLIENT_TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");
const ARENA_R = 42.0;

// module name per arena — the same wasm deployed NARENAS times
const ARENAS = [];
for (let i = 0; i < NARENAS; i++) ARENAS.push({ name: `marble_${i}`, idx: i, count: 0 });

// session -> arena idx (matchmaking + order routing)
const sessionArena = new Map();

function pickArena() {
  // least-full arena with room; falls back to least-full overall
  let best = ARENAS[0];
  for (const a of ARENAS) if (a.count < best.count) best = a;
  return best;
}

// --- gRPC --------------------------------------------------------------------
const proto = grpc.loadPackageDefinition(
  protoLoader.loadSync(resolve(__dirname, "../../proto/origindb.proto"), { keepCase: true, longs: String, defaults: true })
).origindb.grpc;
const wasm = new proto.WasmService(GRPC_TARGET, grpc.credentials.createInsecure());

function authMeta() { const m = new grpc.Metadata(); if (CLIENT_TOKEN) m.set("authorization", `Bearer ${CLIENT_TOKEN}`); return m; }
function toWasmValue(v) {
  if (typeof v === "boolean") return { bool_value: v };
  if (typeof v === "number") return Number.isInteger(v) ? { int64_value: v } : { double_value: v };
  if (typeof v === "string") return { string_value: v };
  return { string_value: JSON.stringify(v) };
}
function firstResult(response) {
  const r = response && response.results && response.results[0];
  if (!r) return null;
  let s = null;
  if (r.bytes_value != null && r.bytes_value.length) s = Buffer.from(r.bytes_value).toString("utf8");
  else if (r.string_value != null && r.string_value.length) s = r.string_value;
  if (s == null) return null;
  try { return JSON.parse(s); } catch { return s; }
}
function executeReducer(moduleName, reducer, argsList, timeoutMs = 5000) {
  return new Promise((res, rej) => {
    wasm.ExecuteReducer(
      { module_name: moduleName, reducer_name: reducer, sender_identity: "marbles-web", args: argsList.map(toWasmValue) },
      authMeta(), { deadline: Date.now() + timeoutMs }, (err, r) => (err ? rej(err) : res(r)));
  });
}

// --- parallel per-arena 60 Hz tick -------------------------------------------
let lastTick = Date.now();
let tickInFlight = false;
let slowTicks = 0, totalTicks = 0, maxTickMs = 0;
const tickPeriod = TICK_HZ > 0 ? Math.max(4, Math.floor(1000 / TICK_HZ)) : 0;
if (tickPeriod > 0) setInterval(async () => {
  if (tickInFlight) { slowTicks++; return; }   // previous frame still running → we're behind
  const now = Date.now();
  const dtMs = now - lastTick;
  lastTick = now;
  tickInFlight = true;
  const t0 = Date.now();
  try {
    await Promise.all(ARENAS.map((a) =>
      executeReducer(a.name, "tick", [dtMs, SUBSTEPS, a.idx], 4000).catch((e) => {
        if (server.listening) process.stderr.write(`tick ${a.name} failed: ${e.message}\n`);
      })));
  } finally {
    const el = Date.now() - t0;
    if (el > maxTickMs) maxTickMs = el;
    if (el > tickPeriod) slowTicks++;
    totalTicks++;
    tickInFlight = false;
  }
}, tickPeriod);

// periodic health line to stdout
setInterval(() => {
  const marbles = ARENAS.reduce((s, a) => s + a.count, 0);
  const mode = tickPeriod > 0 ? `bridge-tick=${TICK_HZ}Hz maxTickMs=${maxTickMs} slow=${slowTicks}/${totalTicks}` : `tick=native(server)`;
  process.stdout.write(`[health] arenas=${NARENAS} joined=${marbles} ${mode}\n`);
  maxTickMs = 0; slowTicks = 0; totalTicks = 0;
}, 5000);

// --- HTTP --------------------------------------------------------------------
const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };
const ALLOWED = new Set(["spawn", "steer", "boost", "leave"]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT, token: CLIENT_TOKEN, arenaR: ARENA_R, arenas: NARENAS, cap: CAP }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/call") {
      let body = "";
      for await (const chunk of req) { body += chunk; if (body.length > 64 * 1024) throw new Error("payload too large"); }
      const { reducer, args: callArgs } = JSON.parse(body);
      if (!ALLOWED.has(reducer) || !Array.isArray(callArgs)) {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ success: false, error: "invalid call" })); return;
      }
      const session = String(callArgs[0] || "");
      if (!session) { res.writeHead(400); res.end(JSON.stringify({ success: false, error: "no session" })); return; }

      if (reducer === "spawn") {
        // matchmake: reuse existing arena if the session already has one
        let a = ARENAS[sessionArena.get(session) ?? -1];
        if (!a) { a = pickArena(); sessionArena.set(session, a.idx); a.count++; }
        // spawn(session, name, color, arena) — bridge appends the arena index
        const spawnArgs = [callArgs[0], callArgs[1] ?? "", callArgs[2] ?? "", a.idx];
        const response = await executeReducer(a.name, "spawn", spawnArgs);
        const result = firstResult(response);
        res.writeHead(200, { "content-type": "application/json" });
        res.end(JSON.stringify({ success: response.success, error: response.error, result: { ...result, arena: a.idx } }));
        return;
      }

      const idx = sessionArena.get(session);
      if (idx === undefined) { res.writeHead(409); res.end(JSON.stringify({ success: false, error: "not joined" })); return; }
      const a = ARENAS[idx];
      if (reducer === "leave") { a.count = Math.max(0, a.count - 1); sessionArena.delete(session); }
      const response = await executeReducer(a.name, reducer, callArgs);
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: response.success, error: response.error, result: firstResult(response) }));
      return;
    }

    let path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) { res.writeHead(403); res.end(); return; }
    const file = join(__dirname, "public", path);
    const data = await readFile(file);
    res.writeHead(200, { "content-type": MIME[extname(file)] ?? "application/octet-stream", "cache-control": "no-cache" });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") { res.writeHead(404); res.end("not found"); }
    else { process.stderr.write(`${err.message}\n`); res.writeHead(500, { "content-type": "application/json" }); res.end(JSON.stringify({ success: false, error: err.message })); }
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`marble clash: http://localhost:${PORT}`);
  console.log(`gRPC target:  ${GRPC_TARGET}`);
  console.log(`realtime ws:  ws://<this-host>:${WS_PORT}`);
  console.log(`arenas:       ${NARENAS} x cap ${CAP} = ${NARENAS * CAP} seats @ ${TICK_HZ} Hz (substeps ${SUBSTEPS})`);
});
