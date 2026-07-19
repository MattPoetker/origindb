// Marble Clash client — three.js renderer over an OriginDB changefeed.
//
// The server sims physics at 60 Hz inside a WASM reducer and streams every
// marble's state over the changefeed. This client:
//   * subscribes to `SELECT * FROM marbles WHERE arena = <n>` (AOI = one arena),
//   * interpolates positions between server snapshots for smooth motion,
//   * turns mouse / WASD into a steer vector and posts it at ~20 Hz,
//   * renders a glossy 3D arena with three.js.
import * as THREE from "three";

// ---- identity + prefs -------------------------------------------------------
let session = localStorage.getItem("mc_session");
if (!session) { session = "s_" + Math.random().toString(36).slice(2) + Date.now().toString(36); localStorage.setItem("mc_session", session); }
const COLORS = ["#4da3ff", "#ff6b6b", "#ffd166", "#5ee6a8", "#c792ff", "#ff9f6b", "#4ee1e1", "#f078c8"];
let myColor = localStorage.getItem("mc_color") || COLORS[0];
let myName = localStorage.getItem("mc_name") || "";

let cfg = { wsPort: 8790, arenaR: 42, arenas: 8, cap: 60 };
let ARENA_R = 42, MARBLE_R = 1.1;
let myArena = 0, myKey = "", joined = false, sock = null, subId = null;

// ---- three.js scene ---------------------------------------------------------
const app = document.getElementById("app");
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

// lights
scene.add(new THREE.AmbientLight(0x415066, 0.9));
const key = new THREE.DirectionalLight(0xffffff, 1.5);
key.position.set(30, 80, 20);
key.castShadow = true;
key.shadow.mapSize.set(2048, 2048);
key.shadow.camera.left = -70; key.shadow.camera.right = 70;
key.shadow.camera.top = 70; key.shadow.camera.bottom = -70;
key.shadow.camera.near = 10; key.shadow.camera.far = 200;
scene.add(key);
const rim = new THREE.PointLight(0x4da3ff, 0.6, 300);
rim.position.set(-40, 30, -30);
scene.add(rim);

function buildArena(R) {
  ARENA_R = R;
  // floor disc
  const floor = new THREE.Mesh(
    new THREE.CircleGeometry(R, 96),
    new THREE.MeshStandardMaterial({ color: 0x0f1a2e, roughness: 0.85, metalness: 0.1 })
  );
  floor.rotation.x = -Math.PI / 2;
  floor.receiveShadow = true;
  scene.add(floor);
  // concentric guide rings
  for (let i = 1; i <= 4; i++) {
    const rr = (R * i) / 4;
    const ring = new THREE.Mesh(
      new THREE.RingGeometry(rr - 0.08, rr + 0.08, 96),
      new THREE.MeshBasicMaterial({ color: 0x1c3252, transparent: true, opacity: 0.5 })
    );
    ring.rotation.x = -Math.PI / 2; ring.position.y = 0.02;
    scene.add(ring);
  }
  // glowing rim torus
  const rimMesh = new THREE.Mesh(
    new THREE.TorusGeometry(R, 0.55, 16, 120),
    new THREE.MeshStandardMaterial({ color: 0x4da3ff, emissive: 0x2a6bd0, emissiveIntensity: 1.4, roughness: 0.4, metalness: 0.3 })
  );
  rimMesh.rotation.x = -Math.PI / 2; rimMesh.position.y = 0.4;
  scene.add(rimMesh);
}

// ---- marble entities --------------------------------------------------------
const marbles = new Map();   // key -> {rx,rz, tx,tz, vx,vz, mesh, color, alive, score, name, owner, fall}
const geo = new THREE.SphereGeometry(MARBLE_R, 24, 18);

function colorInt(hex) { return parseInt(hex.replace("#", "0x")); }

function ensureMarble(k, cols) {
  let m = marbles.get(k);
  if (!m) {
    const mine = cols.owner === session;
    const mat = new THREE.MeshStandardMaterial({
      color: colorInt(cols.color || "#4da3ff"),
      roughness: 0.25, metalness: 0.55,
      emissive: mine ? colorInt(cols.color || "#4da3ff") : 0x000000,
      emissiveIntensity: mine ? 0.35 : 0,
    });
    const mesh = new THREE.Mesh(geo, mat);
    mesh.castShadow = true;
    scene.add(mesh);
    // own marble gets a ground halo
    if (mine) {
      const halo = new THREE.Mesh(
        new THREE.RingGeometry(MARBLE_R * 1.4, MARBLE_R * 1.9, 40),
        new THREE.MeshBasicMaterial({ color: colorInt(cols.color || "#4da3ff"), transparent: true, opacity: 0.6 })
      );
      halo.rotation.x = -Math.PI / 2; halo.position.y = 0.03;
      mesh.userData.halo = halo; scene.add(halo);
      myKey = k;
    }
    m = { rx: cols.x, rz: cols.y, tx: cols.x, tz: cols.y, vx: 0, vz: 0, mesh,
          color: cols.color, alive: true, score: 0, name: cols.name, owner: cols.owner, fall: 0 };
    marbles.set(k, m);
  }
  return m;
}

function applyMarble(k, cols) {
  const m = ensureMarble(k, cols);
  m.tx = cols.x; m.tz = cols.y;
  m.vx = cols.vx || 0; m.vz = cols.vy || 0;
  m.score = cols.score || 0;
  m.name = cols.name || m.name;
  const wasAlive = m.alive;
  m.alive = cols.alive === true || cols.alive === 1;
  if (!m.alive && wasAlive) m.fall = 1;                 // begin drop animation
  if (m.alive && !wasAlive) { m.fall = 0; m.rx = cols.x; m.rz = cols.y; m.mesh.position.y = MARBLE_R; }
  if (k === myKey) updateSelf(m, cols);
}

function removeMarble(k) {
  const m = marbles.get(k);
  if (!m) return;
  scene.remove(m.mesh);
  if (m.mesh.userData.halo) scene.remove(m.mesh.userData.halo);
  marbles.delete(k);
}

// ---- HUD --------------------------------------------------------------------
const el = (id) => document.getElementById(id);
let lastBoostAt = 0;
const BOOST_COOLDOWN = 1500;

function updateSelf(m, cols) {
  el("sScore").textContent = Math.floor(m.score);
  lastBoostAt = cols.boostAt || 0;
  const dead = !m.alive;
  el("dead").style.display = dead ? "flex" : "none";
}

function updateBoard() {
  const arr = [...marbles.values()].sort((a, b) => b.score - a.score).slice(0, 6);
  const live = [...marbles.values()].filter((m) => m.alive).length;
  el("sLive").textContent = live;
  const list = el("boardList");
  list.innerHTML = "";
  for (const m of arr) {
    const li = document.createElement("li");
    if (m.owner === session) li.className = "me";
    li.innerHTML = `<span class="dot" style="background:${m.color}"></span>` +
      `<span class="nm">${escapeHtml(m.name || "marble")}</span><span class="sc">${Math.floor(m.score)}</span>`;
    list.appendChild(li);
  }
}
function escapeHtml(s) { return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c])); }

// ---- networking -------------------------------------------------------------
async function call(reducer, args) {
  try {
    const r = await fetch("/api/call", { method: "POST", headers: { "content-type": "application/json" },
      body: JSON.stringify({ reducer, args }) });
    return await r.json();
  } catch { return { success: false }; }
}

function connectWs() {
  const q = cfg.token ? `?token=${encodeURIComponent(cfg.token)}` : "";
  const ws = new WebSocket(`ws://${location.hostname}:${cfg.wsPort}${q}`);
  sock = ws;
  const conn = el("conn");
  ws.onopen = () => {
    conn.textContent = "● live"; conn.classList.add("ok");
    ws.send(JSON.stringify({ type: "sql_subscribe", sql: `SELECT * FROM marbles WHERE arena = ${myArena}` }));
  };
  ws.onclose = () => { conn.textContent = "reconnecting…"; conn.classList.remove("ok"); sock = null; setTimeout(connectWs, 1000); };
  ws.onmessage = (e) => {
    let msg; try { msg = JSON.parse(e.data); } catch { return; }
    if (msg.type === "initial_state") {
      for (const row of msg.rows || []) applyMarble(row.key, row.data);
    } else if (msg.type === "sql_subscription_created") {
      subId = msg.subscription_id;
    } else if (msg.type === "sql_changefeed_event") {
      if (msg.table !== "marbles") return;
      if (msg.operation === "DELETE") { removeMarble(msg.key); return; }
      try { const p = JSON.parse(msg.new_value); applyMarble(msg.key, p.columns ?? p); } catch {}
    }
  };
}

// ---- input ------------------------------------------------------------------
const keys = { w: false, a: false, s: false, d: false };
let pointer = new THREE.Vector2(0, 0), havePointer = false;
const raycaster = new THREE.Raycaster();
const groundPlane = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);
let steerX = 0, steerY = 0;

addEventListener("mousemove", (e) => { pointer.set((e.clientX / innerWidth) * 2 - 1, -(e.clientY / innerHeight) * 2 + 1); havePointer = true; });
addEventListener("keydown", (e) => {
  const k = e.key.toLowerCase();
  if (k in keys) keys[k] = true;
  if (e.code === "Space" && joined) { e.preventDefault(); doBoost(); }
});
addEventListener("keyup", (e) => { const k = e.key.toLowerCase(); if (k in keys) keys[k] = false; });

function doBoost() {
  if (Date.now() - lastBoostAt < BOOST_COOLDOWN) return;
  lastBoostAt = Date.now();
  call("boost", [session]);
}

// compute steer vector in sim space from WASD (priority) or pointer→ground ray
function computeSteer(me) {
  let kx = (keys.d ? 1 : 0) - (keys.a ? 1 : 0);
  let ky = (keys.s ? 1 : 0) - (keys.w ? 1 : 0);
  if (kx !== 0 || ky !== 0) { const l = Math.hypot(kx, ky); return [kx / l, ky / l]; }
  if (havePointer && me) {
    raycaster.setFromCamera(pointer, camera);
    const hit = new THREE.Vector3();
    if (raycaster.ray.intersectPlane(groundPlane, hit)) {
      const dx = hit.x - me.rx, dy = hit.z - me.rz;
      const l = Math.hypot(dx, dy);
      if (l > 0.6) return [dx / l, dy / l];
    }
  }
  return [0, 0];
}

// send steer at ~20 Hz, only when it meaningfully changes
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
  if (joined) maybeSendSteer(me);

  // interpolate + place meshes
  for (const [k, m] of marbles) {
    if (m.alive) {
      // smooth toward latest server position (snapshots arrive up to 60 Hz)
      m.rx += (m.tx - m.rx) * Math.min(1, dt * 18);
      m.rz += (m.tz - m.rz) * Math.min(1, dt * 18);
      m.mesh.position.set(m.rx, MARBLE_R, m.rz);
      // rolling: rotate about axis perpendicular to velocity
      const sp = Math.hypot(m.vx, m.vz);
      if (sp > 0.1) { m.mesh.rotation.x += (m.vz / MARBLE_R) * dt; m.mesh.rotation.z -= (m.vx / MARBLE_R) * dt; }
      m.mesh.visible = true;
    } else if (m.fall > 0) {
      // drop through the void
      m.fall = Math.max(0, m.fall - dt * 1.2);
      m.mesh.position.set(m.rx, MARBLE_R - (1 - m.fall) * 40, m.rz);
      m.mesh.visible = m.fall > 0.02;
    } else {
      m.mesh.visible = false;
    }
    if (m.mesh.userData.halo) { m.mesh.userData.halo.position.set(m.rx, 0.03, m.rz); m.mesh.userData.halo.visible = m.mesh.visible; }
  }

  // camera follows own marble (or gently orbits center as spectator)
  if (me && me.alive) {
    camTarget.set(me.rx, 0, me.rz);
    camPos.set(me.rx, 46, me.rz + 40);
  } else {
    const t = now * 0.00008;
    camTarget.set(0, 0, 0);
    camPos.set(Math.sin(t) * 60, 72, Math.cos(t) * 60);
  }
  camera.position.lerp(camPos, Math.min(1, dt * 3));
  camera.lookAt(camTarget);

  // boost cooldown bar
  const cd = Math.min(1, (Date.now() - lastBoostAt) / BOOST_COOLDOWN);
  el("boostfill").style.width = (cd * 100) + "%";
  el("boostfill").style.background = cd >= 1 ? "#5ee6a8" : "#4da3ff";

  updateBoard();
  renderer.render(scene, camera);
}

addEventListener("resize", () => {
  camera.aspect = innerWidth / innerHeight; camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});

// ---- join flow --------------------------------------------------------------
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

async function join() {
  myName = el("name").value.trim().slice(0, 16) || myName;
  if (myName) localStorage.setItem("mc_name", myName);
  const res = await call("spawn", [session, myName, myColor]);
  if (!res || !res.success) { el("play").textContent = "server unavailable — retry"; return; }
  myArena = res.result?.arena ?? 0;
  el("sArena").textContent = myArena;
  joined = true;
  el("join").classList.add("hidden");
  connectWs();
}

async function boot() {
  try { cfg = await (await fetch("/api/config")).json(); } catch {}
  ARENA_R = cfg.arenaR || 42;
  buildArena(ARENA_R);
  buildColorPicker();
  el("name").value = myName;
  el("play").onclick = join;
  el("name").addEventListener("keydown", (e) => { if (e.key === "Enter") join(); });
  animate();
}

addEventListener("beforeunload", () => { navigator.sendBeacon?.("/api/call", JSON.stringify({ reducer: "leave", args: [session] })); });
boot();
