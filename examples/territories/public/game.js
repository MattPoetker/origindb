// Territories client. Server-authoritative RTS: we issue orders (move / gather /
// attack / build / train), OriginDB's `tick` reducer runs the sim on active
// chunks, and we render whatever the changefeed streams back (interpolated).
//
// The base terrain is PROCEDURAL — terrain() below is byte-identical to the
// module's terrain() (integer value-noise, u32 math via Math.imul), so the huge
// world renders with zero server round-trips and no tile rows are ever stored.

// ---- config (from /api/config; seed/world/chunk must match the module) ------
let SEED = 1337, WORLD = 6000, CHUNK = 100;
let RS = 20, REGS = 3;              // region size (chunks) + regions per side
let cfg = { wsPort: 8788, token: "" };

// region of a chunk coord; orders route to the module owning that region
const regOf = (cx, cy) => [Math.floor(cx / RS), Math.floor(cy / RS)];

// ---- deterministic terrain (mirror of examples/territories index.ts) --------
function hash2(ix, iy) {
  let h = (Math.imul(ix, 374761393) + Math.imul(iy, 668265263) + Math.imul(SEED, 1442695040)) >>> 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177) >>> 0;
  h = (h ^ (h >>> 16)) >>> 0;
  return (h & 0xffffff) / 16777216.0;
}
function smooth(t) { return t * t * (3.0 - 2.0 * t); }
function valueNoise(x, y) {
  const x0 = Math.floor(x), y0 = Math.floor(y);
  const fx = smooth(x - x0), fy = smooth(y - y0);
  const a = hash2(x0, y0), b = hash2(x0 + 1, y0), c = hash2(x0, y0 + 1), d = hash2(x0 + 1, y0 + 1);
  const top = a + (b - a) * fx, bot = c + (d - c) * fx;
  return top + (bot - top) * fy;
}
function terrain(tx, ty) {
  const e = valueNoise(tx / 260, ty / 260) * 0.6 + valueNoise(tx / 90, ty / 90) * 0.3 + valueNoise(tx / 30, ty / 30) * 0.1;
  const m = valueNoise(tx / 200 + 100, ty / 200 - 40);
  if (e < 0.34) return 0;             // water
  if (e < 0.38) return 1;             // sand
  if (e > 0.80) return 4;             // mountain
  if (m > 0.62 && e < 0.72) return 3; // forest
  return 2;                           // grass
}
const TERRAIN_COL = ["#14344f", "#c9b779", "#3e6b3a", "#28502e", "#6f6a63"];
const TERRAIN_COL2 = ["#0f2740", "#b6a469", "#356030", "#213f25", "#5c5850"];
function buildableTile(tx, ty) { const t = terrain(tx, ty); return t === 1 || t === 2 || t === 3; }

// ---- identity ---------------------------------------------------------------
let session = localStorage.getItem("terr_session");
if (!session) { session = "s_" + Math.random().toString(36).slice(2) + Date.now().toString(36); localStorage.setItem("terr_session", session); }
const COLORS = ["#41d6ff", "#ff5d73", "#ffd23f", "#8a7cff", "#ff8f3f", "#6bff8a", "#c56bff", "#ff6bd0"];
let myColor = localStorage.getItem("terr_color") || COLORS[0];
let myName = localStorage.getItem("terr_name") || "";

// ---- game state (streamed from changefeed) ----------------------------------
const players = new Map();    // session -> {gold,wood,food,hqx,hqy,name,color,alive}
const units = new Map();      // id -> {x,y,rx,ry,owner,kind,hp,maxHp,cmd,cx,cy}
const buildings = new Map();  // id -> {x,y,owner,kind,hp,maxHp,done,cx,cy}
const resources = new Map();  // id -> {x,y,kind,amt}
const chunks = new Map();     // "cx:cy" -> {owner}

let joined = false;
const canvas = document.getElementById("game");
const ctx = canvas.getContext("2d");
function resize() { canvas.width = innerWidth; canvas.height = innerHeight; }
addEventListener("resize", resize); resize();

// ---- camera -----------------------------------------------------------------
let camX = WORLD / 2, camY = WORLD / 2;   // tile coords at screen center
let zoom = 12;                            // pixels per tile
const ZMIN = 3.5, ZMAX = 34;
const keys = new Set();
addEventListener("keydown", (e) => {
  if (["KeyW", "KeyA", "KeyS", "KeyD", "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.code)) keys.add(e.code);
  if (e.code === "Escape") { buildMode = null; refreshPalette(); }
});
addEventListener("keyup", (e) => keys.delete(e.code));
addEventListener("wheel", (e) => {
  const before = screenToWorld(e.clientX, e.clientY);
  zoom = Math.max(ZMIN, Math.min(ZMAX, zoom * (e.deltaY < 0 ? 1.12 : 0.89)));
  const after = screenToWorld(e.clientX, e.clientY);
  camX += before.x - after.x; camY += before.y - after.y;   // zoom toward cursor
}, { passive: true });

function sx(tx) { return (tx - camX) * zoom + canvas.width / 2; }
function sy(ty) { return (ty - camY) * zoom + canvas.height / 2; }
function screenToWorld(px, py) { return { x: (px - canvas.width / 2) / zoom + camX, y: (py - canvas.height / 2) / zoom + camY }; }

// ---- networking -------------------------------------------------------------
// rx/ry pick the region shard the order routes to (omit for spawn/leave).
async function apiCall(reducer, cargs, rx, ry) {
  try {
    const r = await fetch("/api/call", { method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ reducer, args: cargs, rx, ry }) });
    return await r.json();
  } catch { return { success: false }; }
}

// row keys — carry cx:cy so the server does an O(1) readTable, no scan
const unitKey = (id, u) => `u:${u.cx}:${u.cy}:${id}`;
const bldgKey = (id, b) => `b:${b.cx}:${b.cy}:${id}`;
const resKey  = (id, r) => `r:${r.cx}:${r.cy}:${id}`;

function idFromKey(key) { const i = key.lastIndexOf(":"); return i < 0 ? key : key.slice(i + 1); }

function applyRow(table, key, cols) {
  if (!cols) return;
  if (table === "players") {
    players.set(key, cols);
    if (key === session) onSelf(cols);
  } else if (table === "units") {
    const id = cols.id || idFromKey(key);
    const prev = units.get(id);
    const u = prev || { rx: cols.x, ry: cols.y };
    Object.assign(u, cols);
    if (!prev) { u.rx = cols.x; u.ry = cols.y; }
    units.set(id, u);
  } else if (table === "buildings") {
    buildings.set(cols.id || idFromKey(key), cols);
  } else if (table === "resources") {
    resources.set(cols.id || idFromKey(key), cols);
  } else if (table === "chunks") {
    chunks.set(`${cols.cx}:${cols.cy}`, cols);
  }
}

// ---- area-of-interest (AOI) subscriptions -----------------------------------
// Instead of subscribing to the whole world, each client subscribes only to the
// chunks in its viewport (WHERE cx/cy BETWEEN ...) and RE-subscribes when it
// pans, dropping the old window. So the server only ever streams a client the
// few rows it can see — the mechanism that lets thousands of empires coexist.
let sock = null;
const AOI_TABLES = ["units", "buildings", "resources"];
const aoiSub = { units: null, buildings: null, resources: null };  // active sub ids
let aoiBox = null;                                                  // {cx0,cx1,cy0,cy1}
const AOI_PAD = 2;                                                  // chunks of overscan

function viewportChunks() {
  const W = canvas.width, H = canvas.height;
  const cx0 = Math.max(0, Math.floor((camX - W / 2 / zoom) / CHUNK) - AOI_PAD);
  const cx1 = Math.min(NCHUNKS - 1, Math.floor((camX + W / 2 / zoom) / CHUNK) + AOI_PAD);
  const cy0 = Math.max(0, Math.floor((camY - H / 2 / zoom) / CHUNK) - AOI_PAD);
  const cy1 = Math.min(NCHUNKS - 1, Math.floor((camY + H / 2 / zoom) / CHUNK) + AOI_PAD);
  return { cx0, cx1, cy0, cy1 };
}
const NCHUNKS = 60;   // world chunks per side (matches server)

function refreshAOI() {
  if (!sock || sock.readyState !== 1) return;
  const b = viewportChunks();
  if (aoiBox && b.cx0 === aoiBox.cx0 && b.cx1 === aoiBox.cx1 && b.cy0 === aoiBox.cy0 && b.cy1 === aoiBox.cy1) return;
  aoiBox = b;
  const where = `cx >= ${b.cx0} AND cx <= ${b.cx1} AND cy >= ${b.cy0} AND cy <= ${b.cy1}`;
  for (const t of AOI_TABLES)
    sock.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM ${t} WHERE ${where}` }));
}

// prune entities that have left the AOI (server no longer streams their updates)
function pruneAOI() {
  if (!aoiBox) return;
  const inBox = (cx, cy) => cx >= aoiBox.cx0 - 1 && cx <= aoiBox.cx1 + 1 && cy >= aoiBox.cy0 - 1 && cy <= aoiBox.cy1 + 1;
  for (const [id, u] of units) if (id !== "" && !inBox(u.cx | 0, u.cy | 0) && id !== session) units.delete(id);
  for (const [id, b] of buildings) if (!inBox(b.cx | 0, b.cy | 0)) buildings.delete(id);
  for (const [id, r] of resources) if (!inBox(r.cx | 0, r.cy | 0)) resources.delete(id);
}

function connectWs() {
  const q = cfg.token ? `?token=${encodeURIComponent(cfg.token)}` : "";
  const ws = new WebSocket(`ws://${location.hostname}:${cfg.wsPort}${q}`);
  sock = ws;
  const conn = document.getElementById("conn");
  ws.onopen = () => {
    conn.textContent = "● live"; conn.classList.add("ok");
    // own player row (HUD/resources) — only mine, not everyone's
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM players WHERE id = '${session}'` }));
    // chunk ownership for the minimap — small + low update rate, kept global
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM chunks` }));
    // viewport slices for units/buildings/resources
    aoiBox = null; refreshAOI();
  };
  ws.onclose = () => { conn.textContent = "reconnecting…"; conn.classList.remove("ok"); sock = null; setTimeout(connectWs, 1200); };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type === "initial_state") {
      const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1];
      for (const row of msg.rows || []) applyRow(table, row.key, row.data);
    } else if (msg.type === "sql_subscription_created") {
      // swap in the new viewport sub for this table and drop the previous one
      const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1];
      if (AOI_TABLES.includes(table)) {
        const old = aoiSub[table];
        aoiSub[table] = msg.subscription_id;
        if (old && old !== msg.subscription_id) ws.send(JSON.stringify({ type: "sql_unsubscribe", subscription_id: old }));
      }
    } else if (msg.type === "sql_changefeed_event") {
      const table = msg.table;
      if (msg.operation === "DELETE") {
        if (table === "units") units.delete(idFromKey(msg.key));
        else if (table === "buildings") buildings.delete(idFromKey(msg.key));
        else if (table === "resources") resources.delete(idFromKey(msg.key));
        else if (table === "players") players.delete(msg.key);
        else if (table === "chunks") chunks.delete(msg.key.slice(2));
        return;
      }
      try { const p = JSON.parse(msg.new_value); applyRow(table, msg.key, p.columns ?? p); } catch {}
    }
  };
}

let eliminated = false;
function onSelf(cols) {
  document.getElementById("rGold").textContent = Math.floor(cols.gold || 0);
  document.getElementById("rWood").textContent = Math.floor(cols.wood || 0);
  document.getElementById("rFood").textContent = Math.floor(cols.food || 0);
  const alive = cols.alive === true || cols.alive === 1;
  if (joined && !alive && !eliminated) {
    // HQ destroyed → empire eliminated. Show the deploy screen to redeploy fresh.
    eliminated = true; joined = false;
    selUnits.clear(); selBuilding = null;
    showToast("Your HQ was destroyed — empire eliminated");
    const s = document.getElementById("start");
    if (s) { s.classList.remove("hidden"); const btn = document.getElementById("playBtn"); if (btn) btn.textContent = "REDEPLOY EMPIRE"; }
  }
}

// ---- selection + orders -----------------------------------------------------
let selUnits = new Set();     // selected own unit ids
let selBuilding = null;       // selected own building id
let buildMode = null;         // building kind pending placement
let dragging = null;          // {x0,y0,x1,y1} selection box in screen px
let mouse = { x: 0, y: 0 };

addEventListener("mousemove", (e) => { mouse.x = e.clientX; mouse.y = e.clientY; if (dragging) { dragging.x1 = e.clientX; dragging.y1 = e.clientY; } });

canvas.addEventListener("mousedown", (e) => {
  if (!joined) return;
  if (e.button === 0) {
    if (buildMode) { placeBuilding(); return; }
    dragging = { x0: e.clientX, y0: e.clientY, x1: e.clientX, y1: e.clientY };
  } else if (e.button === 2) {
    e.preventDefault();
    issueCommand(e.clientX, e.clientY);
  }
});
canvas.addEventListener("mouseup", (e) => {
  if (e.button !== 0 || !dragging) return;
  const dx = Math.abs(dragging.x1 - dragging.x0), dy = Math.abs(dragging.y1 - dragging.y0);
  if (dx < 5 && dy < 5) clickSelect(e.clientX, e.clientY);
  else boxSelect();
  dragging = null;
});
canvas.addEventListener("contextmenu", (e) => e.preventDefault());

function clickSelect(px, py) {
  selBuilding = null; selUnits.clear();
  const w = screenToWorld(px, py);
  // Prefer a nearby own unit, else an own building.
  let best = null, bestD = 2.6 * 2.6;
  for (const [id, u] of units) {
    if (u.owner !== session) continue;
    const d = (u.rx - w.x) ** 2 + (u.ry - w.y) ** 2;
    if (d < bestD) { bestD = d; best = id; }
  }
  if (best) { selUnits.add(best); updateSelPanel(); return; }
  let bb = null, bd = 3.2 * 3.2;
  for (const [id, b] of buildings) {
    if (b.owner !== session) continue;
    const d = (b.x - w.x) ** 2 + (b.y - w.y) ** 2;
    if (d < bd) { bd = d; bb = id; }
  }
  selBuilding = bb;
  updateSelPanel();
}

function boxSelect() {
  selBuilding = null; selUnits.clear();
  const a = screenToWorld(Math.min(dragging.x0, dragging.x1), Math.min(dragging.y0, dragging.y1));
  const b = screenToWorld(Math.max(dragging.x0, dragging.x1), Math.max(dragging.y0, dragging.y1));
  for (const [id, u] of units) {
    if (u.owner !== session) continue;
    if (u.rx >= a.x && u.rx <= b.x && u.ry >= a.y && u.ry <= b.y) selUnits.add(id);
  }
  updateSelPanel();
}

// Group the selected units by region → Map "rx,ry" -> [rowKey,...], so each
// region's units are commanded on the shard that owns (and ticks) them.
function selByRegion() {
  const groups = new Map();
  for (const id of selUnits) {
    const u = units.get(id); if (!u) continue;
    const [rx, ry] = regOf(u.cx, u.cy);
    const k = `${rx},${ry}`;
    (groups.get(k) || groups.set(k, { rx, ry, keys: [] }).get(k)).keys.push(unitKey(id, u));
  }
  return [...groups.values()];
}

function issueCommand(px, py) {
  const w = screenToWorld(px, py);
  // Right-click on own producer building with no units selected = collect.
  if (selUnits.size === 0) {
    const bId = buildingAt(w.x, w.y);
    const b = bId && buildings.get(bId);
    if (b && b.owner === session && (b.kind === "mine" || b.kind === "farm")) {
      const [rx, ry] = regOf(b.cx, b.cy);
      apiCall("collect", [session, bldgKey(bId, b)], rx, ry).then(showResult);
    }
    return;
  }
  const groups = selByRegion();
  const resId = resourceAt(w.x, w.y);
  const enemyId = enemyUnitAt(w.x, w.y);
  const ebId = enemyBuildingAt(w.x, w.y);
  const res = resId && resources.get(resId);
  const enemy = enemyId && units.get(enemyId);
  const ebld = ebId && buildings.get(ebId);
  // priority: gather node > attack enemy unit > attack enemy base/building > move
  const targetKey = enemy ? unitKey(enemyId, enemy) : ebld ? bldgKey(ebId, ebld) : null;
  for (const g of groups) {
    const keysCsv = g.keys.join(",");
    if (res) apiCall("gather", [session, keysCsv, resKey(resId, res)], g.rx, g.ry);
    else if (targetKey) apiCall("attack", [session, keysCsv, targetKey], g.rx, g.ry);
    else apiCall("moveUnits", [session, keysCsv, w.x, w.y], g.rx, g.ry);
  }
  pingWorld(w.x, w.y, res ? "#7ec86a" : targetKey ? "#ff5d73" : "#41d6ff");
}

function resourceAt(x, y) { let best = null, bd = 2.2 * 2.2; for (const [id, r] of resources) { const d = (r.x - x) ** 2 + (r.y - y) ** 2; if (d < bd) { bd = d; best = id; } } return best; }
function enemyUnitAt(x, y) { let best = null, bd = 2.2 * 2.2; for (const [id, u] of units) { if (u.owner === session) continue; const d = (u.rx - x) ** 2 + (u.ry - y) ** 2; if (d < bd) { bd = d; best = id; } } return best; }
function enemyBuildingAt(x, y) { let best = null, bd = 4 * 4; for (const [id, b] of buildings) { if (b.owner === session) continue; const d = (b.x - x) ** 2 + (b.y - y) ** 2; if (d < bd) { bd = d; best = id; } } return best; }
function buildingAt(x, y) { let best = null, bd = 3.2 * 3.2; for (const [id, b] of buildings) { const d = (b.x - x) ** 2 + (b.y - y) ** 2; if (d < bd) { bd = d; best = id; } } return best; }

// ---- build placement --------------------------------------------------------
document.querySelectorAll(".pbtn").forEach((btn) => {
  btn.onclick = () => { buildMode = buildMode === btn.dataset.build ? null : btn.dataset.build; refreshPalette(); };
});
function refreshPalette() { document.querySelectorAll(".pbtn").forEach((b) => b.classList.toggle("sel", b.dataset.build === buildMode)); }

async function placeBuilding() {
  const w = screenToWorld(mouse.x, mouse.y);
  if (!buildableTile(w.x, w.y)) { showToast("can't build on water or mountains"); return; }
  const [rx, ry] = regOf(Math.floor(w.x / CHUNK), Math.floor(w.y / CHUNK));
  const res = await apiCall("build", [session, buildMode, w.x, w.y], rx, ry);
  showResult(res);
  buildMode = null; refreshPalette();
}

// ---- selection panel --------------------------------------------------------
function updateSelPanel() {
  const el = document.getElementById("sel");
  if (selUnits.size > 0) {
    let workers = 0, soldiers = 0;
    for (const id of selUnits) { const u = units.get(id); if (!u) continue; u.kind === "soldier" ? soldiers++ : workers++; }
    el.style.display = "block";
    el.innerHTML = `<h4>SELECTION</h4><div class="meta">${workers} worker${workers !== 1 ? "s" : ""} · ${soldiers} soldier${soldiers !== 1 ? "s" : ""}</div>
      <div class="meta" style="margin-top:6px">right-click to move · gather · attack</div>`;
  } else if (selBuilding) {
    const b = buildings.get(selBuilding);
    if (!b) { el.style.display = "none"; return; }
    el.style.display = "block";
    let html = `<h4>${(b.kind || "").toUpperCase()}</h4><div class="meta">hp ${Math.round(b.hp)}/${Math.round(b.maxHp)}${b.done ? "" : " · building…"}</div>`;
    if (b.done && b.kind === "hq") html += `<button class="sbtn" data-act="train-worker">Train worker <span class="meta">(25 food)</span></button>`;
    if (b.done && b.kind === "barracks") html += `<button class="sbtn" data-act="train-soldier">Train soldier <span class="meta">(30 food · 20 gold)</span></button>`;
    if (b.done && (b.kind === "mine" || b.kind === "farm")) html += `<button class="sbtn" data-act="collect">Collect production</button>`;
    el.innerHTML = html;
    el.querySelectorAll(".sbtn").forEach((btn) => btn.onclick = () => selAction(btn.dataset.act));
  } else {
    el.style.display = "none";
  }
}
async function selAction(act) {
  if (!selBuilding) return;
  const b = buildings.get(selBuilding);
  if (!b) return;
  const key = bldgKey(selBuilding, b);
  const [rx, ry] = regOf(b.cx, b.cy);
  if (act === "train-worker") showResult(await apiCall("train", [session, key, "worker"], rx, ry));
  else if (act === "train-soldier") showResult(await apiCall("train", [session, key, "soldier"], rx, ry));
  else if (act === "collect") showResult(await apiCall("collect", [session, key], rx, ry));
}

// ---- feedback ---------------------------------------------------------------
let pings = [];
function pingWorld(x, y, color) { pings.push({ x, y, color, t: 1 }); }
let toastT = 0;
function showToast(m) { const el = document.getElementById("toast"); el.textContent = m; el.classList.add("show"); toastT = 90; }
function showResult(res) { if (res && res.result && res.result.ok === false && res.result.error) showToast(res.result.error); else if (res && res.success === false && res.error) showToast(res.error); }

// ---- join -------------------------------------------------------------------
async function doJoin() {
  myName = (document.getElementById("name").value || "").trim() || "Commander";
  localStorage.setItem("terr_name", myName); localStorage.setItem("terr_color", myColor);
  const res = await apiCall("spawnEmpire", [session, myName, "azure", myColor]);
  if (res && res.success !== false) {
    joined = true; eliminated = false;
    document.getElementById("start").classList.add("hidden");
    const r = res.result;
    if (r && typeof r.hqx === "number") { camX = r.hqx; camY = r.hqy; zoom = 16; }
  } else { showToast("could not deploy — is the server running?"); }
}

// ---- render loop ------------------------------------------------------------
function tickCamera() {
  const pan = 12 / zoom * 1.2;
  if (keys.has("KeyW") || keys.has("ArrowUp")) camY -= pan * 60 / 60;
  if (keys.has("KeyS") || keys.has("ArrowDown")) camY += pan;
  if (keys.has("KeyA") || keys.has("ArrowLeft")) camX -= pan;
  if (keys.has("KeyD") || keys.has("ArrowRight")) camX += pan;
  camX = Math.max(0, Math.min(WORLD, camX)); camY = Math.max(0, Math.min(WORLD, camY));
}

let aoiTick = 0;
function draw() {
  requestAnimationFrame(draw);
  tickCamera();
  // re-evaluate the viewport window a few times a second (re-subscribes only
  // when the chunk box actually changes) and drop entities that left view
  if ((aoiTick++ % 12) === 0) { refreshAOI(); pruneAOI(); }
  const W = canvas.width, H = canvas.height;

  // interpolate units toward latest server position
  for (const u of units.values()) { u.rx += (u.x - u.rx) * 0.25; u.ry += (u.y - u.ry) * 0.25; }

  drawTerrain(W, H);
  drawChunkBorders(W, H);
  drawResources();
  drawBuildings();
  drawUnits();
  drawPings();
  if (dragging) drawSelectionBox();
  drawMinimap();

  if (toastT > 0 && --toastT === 0) document.getElementById("toast").classList.remove("show");
}

function drawTerrain(W, H) {
  const tx0 = Math.floor(camX - W / 2 / zoom) - 1, tx1 = Math.ceil(camX + W / 2 / zoom) + 1;
  const ty0 = Math.floor(camY - H / 2 / zoom) - 1, ty1 = Math.ceil(camY + H / 2 / zoom) + 1;
  const step = Math.max(1, Math.ceil((tx1 - tx0) / 240));   // cap cells drawn
  const px = zoom * step + 1;
  for (let ty = ty0; ty <= ty1; ty += step) {
    for (let tx = tx0; tx <= tx1; tx += step) {
      const t = (tx < 0 || ty < 0 || tx >= WORLD || ty >= WORLD) ? 0 : terrain(tx, ty);
      // subtle checker for texture
      ctx.fillStyle = ((tx + ty) & (step << 1)) ? TERRAIN_COL[t] : TERRAIN_COL2[t];
      ctx.fillRect(sx(tx) - 0.5, sy(ty) - 0.5, px, px);
    }
  }
  // world edge
  ctx.strokeStyle = "#0a1420"; ctx.lineWidth = 3;
  ctx.strokeRect(sx(0), sy(0), WORLD * zoom, WORLD * zoom);
}

function drawChunkBorders(W, H) {
  if (zoom < 6) return;
  ctx.strokeStyle = "rgba(120,160,210,.06)"; ctx.lineWidth = 1;
  const cx0 = Math.floor((camX - W / 2 / zoom) / CHUNK) * CHUNK;
  const cy0 = Math.floor((camY - H / 2 / zoom) / CHUNK) * CHUNK;
  ctx.beginPath();
  for (let x = cx0; x < camX + W / 2 / zoom; x += CHUNK) { ctx.moveTo(sx(x), 0); ctx.lineTo(sx(x), H); }
  for (let y = cy0; y < camY + H / 2 / zoom; y += CHUNK) { ctx.moveTo(0, sy(y)); ctx.lineTo(W, sy(y)); }
  ctx.stroke();
}

function drawResources() {
  const s = Math.max(3, zoom * 0.9);
  for (const r of resources.values()) {
    const x = sx(r.x), y = sy(r.y);
    if (x < -20 || y < -20 || x > canvas.width + 20 || y > canvas.height + 20) continue;
    if (r.kind === "gold") {
      ctx.fillStyle = "#ffcf4d"; ctx.shadowBlur = 8; ctx.shadowColor = "#ffcf4d";
      ctx.beginPath(); ctx.moveTo(x, y - s); ctx.lineTo(x + s, y); ctx.lineTo(x, y + s); ctx.lineTo(x - s, y); ctx.closePath(); ctx.fill();
    } else {
      ctx.fillStyle = "#5fae52"; ctx.shadowBlur = 6; ctx.shadowColor = "#2f5d34";
      ctx.beginPath(); ctx.arc(x, y, s, 0, 7); ctx.fill();
    }
    ctx.shadowBlur = 0;
  }
}

// With AOI we no longer stream every player's row, so enemy colours are derived
// deterministically from the owner id (stable, distinct-ish) rather than looked up.
const _colorCache = new Map();
function hashColor(s) {
  if (_colorCache.has(s)) return _colorCache.get(s);
  let h = 2166136261; for (let i = 0; i < s.length; i++) { h = Math.imul(h ^ s.charCodeAt(i), 16777619); }
  const hue = (h >>> 0) % 360;
  const c = `hsl(${hue} 70% 60%)`;
  _colorCache.set(s, c); return c;
}
function ownerColor(owner) {
  if (owner === session) return myColor;
  const p = players.get(owner);
  return (p && p.color) || hashColor(owner || "?");
}

function drawBuildings() {
  for (const b of buildings.values()) {
    const x = sx(b.x), y = sy(b.y);
    const isHQ = b.kind === "hq";
    const s = (isHQ ? 2.4 : 1.7) * Math.max(4, zoom);
    if (x < -s - 20 || y < -s - 20 || x > canvas.width + s + 20 || y > canvas.height + s + 20) continue;
    const col = ownerColor(b.owner);
    ctx.globalAlpha = b.done ? 1 : 0.45;
    ctx.shadowBlur = isHQ ? 18 : 8; ctx.shadowColor = col;
    ctx.fillStyle = col;
    ctx.fillRect(x - s / 2, y - s / 2, s, s);
    ctx.shadowBlur = 0; ctx.globalAlpha = 1;
    ctx.strokeStyle = "rgba(0,0,0,.55)"; ctx.lineWidth = 1.5; ctx.strokeRect(x - s / 2, y - s / 2, s, s);
    // glyph
    if (zoom > 6) {
      ctx.fillStyle = "rgba(0,0,0,.7)"; ctx.font = `${Math.max(9, s * 0.5)}px ui-monospace, monospace`;
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      const g = isHQ ? "★" : b.kind === "barracks" ? "⚔" : b.kind === "mine" ? "◆" : "❦";
      ctx.fillText(g, x, y + 1);
    }
    if (b.done && b.hp < b.maxHp) drawHpBar(x, y - s / 2 - 5, s, b.hp / b.maxHp);
    if (selBuilding && buildings.get(selBuilding) === b) drawSelRing(x, y, s * 0.8);
  }
}

function drawUnits() {
  for (const [id, u] of units) {
    const x = sx(u.rx), y = sy(u.ry);
    const r = Math.max(2.5, zoom * (u.kind === "soldier" ? 0.42 : 0.34));
    if (x < -20 || y < -20 || x > canvas.width + 20 || y > canvas.height + 20) continue;
    const mine = u.owner === session;
    const col = ownerColor(u.owner);
    ctx.shadowBlur = mine ? 8 : 4; ctx.shadowColor = col;
    ctx.fillStyle = col;
    if (u.kind === "soldier") {
      ctx.beginPath(); ctx.moveTo(x, y - r); ctx.lineTo(x + r, y + r); ctx.lineTo(x - r, y + r); ctx.closePath(); ctx.fill();
    } else {
      ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fill();
    }
    ctx.shadowBlur = 0;
    ctx.strokeStyle = mine ? "rgba(255,255,255,.5)" : "rgba(0,0,0,.5)"; ctx.lineWidth = 1; ctx.stroke();
    if (u.hp < u.maxHp) drawHpBar(x, y - r - 4, r * 2.2, u.hp / u.maxHp);
    if (selUnits.has(id)) drawSelRing(x, y, r + 3);
  }
}

function drawHpBar(x, yTop, w, frac) {
  w = Math.max(8, w);
  ctx.fillStyle = "rgba(0,0,0,.6)"; ctx.fillRect(x - w / 2, yTop, w, 3);
  ctx.fillStyle = frac > 0.5 ? "#6bff8a" : frac > 0.25 ? "#ffd23f" : "#ff5d73";
  ctx.fillRect(x - w / 2, yTop, w * Math.max(0, frac), 3);
}
function drawSelRing(x, y, r) { ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 1.5; ctx.setLineDash([3, 3]); ctx.beginPath(); ctx.arc(x, y, r + 2, 0, 7); ctx.stroke(); ctx.setLineDash([]); }

function drawPings() {
  pings = pings.filter((p) => p.t > 0);
  for (const p of pings) {
    p.t -= 0.04;
    const x = sx(p.x), y = sy(p.y);
    ctx.strokeStyle = p.color; ctx.globalAlpha = Math.max(0, p.t); ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(x, y, (1 - p.t) * 22 + 4, 0, 7); ctx.stroke(); ctx.globalAlpha = 1;
  }
}

function drawSelectionBox() {
  const x = Math.min(dragging.x0, dragging.x1), y = Math.min(dragging.y0, dragging.y1);
  const w = Math.abs(dragging.x1 - dragging.x0), h = Math.abs(dragging.y1 - dragging.y0);
  ctx.fillStyle = "rgba(65,214,255,.12)"; ctx.fillRect(x, y, w, h);
  ctx.strokeStyle = "#41d6ff"; ctx.lineWidth = 1; ctx.strokeRect(x, y, w, h);
}

// ---- minimap ----------------------------------------------------------------
const mm = document.getElementById("minimap"), mmx = mm.getContext("2d");
function drawMinimap() {
  const S = mm.width, sc = S / WORLD;
  // coarse terrain backdrop (sampled)
  mmx.fillStyle = "#0b1420"; mmx.fillRect(0, 0, S, S);
  const N = 60;
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    const t = terrain((i + 0.5) * WORLD / N, (j + 0.5) * WORLD / N);
    mmx.fillStyle = TERRAIN_COL[t]; mmx.fillRect(i * S / N, j * S / N, S / N + 1, S / N + 1);
  }
  // owned chunks
  for (const c of chunks.values()) {
    if (!c.owner) continue;
    mmx.fillStyle = ownerColor(c.owner); mmx.globalAlpha = 0.5;
    mmx.fillRect(c.cx * CHUNK * sc, c.cy * CHUNK * sc, CHUNK * sc, CHUNK * sc);
  }
  mmx.globalAlpha = 1;
  // units as dots
  for (const u of units.values()) { mmx.fillStyle = u.owner === session ? "#fff" : ownerColor(u.owner); mmx.fillRect(u.x * sc - 1, u.y * sc - 1, 2, 2); }
  // viewport rect
  const vw = canvas.width / zoom * sc, vh = canvas.height / zoom * sc;
  mmx.strokeStyle = "#41d6ff"; mmx.lineWidth = 1;
  mmx.strokeRect((camX * sc) - vw / 2, (camY * sc) - vh / 2, vw, vh);
}
mm.addEventListener("mousedown", (e) => {
  const rect = mm.getBoundingClientRect();
  camX = (e.clientX - rect.left) / mm.width * WORLD;
  camY = (e.clientY - rect.top) / mm.height * WORLD;
});

// ---- boot -------------------------------------------------------------------
(async function boot() {
  try { cfg = await fetch("/api/config").then((r) => r.json()); } catch {}
  SEED = cfg.seed ?? SEED; WORLD = cfg.world ?? WORLD; CHUNK = cfg.chunk ?? CHUNK;
  RS = cfg.regionSize ?? RS; REGS = cfg.regions ?? REGS;
  camX = WORLD / 2; camY = WORLD / 2;
  connectWs();
  draw();

  const sw = document.getElementById("swatches");
  COLORS.forEach((c) => {
    const el = document.createElement("div");
    el.className = "sw" + (c === myColor ? " sel" : "");
    el.style.background = c; el.style.color = c;
    el.onclick = () => { myColor = c; [...sw.children].forEach((n) => n.classList.remove("sel")); el.classList.add("sel"); };
    sw.appendChild(el);
  });
  document.getElementById("name").value = myName;
  document.getElementById("playBtn").onclick = doJoin;
  document.getElementById("name").addEventListener("keydown", (e) => { if (e.key === "Enter") doJoin(); });
})();

addEventListener("beforeunload", () => { navigator.sendBeacon?.("/api/call", JSON.stringify({ reducer: "leave", args: [session] })); });
