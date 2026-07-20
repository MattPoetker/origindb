// Marble Clash web server — LOBBY + MATCHMAKING, single-module edition.
//
// One OriginDB module (marble_0) hosts MANY concurrent matches; the server's
// native tick scheduler ticks it once per frame and its `tick` reducer steps
// every active match. Matchmaking is pure data (lobbies/members tables). This
// bridge is HTTP -> gRPC for the writes; reads stream to the browser over the
// OriginDB websocket (lobbies/members + marbles WHERE arena='<lobbyId>').
//
//   browser --POST /api/call {reducer,args}--> bridge --gRPC--> marble_0
//   browser <--ws sql_changefeed_event --------------------- OriginDB
//
// Usage: node server.js [--port 9093] [--grpc localhost:50054] [--ws-port 8790]

import http from "node:http";
import net from "node:net";
import crypto from "node:crypto";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

const __dirname = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };

const PORT = parseInt(flag("--port", "9093"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50054");
const WS_PORT = parseInt(flag("--ws-port", "8790"), 10);
const LOBBY_CAP = parseInt(flag("--cap", "8"), 10);
const CLIENT_TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");
const ARENA_R = 42.0;
const MODULE = "marble_0";   // the single module that hosts all matches

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
// Derive a public identity from a client's SECRET anon token: identity =
// sha256(token). The token stays secret (client-held, sent per request); the
// identity is what reducers see + store as an owner. Because it's a one-way
// hash, seeing an owner identity in a streamed row does NOT let an attacker
// authenticate as it — they'd need the original token. (A stand-in for real
// OIDC/JWT identities; same pattern: public identity, secret credential.)
function identityFrom(token) {
  if (!token) return "";
  return "u_" + crypto.createHash("sha256").update(String(token)).digest("hex").slice(0, 24);
}

function executeReducer(reducer, argsList, sender, timeoutMs = 5000) {
  return new Promise((res, rej) => {
    wasm.ExecuteReducer(
      { module_name: MODULE, reducer_name: reducer, sender_identity: sender || "", args: argsList.map(toWasmValue) },
      authMeta(), { deadline: Date.now() + timeoutMs }, (err, r) => (err ? rej(err) : res(r)));
  });
}

// --- HTTP --------------------------------------------------------------------
const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };
const ALLOWED = new Set(["createLobby", "joinLobby", "startNow", "leaveLobby", "dissolveLobby", "steer", "boost"]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);
    res.setHeader("access-control-allow-origin", "*");
    res.setHeader("access-control-allow-headers", "content-type, x-odb-identity");
    res.setHeader("access-control-allow-methods", "GET, POST, OPTIONS");
    if (req.method === "OPTIONS") { res.writeHead(204); res.end(); return; }

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT, token: CLIENT_TOKEN, arenaR: ARENA_R, lobbyCap: LOBBY_CAP }));
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
      // The caller's identity comes from a per-client secret token header, NOT
      // from the payload — so it can't be spoofed by another client.
      const sender = identityFrom(req.headers["x-odb-identity"]);
      const response = await executeReducer(reducer, callArgs, sender);
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: response.success, error: response.error, result: firstResult(response) }));
      return;
    }

    let path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) { res.writeHead(403); res.end(); return; }
    const file = join(__dirname, "public", path);
    const data = await readFile(file);
    // HTML must never be stored (it's the entry doc; a stale copy pins old asset
    // URLs and breaks clients after a deploy). Assets revalidate; they're also
    // version-busted in index.html (game.js?v=N), so a new deploy = new URL.
    const isHtml = extname(file) === ".html";
    res.writeHead(200, {
      "content-type": MIME[extname(file)] ?? "application/octet-stream",
      "cache-control": isHtml ? "no-store, must-revalidate" : "no-cache",
    });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") { res.writeHead(404); res.end("not found"); }
    else { process.stderr.write(`${err.message}\n`); res.writeHead(500, { "content-type": "application/json" }); res.end(JSON.stringify({ success: false, error: err.message })); }
  }
});

// Websocket proxy: pipe any Upgrade request through to OriginDB's websocket
// (WS_PORT). Lets ONE public hostname (marble.origindb.org via the tunnel) serve
// both reducer calls over HTTP (/api/*) and realtime reads over wss://…/ — the
// browser connects same-origin, so no mixed-content and no separate ws port.
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
  console.log(`marble clash (lobby): http://localhost:${PORT}`);
  console.log(`gRPC target:          ${GRPC_TARGET}  module ${MODULE}`);
  console.log(`realtime ws:          ws://<this-host>:${WS_PORT}`);
  console.log(`matches:              unbounded, ${LOBBY_CAP} players each, ticked in one module`);
});
