// cube.io demo web server.
//
// Serves the game client and bridges browser HTTP calls to OriginDB's gRPC
// WasmService. The authoritative simulation is a WASM reducer (`tick`) that
// this server drives on a fixed interval — every player and food cube lives in
// OriginDB tables and streams to browsers over the changefeed websocket.
//
//   browser --POST /api/call {join,input,leave}--> server --gRPC--> OriginDB
//   server  --gRPC tick(dtMs) @ TICK_HZ------------------------------> OriginDB
//   browser <--ws sql_changefeed_event (players, food)-------------- OriginDB
//
// Usage: node server.js [--port 9091] [--grpc localhost:50052] [--ws-port 8788] [--token <client>]

import http from "node:http";
import net from "node:net";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

const __dirname = dirname(fileURLToPath(import.meta.url));

const args = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
const PORT = parseInt(flag("--port", "9091"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50052");
const WS_PORT = parseInt(flag("--ws-port", "8788"), 10);
const TICK_HZ = parseInt(flag("--tick-hz", "15"), 10);
const MODULE = "cubeio";
const WORLD = 12000;
const CLIENT_TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");

// --- gRPC client -------------------------------------------------------------

const protoPath = resolve(__dirname, "../../proto/origindb.proto");
const packageDef = protoLoader.loadSync(protoPath, {
  keepCase: true,
  longs: String,
  defaults: true,
});
const proto = grpc.loadPackageDefinition(packageDef).origindb.grpc;
const wasm = new proto.WasmService(GRPC_TARGET, grpc.credentials.createInsecure());

function authMeta() {
  const meta = new grpc.Metadata();
  if (CLIENT_TOKEN) meta.set("authorization", `Bearer ${CLIENT_TOKEN}`);
  return meta;
}

function toWasmValue(v) {
  if (typeof v === "boolean") return { bool_value: v };
  if (typeof v === "number")
    return Number.isInteger(v) ? { int64_value: v } : { double_value: v };
  if (typeof v === "string") return { string_value: v };
  return { string_value: JSON.stringify(v) };
}

function executeReducer(reducer, argsList, timeoutMs = 5000) {
  return new Promise((res, rej) => {
    wasm.ExecuteReducer(
      {
        module_name: MODULE,
        reducer_name: reducer,
        sender_identity: "cubeio-web",
        args: argsList.map(toWasmValue),
      },
      authMeta(),
      { deadline: Date.now() + timeoutMs },
      (err, response) => (err ? rej(err) : res(response)),
    );
  });
}

// --- authoritative tick loop -------------------------------------------------

let lastTick = Date.now();
let tickInFlight = false;
setInterval(async () => {
  if (tickInFlight) return;        // never overlap ticks
  const now = Date.now();
  const dtMs = now - lastTick;
  lastTick = now;
  tickInFlight = true;
  try {
    await executeReducer("tick", [dtMs], 4000);
  } catch (err) {
    // A dropped tick is survivable; the next one uses a larger dt.
    if (!server.listening) return;
    process.stderr.write(`tick failed: ${err.message}\n`);
  } finally {
    tickInFlight = false;
  }
}, Math.max(20, Math.floor(1000 / TICK_HZ)));

// --- HTTP server -------------------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
};

const ALLOWED = new Set(["join", "input", "leave"]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT, module: MODULE, token: CLIENT_TOKEN, world: WORLD }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/call") {
      let body = "";
      for await (const chunk of req) {
        body += chunk;
        if (body.length > 16 * 1024) throw new Error("payload too large");
      }
      const { reducer, args: callArgs } = JSON.parse(body);
      if (!ALLOWED.has(reducer) || !Array.isArray(callArgs)) {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ success: false, error: "invalid call" }));
        return;
      }
      const response = await executeReducer(reducer, callArgs);
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: response.success, error: response.error }));
      return;
    }

    let path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) { res.writeHead(403); res.end(); return; }
    const file = join(__dirname, "public", path);
    const data = await readFile(file);
    res.writeHead(200, {
      "content-type": MIME[extname(file)] ?? "application/octet-stream",
      "cache-control": "no-cache",
    });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") { res.writeHead(404); res.end("not found"); }
    else {
      process.stderr.write(`${err.message}\n`);
      res.writeHead(500, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: false, error: err.message }));
    }
  }
});

// Websocket proxy: pipe Upgrade requests through to OriginDB's ws (WS_PORT) so
// the browser connects same-origin (wss://cubeio.origindb.org/) — no mixed
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
  console.log(`cube.io:      http://localhost:${PORT}`);
  console.log(`gRPC target:  ${GRPC_TARGET}`);
  console.log(`realtime ws:  ws://<this-host>:${WS_PORT}`);
  console.log(`tick:         ${TICK_HZ} Hz`);
});
