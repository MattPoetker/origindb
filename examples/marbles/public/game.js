// Marble Clash client — LOBBY + MATCHMAKING over an OriginDB changefeed.
//
// Flow (state machine):  name → lobby browser → waiting room → game → result
//   * lobby ws  : SELECT * FROM lobbies  +  SELECT * FROM members   (matchmaking)
//   * game  ws  : SELECT * FROM marbles WHERE arena = '<lobbyId>'   (the match)
// The server spawns marbles when a match starts and runs the authoritative 60 Hz
// physics inside a WASM reducer; this client only renders + sends steer/boost.
import * as THREE from "three";

// ---- identity + prefs -------------------------------------------------------
let session = localStorage.getItem("mc_session");
if (!session) { session = "s_" + Math.random().toString(36).slice(2) + Date.now().toString(36); localStorage.setItem("mc_session", session); }
// Secret per-browser identity token. Sent as x-odb-identity; the server hashes
// it into a public owner identity (lobby.host) — so only the browser that
// created a lobby can start/dissolve it. Kept secret (never rendered), unlike
// the game `session` which appears in streamed rows.
let idToken = localStorage.getItem("mc_token");
if (!idToken) {
  idToken = "t_" + (crypto?.randomUUID ? crypto.randomUUID() : (Math.random().toString(36).slice(2) + Math.random().toString(36).slice(2)));
  localStorage.setItem("mc_token", idToken);
}
const COLORS = ["#4da3ff", "#ff6b6b", "#ffd166", "#5ee6a8", "#c792ff", "#ff9f6b", "#4ee1e1", "#f078c8"];
let myColor = localStorage.getItem("mc_color") || COLORS[0];
let myName = localStorage.getItem("mc_name") || "";

let cfg = { wsPort: 8790, arenaR: 42, shards: 8, lobbyCap: 8 };
let ARENA_R = 42, MARBLE_R = 1.1;
const HEAVY_R = 1.8;                                  // matches module HEAVY_R
const PU_COLOR = { heavy: 0xff8a3d, swift: 0x4ee1e1 }; // pickup + buff tint colors

// match/session state
let state = "name";               // name | lobby | wait | game | result
let myLobbyId = "";               // lobby I created or joined
let iAmHost = false;
let myMatch = "";                 // match id (== lobbyId) once my match is active
let myKey = "", joined = false;

// live data
const lobbies = new Map();        // id -> lobby row
const members = new Map();        // memberKey -> member row (lm:<lobbyId>:<session>)

// ---- DOM helpers ------------------------------------------------------------
const el = (id) => document.getElementById(id);
const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

const HUD = ["topbar", "board", "tips", "boostwrap"];
const OVERLAYS = { name: "screen-name", lobby: "screen-lobby", wait: "screen-wait", result: "screen-result" };
function show(next) {
  state = next;
  for (const k in OVERLAYS) el(OVERLAYS[k]).classList.toggle("hidden", OVERLAYS[k] !== OVERLAYS[next]);
  const inGame = next === "game";
  for (const h of HUD) el(h).classList.toggle("hidden", !inGame);
  if (!inGame) el("dead").style.display = "none";
}

// ---- three.js scene ---------------------------------------------------------
const app = el("app");
const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(innerWidth, innerHeight);
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
app.appendChild(renderer.domElement);

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x070b12);
scene.fog = new THREE.Fog(0x070b12, 90, 220);

const camera = new THREE.PerspectiveCamera(52, innerWidth / innerHeight, 0.1, 1000);
camera.position.set(0, 70, 70);
camera.lookAt(0, 0, 0);

scene.add(new THREE.AmbientLight(0x415066, 0.9));
const keyLight = new THREE.DirectionalLight(0xffffff, 1.5);
keyLight.position.set(30, 80, 20);
keyLight.castShadow = true;
keyLight.shadow.mapSize.set(2048, 2048);
keyLight.shadow.camera.left = -70; keyLight.shadow.camera.right = 70;
keyLight.shadow.camera.top = 70; keyLight.shadow.camera.bottom = -70;
keyLight.shadow.camera.near = 10; keyLight.shadow.camera.far = 200;
scene.add(keyLight);
const rimLight = new THREE.PointLight(0x4da3ff, 0.6, 300);
rimLight.position.set(-40, 30, -30);
scene.add(rimLight);

let arenaBuilt = false;
function buildArena(R) {
  if (arenaBuilt) return;
  arenaBuilt = true;
  ARENA_R = R;
  const floor = new THREE.Mesh(new THREE.CircleGeometry(R, 96),
    new THREE.MeshStandardMaterial({ color: 0x0f1a2e, roughness: 0.85, metalness: 0.1 }));
  floor.rotation.x = -Math.PI / 2; floor.receiveShadow = true; scene.add(floor);
  for (let i = 1; i <= 4; i++) {
    const rr = (R * i) / 4;
    const ring = new THREE.Mesh(new THREE.RingGeometry(rr - 0.08, rr + 0.08, 96),
      new THREE.MeshBasicMaterial({ color: 0x1c3252, transparent: true, opacity: 0.5 }));
    ring.rotation.x = -Math.PI / 2; ring.position.y = 0.02; scene.add(ring);
  }
  const rimMesh = new THREE.Mesh(new THREE.TorusGeometry(R, 0.55, 16, 120),
    new THREE.MeshStandardMaterial({ color: 0x4da3ff, emissive: 0x2a6bd0, emissiveIntensity: 1.4, roughness: 0.4, metalness: 0.3 }));
  rimMesh.rotation.x = -Math.PI / 2; rimMesh.position.y = 0.4; scene.add(rimMesh);
}

// ---- marble entities --------------------------------------------------------
const marbles = new Map();   // key -> render state
const geo = new THREE.SphereGeometry(MARBLE_R, 24, 18);
const colorInt = (hex) => parseInt(String(hex).replace("#", "0x")) || 0x4da3ff;

function ensureMarble(k, cols) {
  let m = marbles.get(k);
  if (!m) {
    const mine = cols.owner === session;
    const mat = new THREE.MeshStandardMaterial({
      color: colorInt(cols.color), roughness: 0.25, metalness: 0.55,
      emissive: mine ? colorInt(cols.color) : 0x000000, emissiveIntensity: mine ? 0.35 : 0,
    });
    const mesh = new THREE.Mesh(geo, mat);
    mesh.castShadow = true; scene.add(mesh);
    if (mine) {
      const halo = new THREE.Mesh(new THREE.RingGeometry(MARBLE_R * 1.4, MARBLE_R * 1.9, 40),
        new THREE.MeshBasicMaterial({ color: colorInt(cols.color), transparent: true, opacity: 0.6 }));
      halo.rotation.x = -Math.PI / 2; halo.position.y = 0.03;
      mesh.userData.halo = halo; scene.add(halo);
      myKey = k;
    }
    m = { rx: cols.x, rz: cols.y, tx: cols.x, tz: cols.y, vx: 0, vz: 0, mesh,
          color: cols.color, alive: true, score: 0, name: cols.name, owner: cols.owner, fall: 0, buff: "" };
    marbles.set(k, m);
  }
  return m;
}
function applyMarble(k, cols) {
  const m = ensureMarble(k, cols);
  m.tx = cols.x; m.tz = cols.y;
  m.vx = cols.vx || 0; m.vz = cols.vy || 0;
  m.snapT = performance.now();   // snapshot arrival time, for dead-reckoning
  m.score = cols.score || 0;
  m.name = cols.name || m.name;
  // power-up buff → grow (heavy) + tint the marble
  const buff = cols.buff || "";
  if (buff !== m.buff) {
    m.buff = buff;
    const sc = buff === "heavy" ? HEAVY_R / MARBLE_R : 1;
    m.mesh.scale.setScalar(sc);
    if (m.mesh.userData.halo) m.mesh.userData.halo.scale.setScalar(sc);
    const mine = m.owner === session;
    const tint = buff ? PU_COLOR[buff] : (mine ? colorInt(m.color) : 0x000000);
    m.mesh.material.emissive.setHex(tint || 0x000000);
    m.mesh.material.emissiveIntensity = buff ? 0.7 : (mine ? 0.35 : 0);
    if (k === myKey) showBuff(buff);
  }
  const wasAlive = m.alive;
  m.alive = cols.alive === true || cols.alive === 1;
  if (!m.alive && wasAlive) m.fall = 1;
  if (m.alive && !wasAlive) { m.fall = 0; m.rx = cols.x; m.rz = cols.y; m.mesh.position.y = MARBLE_R; }
  if (k === myKey) { el("sScore").textContent = Math.floor(m.score); lastBoostAt = cols.boostAt || 0; el("dead").style.display = m.alive ? "none" : "flex"; }
}
function removeMarble(k) {
  const m = marbles.get(k); if (!m) return;
  scene.remove(m.mesh);
  if (m.mesh.userData.halo) scene.remove(m.mesh.userData.halo);
  marbles.delete(k);
}
function clearMarbles() { for (const k of [...marbles.keys()]) removeMarble(k); myKey = ""; }

// ---- power-up pickups ----
const pickups = new Map(); // key -> { mesh, x, z }
const puGeo = new THREE.IcosahedronGeometry(0.95, 0);
function applyPickup(k, cols) {
  if (pickups.get(k)) return;
  const kind = cols.kind || "swift";
  const col = PU_COLOR[kind] || 0xffffff;
  const mesh = new THREE.Mesh(puGeo, new THREE.MeshStandardMaterial({
    color: col, emissive: col, emissiveIntensity: 0.75, roughness: 0.3, metalness: 0.4 }));
  mesh.position.set(cols.x, 1.4, cols.y); mesh.castShadow = true; scene.add(mesh);
  const ring = new THREE.Mesh(new THREE.RingGeometry(1.5, 2.0, 32),
    new THREE.MeshBasicMaterial({ color: col, transparent: true, opacity: 0.5 }));
  ring.rotation.x = -Math.PI / 2; ring.position.set(cols.x, 0.03, cols.y); scene.add(ring);
  mesh.userData.ring = ring;
  pickups.set(k, { mesh, x: cols.x, z: cols.y });
}
function removePickup(k) {
  const p = pickups.get(k); if (!p) return;
  scene.remove(p.mesh); if (p.mesh.userData.ring) scene.remove(p.mesh.userData.ring);
  pickups.delete(k);
}
function clearPickups() { for (const k of [...pickups.keys()]) removePickup(k); }

// HUD buff pill
function showBuff(buff) {
  const b = el("buff"); if (!b) return;
  if (!buff) { b.style.display = "none"; return; }
  b.style.display = "inline-flex";
  b.textContent = buff === "heavy" ? "⬣ HEAVY" : "⚡ SWIFT";
  b.style.color = buff === "heavy" ? "#ff8a3d" : "#4ee1e1";
}

function updateBoard() {
  const arr = [...marbles.values()].sort((a, b) => (b.alive - a.alive) || (b.score - a.score)).slice(0, 8);
  el("sLive").textContent = [...marbles.values()].filter((m) => m.alive).length;
  const list = el("boardList"); list.innerHTML = "";
  for (const m of arr) {
    const li = document.createElement("li");
    li.className = (m.owner === session ? "me " : "") + (m.alive ? "" : "dead");
    li.innerHTML = `<span class="dot" style="background:${m.color}"></span>` +
      `<span class="nm">${escapeHtml(m.name || "marble")}</span><span class="sc">${Math.floor(m.score)}</span>`;
    list.appendChild(li);
  }
}

// ---- reducer calls ----------------------------------------------------------
async function call(reducer, args) {
  try {
    const r = await fetch("/api/call", { method: "POST", headers: { "content-type": "application/json", "x-odb-identity": idToken }, body: JSON.stringify({ reducer, args }) });
    return await r.json();
  } catch { return { success: false }; }
}

// ---- lobby websocket (matchmaking) -----------------------------------------
let lobbySock = null;
function connectLobbyWs() {
  const q = cfg.token ? `?token=${encodeURIComponent(cfg.token)}` : "";
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${scheme}://${location.host}/${q}`);
  lobbySock = ws;
  ws.onopen = () => {
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: "SELECT * FROM lobbies" }));
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: "SELECT * FROM members" }));
  };
  ws.onclose = () => { lobbySock = null; setTimeout(connectLobbyWs, 1000); };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type === "initial_state") {
      const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1] || "";
      for (const row of msg.rows || []) ingest(table, "UPSERT", row.key, row.data);
      renderLobbyUI();
    } else if (msg.type === "sql_changefeed_event") {
      if (msg.operation === "DELETE") ingest(msg.table, "DELETE", msg.key, null);
      else { try { const p = JSON.parse(msg.new_value); ingest(msg.table, "UPSERT", msg.key, p.columns ?? p); } catch {} }
      renderLobbyUI();
    }
  };
}
function ingest(table, op, key, data) {
  const map = table === "lobbies" ? lobbies : table === "members" ? members : null;
  if (!map) return;
  if (op === "DELETE") map.delete(key); else map.set(key, data);
  if (table === "lobbies") watchMyLobby(data, key);
}

// React to MY lobby's state changes (start match / match over / lobby closed).
function watchMyLobby(row, key) {
  if (!myLobbyId || key !== myLobbyId || !row) return;
  const st = row.state;
  if (st === "active" && state !== "game") enterGame(myLobbyId);
  else if (st === "done") {
    if (state === "game") showResult(row.winnerName || "Nobody");
    else if (state === "wait") backToLobby();   // host closed / lobby dissolved
  }
}

// ---- game websocket (the match) --------------------------------------------
let gameSock = null;
function connectGameWs(match) {
  const q = cfg.token ? `?token=${encodeURIComponent(cfg.token)}` : "";
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${scheme}://${location.host}/${q}`);
  gameSock = ws;
  ws.onopen = () => {
    el("conn").textContent = "● live"; el("conn").classList.add("ok");
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM marbles WHERE arena = '${match}'` }));
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM pickups WHERE arena = '${match}'` }));
  };
  ws.onclose = () => { el("conn").textContent = "reconnecting…"; el("conn").classList.remove("ok"); if (gameSock === ws && state === "game") { gameSock = null; setTimeout(() => { if (state === "game") connectGameWs(myMatch); }, 1000); } };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type === "initial_state") {
      const table = /FROM\s+(\w+)/i.exec(msg.sql || "")?.[1];
      for (const row of msg.rows || []) (table === "pickups" ? applyPickup : applyMarble)(row.key, row.data);
    } else if (msg.type === "sql_changefeed_event") {
      if (msg.table === "pickups") {
        if (msg.operation === "DELETE") { removePickup(msg.key); return; }
        try { const p = JSON.parse(msg.new_value); applyPickup(msg.key, p.columns ?? p); } catch {}
        return;
      }
      if (msg.table !== "marbles") return;
      if (msg.operation === "DELETE") { removeMarble(msg.key); return; }
      try { const p = JSON.parse(msg.new_value); applyMarble(msg.key, p.columns ?? p); } catch {}
    }
  };
}
function closeGameWs() { if (gameSock) { const s = gameSock; gameSock = null; try { s.close(); } catch {} } }

// ---- state transitions ------------------------------------------------------
function enterGame(match) {
  myMatch = match;
  clearMarbles(); clearPickups(); showBuff("");
  show("game");
  connectGameWs(match);
}
function showResult(winnerName) {
  el("winnerName").textContent = winnerName;
  closeGameWs();
  show("result");
}
function backToLobby() {
  closeGameWs();
  clearMarbles(); clearPickups(); showBuff("");
  myLobbyId = ""; iAmHost = false; myMatch = ""; joined = false;
  show("lobby");
  renderLobbyUI();
}

// ---- lobby / waiting-room UI ------------------------------------------------
function renderLobbyUI() {
  if (state === "lobby") renderLobbyList();
  else if (state === "wait") renderWaitRoom();
}
function renderLobbyList() {
  const open = [...lobbies.values()].filter((l) => l && l.state === "waiting").sort((a, b) => (a.createdMs || 0) - (b.createdMs || 0));
  const list = el("lobbyList");
  if (!open.length) { list.innerHTML = `<div class="empty">No open lobbies — create one to get started.</div>`; return; }
  list.innerHTML = "";
  for (const l of open) {
    const full = (l.count | 0) >= (l.cap | 0);
    const div = document.createElement("div");
    div.className = "litem" + (full ? " full" : "");
    div.innerHTML = `<div><div class="lname">${escapeHtml(l.name || "Lobby")}</div>` +
      `<div class="lmeta">${l.count | 0}/${l.cap | 0} players</div></div>`;
    const btn = document.createElement("button");
    btn.className = "btn sm primary"; btn.textContent = full ? "Full" : "Join"; btn.disabled = full;
    btn.onclick = () => doJoin(l.id);
    div.appendChild(btn); list.appendChild(div);
  }
}
function renderWaitRoom() {
  const l = lobbies.get(myLobbyId);
  if (!l) return;
  el("waitName").textContent = l.name || "Lobby";
  el("waitCap").textContent = l.cap | 0;
  const mine = [...members.values()].filter((m) => m && m.lobbyId === myLobbyId);
  el("waitCount").textContent = mine.length;
  const seats = el("seats"); seats.innerHTML = "";
  const cap = l.cap | 0;
  for (let i = 0; i < cap; i++) {
    const m = mine[i];
    const seat = document.createElement("div");
    seat.className = "seat" + (m ? "" : " open");
    seat.innerHTML = m
      ? `<div class="mk" style="background:${m.color || "#4da3ff"}"></div><div class="snm">${escapeHtml(m.name || "guest")}${m.session === session ? " (you)" : ""}</div>`
      : `<div class="mk"></div><div class="snm">open</div>`;
    seats.appendChild(seat);
  }
  const need = cap - mine.length;
  el("waitStatus").innerHTML = need > 0
    ? `Waiting for <b>${need}</b> more player${need === 1 ? "" : "s"}… match starts automatically when full.`
    : `Lobby full — starting…`;
  el("btnStart").classList.toggle("hidden", !iAmHost);
  el("btnStart").disabled = mine.length < 2;
}

// ---- actions ----------------------------------------------------------------
async function doCreate() {
  const lname = el("lobbyName").value.trim().slice(0, 24);
  const cap = Math.max(2, Math.min(8, parseInt(el("lobbyCap")?.value || "8", 10) || 8));
  const res = await call("createLobby", [session, lname, myName, myColor, cap]);
  const id = res && res.result && res.result.lobbyId;
  if (!id) return;
  myLobbyId = id; iAmHost = true;
  el("lobbyName").value = "";
  show("wait"); renderWaitRoom();
}
async function doJoin(lobbyId) {
  const res = await call("joinLobby", [session, lobbyId, myName, myColor]);
  const r = res && res.result;
  if (!r || r.error) { renderLobbyList(); return; }
  myLobbyId = lobbyId; iAmHost = false;
  if (r.state === "active") enterGame(lobbyId);
  else { show("wait"); renderWaitRoom(); }
}
async function doStart() { await call("startNow", [session, myLobbyId]); }
async function doLeave() {
  const id = myLobbyId;
  backToLobby();
  if (id) await call("leaveLobby", [session, id]);
}

// ---- input: arrow keys (or WASD) — direct direction, no mouse ---------------
const keys = { up: false, down: false, left: false, right: false };
const KEYMAP = {
  arrowup: "up", arrowdown: "down", arrowleft: "left", arrowright: "right",
  w: "up", s: "down", a: "left", d: "right",
};
let steerX = 0, steerY = 0, lastBoostAt = 0;
const BOOST_COOLDOWN = 1500;

addEventListener("keydown", (e) => {
  const dir = KEYMAP[e.key.toLowerCase()];
  if (state === "game" && dir) { keys[dir] = true; e.preventDefault(); }
  if (e.code === "Space" && state === "game") { e.preventDefault(); doBoost(); }
});
addEventListener("keyup", (e) => { const dir = KEYMAP[e.key.toLowerCase()]; if (dir) keys[dir] = false; });

function doBoost() {
  if (Date.now() - lastBoostAt < BOOST_COOLDOWN) return;
  lastBoostAt = Date.now();
  call("boost", [session]);
}
function computeSteer() {
  // screen: up = world −z (forward), right = world +x. Direct, no runaway.
  const kx = (keys.right ? 1 : 0) - (keys.left ? 1 : 0);
  const ky = (keys.down ? 1 : 0) - (keys.up ? 1 : 0);
  if (kx !== 0 || ky !== 0) { const l = Math.hypot(kx, ky); return [kx / l, ky / l]; }
  return [0, 0];
}
let lastSteerSent = 0;
function maybeSendSteer(me) {
  const [sx, sy] = computeSteer(me);
  const now = performance.now();
  const changed = Math.abs(sx - steerX) > 0.08 || Math.abs(sy - steerY) > 0.08;
  if (now - lastSteerSent > 50 && (changed || (sx === 0) !== (steerX === 0))) {
    steerX = sx; steerY = sy; lastSteerSent = now;
    call("steer", [session, sx, sy]);
  }
}

// ---- render loop ------------------------------------------------------------
const camTarget = new THREE.Vector3(0, 0, 0);
const camPos = new THREE.Vector3(0, 70, 70);
let last = performance.now();
function animate() {
  requestAnimationFrame(animate);
  const now = performance.now();
  const dt = Math.min(0.05, (now - last) / 1000); last = now;

  const me = myKey ? marbles.get(myKey) : null;
  if (state === "game") maybeSendSteer(me);

  for (const [k, m] of marbles) {
    const ry = m.buff === "heavy" ? HEAVY_R : MARBLE_R; // heavy marble sits higher
    if (m.alive) {
      // dead-reckoning: extrapolate the last snapshot forward by its velocity
      // (capped) so motion stays smooth between 30 Hz updates, then ease toward it.
      const age = Math.min(0.14, (now - (m.snapT || now)) / 1000);
      const px = m.tx + m.vx * age, pz = m.tz + m.vz * age;
      m.rx += (px - m.rx) * Math.min(1, dt * 24);
      m.rz += (pz - m.rz) * Math.min(1, dt * 24);
      m.mesh.position.set(m.rx, ry, m.rz);
      const sp = Math.hypot(m.vx, m.vz);
      if (sp > 0.1) { m.mesh.rotation.x += (m.vz / MARBLE_R) * dt; m.mesh.rotation.z -= (m.vx / MARBLE_R) * dt; }
      m.mesh.visible = true;
    } else if (m.fall > 0) {
      m.fall = Math.max(0, m.fall - dt * 1.2);
      m.mesh.position.set(m.rx, ry - (1 - m.fall) * 40, m.rz);
      m.mesh.visible = m.fall > 0.02;
    } else { m.mesh.visible = false; }
    if (m.mesh.userData.halo) { m.mesh.userData.halo.position.set(m.rx, 0.03, m.rz); m.mesh.userData.halo.visible = m.mesh.visible; }
  }

  // spin + bob the floating pickups
  for (const [, p] of pickups) {
    p.mesh.rotation.y += dt * 1.8; p.mesh.rotation.x += dt * 0.9;
    p.mesh.position.y = 1.4 + Math.sin(now * 0.003 + p.x) * 0.28;
  }

  if (state === "game" && me && me.alive) { camTarget.set(me.rx, 0, me.rz); camPos.set(me.rx, 46, me.rz + 40); }
  else { const t = now * 0.00008; camTarget.set(0, 0, 0); camPos.set(Math.sin(t) * 60, 72, Math.cos(t) * 60); }
  camera.position.lerp(camPos, Math.min(1, dt * 3));
  camera.lookAt(camTarget);

  if (state === "game") {
    const cd = Math.min(1, (Date.now() - lastBoostAt) / BOOST_COOLDOWN);
    el("boostfill").style.width = (cd * 100) + "%";
    el("boostfill").style.background = cd >= 1 ? "#5ee6a8" : "#4da3ff";
    updateBoard();
  }
  renderer.render(scene, camera);
}
addEventListener("resize", () => {
  camera.aspect = innerWidth / innerHeight; camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});

// ---- name/color screen ------------------------------------------------------
function buildColorPicker() {
  const wrap = el("colors");
  for (const c of COLORS) {
    const b = document.createElement("button");
    b.style.background = c;
    if (c === myColor) b.classList.add("sel");
    b.onclick = () => { myColor = c; localStorage.setItem("mc_color", c); [...wrap.children].forEach((x) => x.classList.remove("sel")); b.classList.add("sel"); };
    wrap.appendChild(b);
  }
}
function submitName() {
  myName = el("name").value.trim().slice(0, 16) || myName || ("marble-" + session.slice(-4));
  localStorage.setItem("mc_name", myName);
  show("lobby"); renderLobbyList();
}

// ---- boot -------------------------------------------------------------------
async function boot() {
  try { cfg = await (await fetch("/api/config")).json(); } catch {}
  ARENA_R = cfg.arenaR || 42;
  el("capLbl").textContent = cfg.lobbyCap || 8;
  buildArena(ARENA_R);
  buildColorPicker();
  el("name").value = myName;
  el("btnName").onclick = submitName;
  el("name").addEventListener("keydown", (e) => { if (e.key === "Enter") submitName(); });
  el("btnCreate").onclick = doCreate;
  el("lobbyName").addEventListener("keydown", (e) => { if (e.key === "Enter") doCreate(); });
  el("btnStart").onclick = doStart;
  el("btnLeave").onclick = doLeave;
  el("btnQuit").onclick = doLeave;
  el("btnAgain").onclick = backToLobby;
  connectLobbyWs();
  animate();
}
addEventListener("beforeunload", () => {
  if (myLobbyId) navigator.sendBeacon?.("/api/call", JSON.stringify({ reducer: "leaveLobby", args: [session, myLobbyId] }));
});
boot();
