// OriginLands web server — a dependency-free bridge.
//
// Everything the browser needs rides one same-origin websocket: it subscribes
// to AoI changefeeds AND calls reducers (createCharacter/login/move) over the
// same socket (call_reducer). So this server only has to:
//   1. serve the static client in public/
//   2. transparently proxy the ws Upgrade through to OriginDB's websocket
//
//   browser <--wss://host/ ---(proxy)---> OriginDB ws  (subscribe + call_reducer)
//
// Usage: node server.js [--port 9099] [--ws-port 8791]
import http from "node:http";
import net from "node:net";
import { readFile } from "node:fs/promises";
import { extname, join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf(n); return i >= 0 && argv[i + 1] ? argv[i + 1] : d; };

const PORT = parseInt(flag("--port", "9099"), 10);
const WS_PORT = parseInt(flag("--ws-port", "8791"), 10);
const MODULE = "originlands";
const CLIENT_TOKEN = flag("--token", process.env.ORIGINDB_TOKEN || "");

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml", ".png": "image/png", ".jpg": "image/jpeg",
  ".json": "application/json; charset=utf-8", ".wasm": "application/wasm",
};

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/api/config") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ module: MODULE, wsPort: WS_PORT, token: CLIENT_TOKEN }));
      return;
    }

    let path = url.pathname === "/" ? "/index.html" : url.pathname;
    if (path.includes("..")) { res.writeHead(403); res.end(); return; }
    const file = join(__dirname, "public", path);
    const data = await readFile(file);
    const isHtml = extname(file) === ".html";
    res.writeHead(200, {
      "content-type": MIME[extname(file)] ?? "application/octet-stream",
      "cache-control": isHtml ? "no-store, must-revalidate" : "no-cache",
    });
    res.end(data);
  } catch (err) {
    if (err.code === "ENOENT") { res.writeHead(404); res.end("not found"); }
    else { console.error(err.message); res.writeHead(500); res.end(err.message); }
  }
});

// Transparent ws proxy: replay the handshake to OriginDB's raw ws socket and
// pipe frames both ways. One public origin serves the page AND realtime.
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
  console.log(`OriginLands:  http://localhost:${PORT}`);
  console.log(`realtime ws:  proxied at ws://localhost:${PORT}/  ->  :${WS_PORT}`);
});
