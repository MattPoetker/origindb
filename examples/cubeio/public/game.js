// cube.io client. Server-authoritative: we send movement intent, OriginDB's
// `tick` reducer runs the sim, and we render whatever the changefeed streams
// back (interpolated for smoothness).

const RADIUS_K = 4.0;            // must match the module
const FOOD_R = 10.0;
const BASE_MASS = 12.0;
let WORLD = 12000;

const radius = (m) => Math.sqrt(Math.max(m, 1)) * RADIUS_K;

// ---- identity: stable session id remembered across visits -------------------
let session = localStorage.getItem("cubeio_session");
if (!session) {
  session = "s_" + Math.random().toString(36).slice(2) + Date.now().toString(36);
  localStorage.setItem("cubeio_session", session);
}

const COLORS = ["#38f5c8", "#ff5d73", "#ffd23f", "#7c8cff", "#ff8f3f", "#41d6ff", "#c56bff", "#6bff8a"];
let chosenColor = localStorage.getItem("cubeio_color") || COLORS[0];
let myName = localStorage.getItem("cubeio_name") || "";

// ---- state ------------------------------------------------------------------
const players = new Map();  // id -> {x,y,rx,ry,mass,name,color,alive}
const food = new Map();     // id -> {x,y,v}
let cfg = { wsPort: 8788, token: "", world: 12000 };
let alive = false;
let joined = false;

const canvas = document.getElementById("game");
const ctx = canvas.getContext("2d");
function resize() { canvas.width = innerWidth; canvas.height = innerHeight; }
addEventListener("resize", resize); resize();

// ---- networking -------------------------------------------------------------
async function apiCall(reducer, args) {
  try {
    const r = await fetch("/api/call", {
      method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ reducer, args }),
    });
    return await r.json();
  } catch { return { success: false }; }
}

function applyRow(table, key, cols) {
  if (!cols) return;
  if (table === "players") {
    const prev = players.get(key);
    const p = prev || { rx: cols.x, ry: cols.y };
    p.x = cols.x; p.y = cols.y; p.mass = cols.mass;
    p.name = cols.name; p.color = cols.color;
    p.alive = cols.alive === true || cols.alive === 1;
    p.boost = cols.boost === true || cols.boost === 1;
    if (prev === undefined) { p.rx = cols.x; p.ry = cols.y; }
    players.set(key, p);
    if (key === session) onSelfUpdate(p);
  } else if (table === "food") {
    food.set(key, { x: cols.x, y: cols.y, v: cols.v });
  }
}

function connectWs() {
  const q = cfg.token ? `?token=${encodeURIComponent(cfg.token)}` : "";
  const ws = new WebSocket(`ws://${location.hostname}:${cfg.wsPort}${q}`);
  const conn = document.getElementById("conn");
  ws.onopen = () => {
    conn.textContent = "● live"; conn.classList.add("ok");
    for (const t of ["players", "food"])
      ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM ${t}` }));
  };
  ws.onclose = () => { conn.textContent = "reconnecting…"; conn.classList.remove("ok"); setTimeout(connectWs, 1200); };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type === "initial_state") {
      const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1];
      for (const row of msg.rows || []) applyRow(table, row.key, row.data);
    } else if (msg.type === "sql_changefeed_event") {
      const table = msg.table;
      if (msg.operation === "DELETE") {
        if (table === "food") food.delete(msg.key);
        else if (table === "players") players.delete(msg.key);
        return;
      }
      try { const parsed = JSON.parse(msg.new_value); applyRow(table, msg.key, parsed.columns ?? parsed); }
      catch { /* ignore */ }
    }
  };
}

function onSelfUpdate(p) {
  if (!p.alive && alive && joined) {
    // We just got eaten.
    alive = false;
    document.getElementById("finalMass").textContent = Math.round(p.mass || 0) || Math.round(p.score || 0);
    document.getElementById("death").classList.remove("hidden");
  }
}

// ---- input ------------------------------------------------------------------
let mouse = { x: innerWidth / 2, y: innerHeight / 2 };
let boosting = false;
addEventListener("mousemove", (e) => { mouse.x = e.clientX; mouse.y = e.clientY; });
addEventListener("mousedown", () => { if (alive) setBoost(true); });
addEventListener("mouseup", () => setBoost(false));
addEventListener("keydown", (e) => { if (e.code === "Space" && alive) { e.preventDefault(); setBoost(true); } });
addEventListener("keyup", (e) => { if (e.code === "Space") setBoost(false); });

let boostDirty = false;
function setBoost(on) { if (boosting !== on) { boosting = on; boostDirty = true; } }

let lastDir = { x: 0, y: 0 };
function currentDir() {
  const dx = mouse.x - canvas.width / 2;
  const dy = mouse.y - canvas.height / 2;
  const len = Math.hypot(dx, dy);
  if (len < 8) return { x: 0, y: 0 };          // dead zone near center = stop
  return { x: dx / len, y: dy / len };
}

// Send input at ~12 Hz, or immediately when boost toggles.
setInterval(() => {
  if (!alive) return;
  const d = currentDir();
  const moved = Math.abs(d.x - lastDir.x) > 0.02 || Math.abs(d.y - lastDir.y) > 0.02;
  if (moved || boostDirty) {
    lastDir = d; boostDirty = false;
    apiCall("input", [session, d.x, d.y, boosting]);
  }
}, 80);

// ---- join / respawn ---------------------------------------------------------
async function doJoin() {
  myName = (document.getElementById("name").value || "").trim() || "cube";
  localStorage.setItem("cubeio_name", myName);
  localStorage.setItem("cubeio_color", chosenColor);
  const res = await apiCall("join", [session, myName, chosenColor]);
  if (res && res.success !== false) {
    joined = true; alive = true;
    document.getElementById("join").classList.add("hidden");
    document.getElementById("death").classList.add("hidden");
  }
}

// ---- render -----------------------------------------------------------------
let camX = WORLD / 2, camY = WORLD / 2, zoom = 0.12;

function draw() {
  requestAnimationFrame(draw);
  const W = canvas.width, H = canvas.height;
  ctx.fillStyle = "#070a12";
  ctx.fillRect(0, 0, W, H);

  // Interpolate everyone toward their latest server position.
  for (const p of players.values()) {
    p.rx += (p.x - p.rx) * 0.25;
    p.ry += (p.y - p.ry) * 0.25;
  }

  const me = players.get(session);
  let myMass = BASE_MASS;
  if (me && me.alive) {
    myMass = me.mass;
    camX += (me.rx - camX) * 0.15;
    camY += (me.ry - camY) * 0.15;
  }
  // Zoom out as you grow.
  const targetZoom = Math.min(0.26, Math.max(0.045, (H / 1100) * Math.pow(BASE_MASS / myMass, 0.28)));
  zoom += (targetZoom - zoom) * 0.08;

  const sx = (wx) => (wx - camX) * zoom + W / 2;
  const sy = (wy) => (wy - camY) * zoom + H / 2;

  // world bounds + grid
  drawGrid(sx, sy, W, H);

  // food (glowing)
  const fr = Math.max(2, FOOD_R * zoom);
  for (const f of food.values()) {
    const x = sx(f.x), y = sy(f.y);
    if (x < -20 || y < -20 || x > W + 20 || y > H + 20) continue;
    ctx.shadowBlur = 12; ctx.shadowColor = "#39d7ff";
    ctx.fillStyle = "#8ff0ff";
    ctx.fillRect(x - fr, y - fr, fr * 2, fr * 2);
  }
  ctx.shadowBlur = 0;

  // players (cubes), small first so big ones draw on top
  const list = [...players.values()].filter((p) => p.alive).sort((a, b) => a.mass - b.mass);
  for (const p of list) {
    const r = radius(p.mass) * zoom;
    const x = sx(p.rx), y = sy(p.ry);
    if (x < -r - 40 || y < -r - 40 || x > W + r + 40 || y > H + r + 40) continue;
    const isMe = players.get(session) === p;

    ctx.shadowBlur = p.boost ? 26 : 14;
    ctx.shadowColor = p.color;
    roundRect(x - r, y - r, r * 2, r * 2, Math.min(8, r * 0.28));
    ctx.fillStyle = p.color; ctx.fill();
    ctx.shadowBlur = 0;
    // inner face
    ctx.fillStyle = "rgba(255,255,255,.12)";
    roundRect(x - r, y - r, r * 2, r * 0.7, Math.min(8, r * 0.28)); ctx.fill();
    // outline for self
    if (isMe) { ctx.lineWidth = 2; ctx.strokeStyle = "#ffffffcc"; roundRect(x - r, y - r, r * 2, r * 2, Math.min(8, r * 0.28)); ctx.stroke(); }

    // label
    if (r > 14) {
      ctx.fillStyle = "#00000088";
      ctx.font = `${Math.max(11, Math.min(r * 0.5, 26))}px ui-monospace, monospace`;
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      ctx.fillText(p.name || "cube", x, y - 1);
      ctx.fillStyle = "#ffffffee";
      ctx.fillText(p.name || "cube", x, y - 2);
      ctx.fillStyle = "#ffffffaa";
      ctx.font = `${Math.max(9, Math.min(r * 0.34, 16))}px ui-monospace, monospace`;
      ctx.fillText(Math.round(p.mass), x, y + r * 0.5);
    }
  }
  ctx.shadowBlur = 0;

  updateHud(me);
}

function drawGrid(sx, sy, W, H) {
  ctx.strokeStyle = "#0e1626"; ctx.lineWidth = 1;
  const step = 300;
  const x0 = Math.floor((camX - W / 2 / zoom) / step) * step;
  const y0 = Math.floor((camY - H / 2 / zoom) / step) * step;
  ctx.beginPath();
  for (let x = x0; x < camX + W / 2 / zoom; x += step) { ctx.moveTo(sx(x), 0); ctx.lineTo(sx(x), H); }
  for (let y = y0; y < camY + H / 2 / zoom; y += step) { ctx.moveTo(0, sy(y)); ctx.lineTo(W, sy(y)); }
  ctx.stroke();
  // arena border
  ctx.strokeStyle = "#26406a"; ctx.lineWidth = 3;
  ctx.strokeRect(sx(0), sy(0), WORLD * zoom, WORLD * zoom);
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function updateHud(me) {
  const score = document.getElementById("score");
  score.innerHTML = me ? `mass <b>${Math.round(me.mass)}</b>` : "";
  const board = document.getElementById("board");
  const top = [...players.values()].filter((p) => p.alive).sort((a, b) => b.mass - a.mass).slice(0, 6);
  board.innerHTML = `<div class="t">LEADERBOARD</div>` + top.map((p) => {
    const meCls = players.get(session) === p ? " me" : "";
    const nm = (p.name || "cube").slice(0, 12);
    return `<div class="row${meCls}">${nm} <span class="m">${Math.round(p.mass)}</span></div>`;
  }).join("");
}

// ---- boot -------------------------------------------------------------------
(async function boot() {
  try { cfg = await fetch("/api/config").then((r) => r.json()); } catch {}
  WORLD = cfg.world || WORLD;
  camX = WORLD / 2; camY = WORLD / 2;
  connectWs();
  draw();

  // color swatches
  const sw = document.getElementById("swatches");
  COLORS.forEach((c) => {
    const el = document.createElement("div");
    el.className = "sw" + (c === chosenColor ? " sel" : "");
    el.style.background = c; el.style.color = c;
    el.onclick = () => { chosenColor = c; [...sw.children].forEach((n) => n.classList.remove("sel")); el.classList.add("sel"); };
    sw.appendChild(el);
  });
  document.getElementById("name").value = myName;
  document.getElementById("playBtn").onclick = doJoin;
  document.getElementById("respawnBtn").onclick = doJoin;
  document.getElementById("name").addEventListener("keydown", (e) => { if (e.key === "Enter") doJoin(); });
})();

// Best-effort: tell the server we left (marks the cube dead so it stops moving).
addEventListener("beforeunload", () => {
  navigator.sendBeacon?.("/api/call", JSON.stringify({ reducer: "leave", args: [session] }));
});
