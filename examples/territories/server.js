// Territories demo web server (sharded).
//
// The world is split into a REGION grid. The SAME module is deployed once per
// region (terr_<rx>_<ry>); each instance owns a disjoint chunk range, so N
// module copies tick in parallel (each has its own per-module mutex → its own
// core). This bridge:
//   * drives every region's tick concurrently each frame, and
//   * routes each order to the module that OWNS the target's region, so a unit's
//     orders and its region tick serialize on one mutex (no cross-module race).
//
//   browser --POST /api/call {reducer,args,rx,ry}--> bridge --gRPC--> owning shard
//   bridge  --gRPC tick(dt,rx,ry,RS) x N shards (parallel)----------> all shards
//   browser <--ws sql_changefeed_event ----------------------------- OriginDB
//
// Usage: node server.js [--port 9092] [--grpc localhost:50053] [--ws-port 8789]
//        [--tick-hz 10] [--region-size 20]

import http from "node:http";
import net from "node:net";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

const __dirname = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const flag = (n, d) => { const i = args.indexOf(n); return i >= 0 && args[i + 1] ? args[i + 1] : d; };

const PORT = parseInt(flag("--port", "9092"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50053");
const WS_PORT = parseInt(flag("--ws-port", "8789"), 10);
const TICK_HZ = parseInt(flag("--tick-hz", "10"), 10);
const CLIENT_TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");

const WORLD = 6000, CHUNK = 100, SEED = 1337;
const NCHUNKS = WORLD / CHUNK;                                   // 60
const RS = parseInt(flag("--region-size", "20"), 10);           // chunks per region side
const REGS = Math.ceil(NCHUNKS / RS);                           // regions per side (3)

// module name per region — the same wasm deployed REGS*REGS times
const MODULES = [];
for (let ry = 0; ry < REGS; ry++) for (let rx = 0; rx < REGS; rx++) MODULES.push({ name: `terr_${rx}_${ry}`, rx, ry });
const moduleFor = (rx, ry) => `terr_${clampR(rx)}_${clampR(ry)}`;
const HOME = MODULES[0].name;                                   // for region-less calls (spawn/leave)
function clampR(v) { v = v | 0; return v < 0 ? 0 : v >= REGS ? REGS - 1 : v; }

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
      { module_name: moduleName, reducer_name: reducer, sender_identity: "territories-web", args: argsList.map(toWasmValue) },
      authMeta(), { deadline: Date.now() + timeoutMs }, (err, r) => (err ? rej(err) : res(r)));
  });
}

// --- parallel per-region tick ------------------------------------------------
let lastTick = Date.now();
let tickInFlight = false;
setInterval(async () => {
  if (tickInFlight) return;
  const now = Date.now();
  const dtMs = now - lastTick;
  lastTick = now;
  tickInFlight = true;
  try {
    // every region ticks concurrently; each only touches its own chunks
    await Promise.all(MODULES.map((m) =>
      executeReducer(m.name, "tick", [dtMs, m.rx, m.ry, RS], 4000).catch((e) => {
        if (server.listening) process.stderr.write(`tick ${m.name} failed: ${e.message}\n`);
      })));
  } finally {
    tickInFlight = false;
  }
}, Math.max(20, Math.floor(1000 / TICK_HZ)));

// --- HTTP --------------------------------------------------------------------
const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };
const ALLOWED = new Set(["spawnEmpire", "moveUnits", "gather", "attack", "build", "train", "collect", "leave"]);
const REGIONLESS = new Set(["spawnEmpire", "leave"]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT, token: CLIENT_TOKEN, world: WORLD, chunk: CHUNK, seed: SEED, regionSize: RS, regions: REGS }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/call") {
      let body = "";
      for await (const chunk of req) { body += chunk; if (body.length > 64 * 1024) throw new Error("payload too large"); }
      const { reducer, args: callArgs, rx, ry } = JSON.parse(body);
      if (!ALLOWED.has(reducer) || !Array.isArray(callArgs)) {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ success: false, error: "invalid call" })); return;
      }
      // route to the owning region's module (spawn/leave go to the home shard)
      const target = REGIONLESS.has(reducer) ? HOME : moduleFor(rx ?? 0, ry ?? 0);
      const response = await executeReducer(target, reducer, callArgs);
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

// Websocket proxy: pipe Upgrade requests through to OriginDB's ws (WS_PORT) so
// the browser connects same-origin (wss://territory.origindb.org/) — no mixed
// content, no separate ws port through the tunnel.
server.on("upgrade", (req, clientSocket, head) => {
  const upstream = net.connect(WS_PORT, "127.0.0.1", () => {
    let raw = `${req.method} ${req.url} HTTP/1.1\r\n`;
    for (let i = 0; i < req.rawHeaders.length; i += 2) raw += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
    upstream.write(raw + "\r\n");
    if (head && head.length) upstream.write(head);
    upstream.pipe(clientSocket);
    clientSocket.pipe(upstream);
  });
  const kill = () => { try { upstream.destroy(); } catch {} try { clientSocket.destroy(); } catch {} };
  upstream.on("error", kill);
  clientSocket.on("error", kill);
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`territories: http://localhost:${PORT}`);
  console.log(`gRPC target: ${GRPC_TARGET}`);
  console.log(`realtime ws: ws://<this-host>:${WS_PORT}`);
  console.log(`shards:      ${MODULES.length} regions (${REGS}x${REGS}, ${RS} chunks each) @ ${TICK_HZ} Hz`);
});
