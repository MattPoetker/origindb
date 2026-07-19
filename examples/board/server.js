// Board demo web server — the minimal bridge for the Getting Started tutorial.
// Serves the static page and forwards reducer CALLS from the browser to
// OriginDB over gRPC (browsers can't speak gRPC). Realtime READS do not pass
// through here — the page subscribes to OriginDB's websocket directly.
//
// Usage: node server.js [--port 3020] [--grpc localhost:50051] [--ws-port 8787]

import http from "node:http";
import { readFile } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import grpc from "@grpc/grpc-js";
import protoLoader from "@grpc/proto-loader";

const __dirname = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };

const PORT = parseInt(flag("--port", "3020"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50051");
const WS_PORT = parseInt(flag("--ws-port", "8787"), 10);
const MODULE = "board";

const proto = grpc.loadPackageDefinition(
  protoLoader.loadSync(resolve(__dirname, "../../proto/origindb.proto"), { keepCase: true, longs: String, defaults: true })
).origindb.grpc;
const wasm = new proto.WasmService(GRPC_TARGET, grpc.credentials.createInsecure());

function toWasmValue(v) {
  if (typeof v === "boolean") return { bool_value: v };
  if (typeof v === "number") return Number.isInteger(v) ? { int64_value: v } : { double_value: v };
  return { string_value: String(v) };
}
function callReducer(reducer, args) {
  return new Promise((res, rej) => wasm.ExecuteReducer(
    { module_name: MODULE, reducer_name: reducer, sender_identity: "board-web", args: args.map(toWasmValue) },
    { deadline: Date.now() + 5000 }, (err, r) => (err ? rej(err) : res(r))));
}

const MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };
const ALLOWED = new Set(["addNote", "clearNotes"]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT }));
      return;
    }
    if (req.method === "POST" && url.pathname === "/api/call") {
      let body = "";
      for await (const chunk of req) { body += chunk; if (body.length > 64 * 1024) throw new Error("too large"); }
      const { reducer, args } = JSON.parse(body);
      if (!ALLOWED.has(reducer) || !Array.isArray(args)) { res.writeHead(400); res.end('{"error":"bad call"}'); return; }
      const r = await callReducer(reducer, args);
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: r.success, error: r.error }));
      return;
    }

    const path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) { res.writeHead(403); res.end(); return; }
    const data = await readFile(join(__dirname, "public", path));
    res.writeHead(200, { "content-type": MIME[extname(path)] ?? "application/octet-stream" });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") { res.writeHead(404); res.end("not found"); }
    else { res.writeHead(500); res.end(JSON.stringify({ error: err.message })); }
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`board demo: http://localhost:${PORT}`);
  console.log(`gRPC:       ${GRPC_TARGET}  ·  realtime ws: :${WS_PORT}`);
});
