// Collab demo web server.
//
// Serves the static frontend on :9090 and bridges browser HTTP calls to the
// OriginDB gRPC WasmService (browsers cannot speak raw gRPC). Realtime data
// flows the other way: the browser subscribes directly to OriginDB's
// websocket (:8080) and receives WHERE-filtered changefeed events.
//
//   browser --POST /api/call--> this server --gRPC ExecuteReducer--> OriginDB
//   browser <--ws sql_changefeed_event------------------------------ OriginDB
//
// Usage: node server.js [--port 9090] [--grpc localhost:50051] [--ws-port 8080]

import http from "node:http";
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
const PORT = parseInt(flag("--port", "9090"), 10);
const GRPC_TARGET = flag("--grpc", "localhost:50051");
const WS_PORT = parseInt(flag("--ws-port", "8080"), 10);
const MODULE = "collab";

// --- gRPC client -------------------------------------------------------------

const protoPath = resolve(__dirname, "../../proto/origindb.proto");
const packageDef = protoLoader.loadSync(protoPath, {
  keepCase: true,
  longs: String,
  defaults: true,
});
const proto = grpc.loadPackageDefinition(packageDef).origindb.grpc;
const wasm = new proto.WasmService(GRPC_TARGET, grpc.credentials.createInsecure());

function toWasmValue(v) {
  if (typeof v === "boolean") return { bool_value: v };
  if (typeof v === "number")
    return Number.isInteger(v) ? { int64_value: v } : { double_value: v };
  if (typeof v === "string") return { string_value: v };
  return { string_value: JSON.stringify(v) };
}

function executeReducer(reducer, argsList) {
  return new Promise((resolvePromise, reject) => {
    wasm.ExecuteReducer(
      {
        module_name: MODULE,
        reducer_name: reducer,
        sender_identity: "collab-web",
        args: argsList.map(toWasmValue),
      },
      { deadline: Date.now() + 5000 },
      (err, response) => (err ? reject(err) : resolvePromise(response)),
    );
  });
}

// --- HTTP server ---------------------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".woff2": "font/woff2",
};

// Reducers the frontend may invoke.
const ALLOWED = new Set([
  "join", "moveCursor", "addNote", "removeNote", "clearBoard",
  "moveNote", "editNote", "sendChat",
]);

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ wsPort: WS_PORT, module: MODULE }));
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/call") {
      let body = "";
      for await (const chunk of req) {
        body += chunk;
        if (body.length > 64 * 1024) throw new Error("payload too large");
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

    // Static files
    let path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) {
      res.writeHead(403);
      res.end();
      return;
    }
    const file = join(__dirname, "public", path);
    const data = await readFile(file);
    res.writeHead(200, {
      "content-type": MIME[extname(file)] ?? "application/octet-stream",
      "cache-control": "no-cache",
    });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") {
      res.writeHead(404);
      res.end("not found");
    } else {
      console.error(err.message);
      res.writeHead(500, { "content-type": "application/json" });
      res.end(JSON.stringify({ success: false, error: err.message }));
    }
  }
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`collab demo:  http://localhost:${PORT}`);
  console.log(`gRPC target:  ${GRPC_TARGET}`);
  console.log(`realtime ws:  ws://<this-host>:${WS_PORT}`);
});
