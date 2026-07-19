// Marble Clash — a 60 Hz physics sumo whose entire authoritative simulation
// runs as an OriginDB WASM reducer. Marbles roll on a circular arena; players
// steer with a thrust vector, collide elastically, and try to knock rivals off
// the rim. Last-toucher scores on a kill. Everything — positions, velocities,
// scores — lives in OriginDB tables, so the match persists through the WAL and
// streams to every client over the changefeed.
//
// Why this is a hard demo: unlike an RTS where only *active* chunks move, in a
// physics sim EVERY body moves EVERY tick, so every row is dirty every frame.
// That is the worst case for a write-ahead-log DB. The scaling trick is to
// SHARD BY ARENA: this same module is deployed once per arena (marble_<n>), and
// each instance ticks on its own mutex → its own core. Tables in OriginDB are
// server-global, so rows are ARENA-PREFIXED (m:<arena>:<id>) — every arena
// only scans/writes its own slice, and clients subscribe WHERE arena = <n>.
// Within an arena, a SPATIAL HASH keeps broad-phase collision at ~O(N).
//
// Tables (server-global, arena-namespaced by key + `arena` column):
//   marbles  key = m:<arena>:<id>   pos, vel, thrust, owner, score, arena
//   players  key = <session>        name/color/score/marble-key/arena/alive
//
// Reducers:
//   spawn(session, name, color, arena)   create this player's marble, return id
//   steer(session, dirX, dirY)           set thrust direction (unit vector)
//   boost(session)                       dash impulse (cooldown)
//   leave(session)                       remove marble
//   tick(dtMs, subSteps, arena)          integrate physics + collisions + score

import {
  JsonValue,
  declareTable,
  deleteTable,
  generateId,
  nowMs,
  readTable,
  registerReducer,
  scanTable,
  setModuleInfo,
  writeTable,
} from "../../assembly/index";

export {
  origindb_alloc,
  origindb_free,
  origindb_describe,
  origindb_invoke,
  __origindb_abort,
} from "../../assembly/index";

setModuleInfo("marbles", "1.0.0");
declareTable("marbles");
declareTable("players");

// ---- arena + physics constants (keep in sync with public/game.js) ----------

const ARENA_R: f64 = 42.0;        // arena radius (world units); rim at this dist
const MARBLE_R: f64 = 1.1;        // marble radius
const THRUST: f64 = 46.0;         // steer acceleration (u/s^2)
const DAMPING: f64 = 0.86;        // linear velocity retained per second
const RESTITUTION: f64 = 0.94;    // marble-marble bounce elasticity
const MAX_SPEED: f64 = 34.0;      // clamp (u/s) — keeps the sim well-behaved
const BOOST_IMPULSE: f64 = 26.0;  // dash delta-v (u/s)
const BOOST_COOLDOWN: f64 = 1500.0; // ms between boosts
const FALL_MARGIN: f64 = 1.5;     // center-dist past rim before a marble drops
const RESPAWN_MS: f64 = 1800.0;   // dead → respawn delay
const CELL: f64 = 2.4;            // spatial-hash cell size (~2*MARBLE_R)
const FIXED_DT: f64 = 1.0 / 60.0; // physics substep (true 60 Hz)
const SCAN_LIMIT: i32 = 100000;   // scan all rows (SDK default caps at 1000)

// ---- deterministic PRNG (seeded per call from host clock) -------------------

let g_seed: u64 = 0;
function rnd(): f64 {
  if (g_seed == 0) g_seed = (<u64>nowMs()) ^ 0x9e3779b97f4a7c15 ^ (<u64>generateId());
  let x = g_seed;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_seed = x;
  return <f64>(x >> 11) / 9007199254740992.0;
}

// A random point inside the inner disc (safe spawn away from the rim).
function spawnPoint(out: StaticArray<f64>): void {
  const a = rnd() * 6.2831853;
  const r = Math.sqrt(rnd()) * (ARENA_R * 0.6);
  out[0] = Math.cos(a) * r;
  out[1] = Math.sin(a) * r;
}

function marbleKey(arena: i32, id: string): string {
  return "m:" + arena.toString() + ":" + id;
}
function marblePrefix(arena: i32): string {
  return "m:" + arena.toString() + ":";
}

// ---- marble struct ----------------------------------------------------------

class M {
  id: string = "";
  arena: i32 = 0;
  owner: string = "";        // session id
  name: string = "";
  color: string = "#4da3ff";
  x: f64 = 0;
  y: f64 = 0;
  vx: f64 = 0;
  vy: f64 = 0;
  tx: f64 = 0;               // thrust direction (unit)
  ty: f64 = 0;
  score: f64 = 0;
  alive: bool = true;
  respawnAt: f64 = 0;        // ms; when dead, time to respawn
  lastHitBy: string = "";    // session of last marble to touch (kill credit)
  boostAt: f64 = 0;          // ms of last boost (cooldown)
  dirty: bool = false;

  static from(v: JsonValue): M {
    const m = new M();
    m.id = v.getString("id", "");
    m.arena = <i32>v.getNumber("arena", 0);
    m.owner = v.getString("owner", "");
    m.name = v.getString("name", "");
    m.color = v.getString("color", "#4da3ff");
    m.x = v.getNumber("x", 0);
    m.y = v.getNumber("y", 0);
    m.vx = v.getNumber("vx", 0);
    m.vy = v.getNumber("vy", 0);
    m.tx = v.getNumber("tx", 0);
    m.ty = v.getNumber("ty", 0);
    m.score = v.getNumber("score", 0);
    m.alive = v.getBool("alive", true);
    m.respawnAt = v.getNumber("respawnAt", 0);
    m.lastHitBy = v.getString("lastHitBy", "");
    m.boostAt = v.getNumber("boostAt", 0);
    return m;
  }

  key(): string { return marbleKey(this.arena, this.id); }

  // Hand-rolled JSON — this runs 60x/sec per live marble in the tick writeback,
  // so we avoid building a JsonValue object graph (which allocated ~15 objects
  // per row and dominated tick time under load). Plain string concat only.
  toJson(): string {
    return "{\"id\":\"" + this.id +
      "\",\"arena\":" + this.arena.toString() +
      ",\"owner\":\"" + esc(this.owner) +
      "\",\"name\":\"" + esc(this.name) +
      "\",\"color\":\"" + esc(this.color) +
      "\",\"x\":" + this.x.toString() +
      ",\"y\":" + this.y.toString() +
      ",\"vx\":" + this.vx.toString() +
      ",\"vy\":" + this.vy.toString() +
      ",\"tx\":" + this.tx.toString() +
      ",\"ty\":" + this.ty.toString() +
      ",\"score\":" + this.score.toString() +
      ",\"alive\":" + (this.alive ? "true" : "false") +
      ",\"respawnAt\":" + this.respawnAt.toString() +
      ",\"lastHitBy\":\"" + esc(this.lastHitBy) +
      "\",\"boostAt\":" + this.boostAt.toString() + "}";
  }
}

// Escape the two JSON-significant chars in a string field (names are short and
// user-supplied; owners/colors are controlled). Keeps writeback JSON valid.
function esc(s: string): string {
  let out = "";
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c == 0x22 || c == 0x5c) out += "\\";
    if (c >= 0x20) out += String.fromCharCode(c);
  }
  return out;
}

// ---- helpers ----------------------------------------------------------------

// The player row stores the marble's FULL key → O(1) read, no scan.
function findMarbleBySession(session: string): M | null {
  const p = readTable("players", session);
  if (p == null) return null;
  const pj = JsonValue.parse(p!);
  const mkey = pj.getString("mkey", "");
  if (mkey.length == 0) return null;
  const v = readTable("marbles", mkey);
  if (v == null) return null;
  return M.from(JsonValue.parse(v!));
}

function writePlayer(session: string, name: string, color: string, mkey: string, arena: i32, score: f64): void {
  writeTable("players", session, JsonValue.newObject()
    .setString("session", session)
    .setString("name", name)
    .setString("color", color)
    .setString("mkey", mkey)
    .setNumber("arena", <f64>arena)
    .setNumber("score", score)
    .setNumber("updated", <f64>nowMs())
    .toString());
}

// ---- reducers ---------------------------------------------------------------

// spawn: create this session's marble in the given arena. Idempotent —
// rejoining with the same session returns the existing marble.
function spawn(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const name = args.length > 1 ? args[1].asString() : "";
  let color = args.length > 2 ? args[2].asString() : "";
  const arena = <i32>(args.length > 3 ? args[3].asNumber() : 0);
  if (session.length == 0) return null;
  if (color.length == 0) color = "#4da3ff";

  const existing = findMarbleBySession(session);
  if (existing != null) {
    return JsonValue.newObject().setString("id", existing!.id).setNumber("arena", <f64>existing!.arena).setBool("existing", true);
  }

  const id = generateId().toString();
  const m = new M();
  m.id = id;
  m.arena = arena;
  m.owner = session;
  m.name = name.length > 0 ? name : ("marble-" + id.substring(id.length - 4));
  m.color = color;
  const p = new StaticArray<f64>(2);
  spawnPoint(p);
  m.x = p[0]; m.y = p[1];
  writeTable("marbles", m.key(), m.toJson());
  writePlayer(session, m.name, color, m.key(), arena, 0);
  return JsonValue.newObject().setString("id", id).setNumber("arena", <f64>arena).setBool("existing", false);
}

// steer: set the thrust direction (client sends a unit vector toward the mouse
// / movement keys). Cheap — updates two columns.
function steer(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  let dx = args.length > 1 ? args[1].asNumber() : 0;
  let dy = args.length > 2 ? args[2].asNumber() : 0;
  const m = findMarbleBySession(session);
  if (m == null || !m!.alive) return null;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len > 0.0001) { dx /= len; dy /= len; } else { dx = 0; dy = 0; }
  m!.tx = dx; m!.ty = dy;
  writeTable("marbles", m!.key(), m!.toJson());
  return null;
}

// boost: a dash impulse in the current thrust direction, on a cooldown.
function boost(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const m = findMarbleBySession(session);
  if (m == null || !m!.alive) return null;
  const now = <f64>nowMs();
  if (now - m!.boostAt < BOOST_COOLDOWN) return null;
  const dx = m!.tx, dy = m!.ty;
  if (dx * dx + dy * dy < 0.0001) return null;   // no direction → no dash
  m!.vx += dx * BOOST_IMPULSE;
  m!.vy += dy * BOOST_IMPULSE;
  m!.boostAt = now;
  writeTable("marbles", m!.key(), m!.toJson());
  return JsonValue.newObject().setBool("ok", true);
}

// leave: remove the marble and player row.
function leave(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const m = findMarbleBySession(session);
  if (m != null) deleteTable("marbles", m!.key());
  deleteTable("players", session);
  return null;
}

// tick: the whole physics sim for ONE arena. Loads every marble in this arena,
// runs `subSteps` fixed-dt integration + collision passes, then writes back
// every marble once. dtMs is advisory; physics uses FIXED_DT for stability.
function tick(args: Array<JsonValue>): JsonValue | null {
  let subSteps = <i32>(args.length > 1 ? args[1].asNumber() : 1);
  if (subSteps < 1) subSteps = 1;
  if (subSteps > 8) subSteps = 8;
  const arena = <i32>(args.length > 2 ? args[2].asNumber() : 0);
  const now = <f64>nowMs();

  const rows = JsonValue.parse(scanTable("marbles", marblePrefix(arena), SCAN_LIMIT));
  const n = rows.length;
  if (n == 0) return null;

  const ms = new Array<M>(n);
  for (let i = 0; i < n; i++) {
    ms[i] = M.from(rows.at(i).get("value"));
  }

  // respawn the dead whose timer elapsed
  for (let i = 0; i < n; i++) {
    const m = ms[i];
    if (!m.alive && now >= m.respawnAt) {
      const p = new StaticArray<f64>(2);
      spawnPoint(p);
      m.x = p[0]; m.y = p[1]; m.vx = 0; m.vy = 0; m.tx = 0; m.ty = 0;
      m.alive = true; m.lastHitBy = "";
      m.dirty = true;
    }
  }

  const rim = ARENA_R + FALL_MARGIN;
  const rim2 = rim * rim;
  const diam = MARBLE_R * 2.0;
  const diam2 = diam * diam;
  const damp = Math.pow(DAMPING, FIXED_DT);

  for (let step = 0; step < subSteps; step++) {
    // --- integrate: thrust, damping, clamp, move ---
    for (let i = 0; i < n; i++) {
      const m = ms[i];
      if (!m.alive) continue;
      m.vx += m.tx * THRUST * FIXED_DT;
      m.vy += m.ty * THRUST * FIXED_DT;
      m.vx *= damp; m.vy *= damp;
      const sp = Math.sqrt(m.vx * m.vx + m.vy * m.vy);
      if (sp > MAX_SPEED) { const s = MAX_SPEED / sp; m.vx *= s; m.vy *= s; }
      m.x += m.vx * FIXED_DT;
      m.y += m.vy * FIXED_DT;
      m.dirty = true;
    }

    // --- broad-phase: spatial hash of live marbles ---
    const grid = new Map<i32, Array<i32>>();
    for (let i = 0; i < n; i++) {
      if (!ms[i].alive) continue;
      const ck = cellKey(ms[i].x, ms[i].y);
      if (!grid.has(ck)) grid.set(ck, new Array<i32>());
      grid.get(ck).push(i);
    }

    // --- narrow-phase: resolve pairs in same + neighbor cells (each pair once) ---
    const keys = grid.keys();
    for (let gk = 0; gk < keys.length; gk++) {
      const cellIdx = keys[gk];
      const cxx = cellIdx >> 16;
      const cyy = (cellIdx << 16) >> 16;   // sign-extend low 16 bits
      const a = grid.get(cellIdx);
      for (let ox = -1; ox <= 1; ox++) {
        for (let oy = -1; oy <= 1; oy++) {
          const nk = packCell(cxx + ox, cyy + oy);
          if (nk < cellIdx || !grid.has(nk)) continue;   // handle each cell-pair once
          const b = grid.get(nk);
          for (let ia = 0; ia < a.length; ia++) {
            for (let ib = 0; ib < b.length; ib++) {
              const i = a[ia], j = b[ib];
              if (nk == cellIdx && j <= i) continue;
              collide(ms[i], ms[j], diam, diam2);
            }
          }
        }
      }
    }

    // --- rim: knock-outs + kill credit ---
    for (let i = 0; i < n; i++) {
      const m = ms[i];
      if (!m.alive) continue;
      if (m.x * m.x + m.y * m.y > rim2) {
        m.alive = false;
        m.respawnAt = now + RESPAWN_MS;
        m.dirty = true;
        if (m.lastHitBy.length > 0 && m.lastHitBy != m.owner) {
          for (let j = 0; j < n; j++) {
            if (ms[j].owner == m.lastHitBy) { ms[j].score += 1; ms[j].dirty = true; break; }
          }
        }
      }
    }
  }

  // --- write back every changed marble (one staged upsert each) ---
  for (let i = 0; i < n; i++) {
    if (ms[i].dirty) writeTable("marbles", ms[i].key(), ms[i].toJson());
  }

  // The long-lived instance has no automatic GC cadence, so its garbage (row
  // parse trees, spatial-hash Map/Arrays) must be reclaimed or memory.grow will
  // eventually fail (wasm unreachable). But a full __collect() every tick is
  // itself expensive at 60 Hz, so we amortize: collect ~5x/sec. That bounds
  // memory to a fraction of a second of garbage while keeping per-tick cost low.
  g_tick_count++;
  if (g_tick_count >= 12) { g_tick_count = 0; __collect(); }
  return null;
}

let g_tick_count: i32 = 0;

// elastic collision between two equal-mass marbles: positional de-penetration
// + velocity impulse along the contact normal. Records mutual last-hit.
function collide(a: M, b: M, diam: f64, diam2: f64): void {
  if (!a.alive || !b.alive) return;
  const dx = b.x - a.x;
  const dy = b.y - a.y;
  const d2 = dx * dx + dy * dy;
  if (d2 >= diam2 || d2 < 1e-9) return;
  const d = Math.sqrt(d2);
  const nx = dx / d, ny = dy / d;
  const overlap = diam - d;
  const push = overlap * 0.5;
  a.x -= nx * push; a.y -= ny * push;
  b.x += nx * push; b.y += ny * push;
  const rvx = b.vx - a.vx, rvy = b.vy - a.vy;
  const vn = rvx * nx + rvy * ny;
  if (vn < 0) {
    const jImp = -(1.0 + RESTITUTION) * vn * 0.5;   // equal mass
    a.vx -= jImp * nx; a.vy -= jImp * ny;
    b.vx += jImp * nx; b.vy += jImp * ny;
  }
  a.lastHitBy = b.owner;
  b.lastHitBy = a.owner;
  a.dirty = true; b.dirty = true;
}

// spatial-hash cell packing: two 16-bit signed cell coords → one i32
function packCell(cx: i32, cy: i32): i32 {
  return (cx << 16) | (cy & 0xffff);
}
function cellKey(x: f64, y: f64): i32 {
  const cx = <i32>Math.floor(x / CELL);
  const cy = <i32>Math.floor(y / CELL);
  return packCell(cx, cy);
}

registerReducer("spawn", spawn, ["session", "name", "color", "arena"]);
registerReducer("steer", steer, ["session", "dirX", "dirY"]);
registerReducer("boost", boost, ["session"]);
registerReducer("leave", leave, ["session"]);
registerReducer("tick", tick, ["dtMs", "subSteps", "arena"]);
