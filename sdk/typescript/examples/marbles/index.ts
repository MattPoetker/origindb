// Marble Clash — a 60 Hz physics sumo with LOBBY + MATCHMAKING, running as a
// SINGLE OriginDB WASM reducer module that hosts MANY concurrent matches.
//
// Matchmaking model (all data-driven — no engine change, no sharding):
//   * Players create or join a LOBBY. At capacity (or host force-start) a MATCH
//     begins immediately — there is no fixed slot pool, matches are unbounded.
//   * Every match is namespaced by its lobbyId: marbles are keyed
//     m:<lobbyId>:<id> and carry an `arena=<lobbyId>` column, so a client
//     subscribes to exactly its match with WHERE arena = '<lobbyId>'.
//   * The native tick scheduler ticks THIS ONE module once per frame; the tick
//     reducer walks every active match and steps its physics. One module, one
//     mutex → every lobby/match op is serialized, so nothing can race.
//   * Win = LAST MARBLE STANDING (one life per player, no respawn).
//
// Tables (server-global, namespaced by key):
//   lobbies  key = <lobbyId>            name/host/cap/count/state/winner
//   members  key = lm:<lobbyId>:<sess>  lobbyId/session/name/color
//   marbles  key = m:<lobbyId>:<id>     pos/vel/thrust/owner/score/alive/arena
//   players  key = <session>            name/color/mkey/lobbyId
//
// Reducers: createLobby · joinLobby · startNow · leaveLobby · steer · boost · tick

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

setModuleInfo("marbles", "2.1.0");
declareTable("marbles");
declareTable("players");
declareTable("lobbies");
declareTable("members");

// ---- arena + physics constants (keep in sync with public/game.js) ----------

const ARENA_R: f64 = 42.0;        // arena radius (world units); rim at this dist
const MARBLE_R: f64 = 1.1;        // marble radius
const THRUST: f64 = 46.0;         // steer acceleration (u/s^2)
const DAMPING: f64 = 0.86;        // linear velocity retained per second
const RESTITUTION: f64 = 0.94;    // marble-marble bounce elasticity
const MAX_SPEED: f64 = 34.0;      // clamp (u/s)
const BOOST_IMPULSE: f64 = 26.0;  // dash delta-v (u/s)
const BOOST_COOLDOWN: f64 = 1500.0; // ms between boosts
const FALL_MARGIN: f64 = 1.5;     // center-dist past rim before a marble drops
const CELL: f64 = 2.4;            // spatial-hash cell size (~2*MARBLE_R)
const FIXED_DT: f64 = 1.0 / 60.0; // physics substep (true 60 Hz)
const SCAN_LIMIT: i32 = 100000;   // scan all rows (SDK default caps at 1000)

const DEFAULT_CAP: i32 = 8;       // players per lobby before auto-start
const MIN_START: i32 = 2;         // host force-start floor
const RESULT_LINGER_MS: f64 = 8000.0;   // keep a finished match visible, then purge
const WAITING_TTL_MS: f64 = 600000.0;   // reap abandoned waiting lobbies (10 min)

// lobbies.state: "waiting" | "active" | "done"

// ---- deterministic PRNG -----------------------------------------------------

let g_seed: u64 = 0;
function rnd(): f64 {
  if (g_seed == 0) g_seed = (<u64>nowMs()) ^ 0x9e3779b97f4a7c15 ^ (<u64>generateId());
  let x = g_seed;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  g_seed = x;
  return <f64>(x >> 11) / 9007199254740992.0;
}

// Evenly-spaced ring start points so a match opens fairly.
function ringPoint(i: i32, n: i32, out: StaticArray<f64>): void {
  const a = (6.2831853 * <f64>i) / <f64>(n < 1 ? 1 : n) + rnd() * 0.15;
  const r = ARENA_R * 0.62;
  out[0] = Math.cos(a) * r;
  out[1] = Math.sin(a) * r;
}

function marbleKey(match: string, id: string): string { return "m:" + match + ":" + id; }
function marblePrefix(match: string): string { return "m:" + match + ":"; }
function memberKey(lobbyId: string, session: string): string { return "lm:" + lobbyId + ":" + session; }
function memberPrefix(lobbyId: string): string { return "lm:" + lobbyId + ":"; }

// ---- marble struct ----------------------------------------------------------

class M {
  id: string = "";
  match: string = "";        // lobbyId this marble belongs to (== arena column)
  owner: string = "";        // session id
  name: string = "";
  color: string = "#4da3ff";
  x: f64 = 0;
  y: f64 = 0;
  vx: f64 = 0;
  vy: f64 = 0;
  tx: f64 = 0;
  ty: f64 = 0;
  score: f64 = 0;
  alive: bool = true;
  lastHitBy: string = "";
  boostAt: f64 = 0;
  dirty: bool = false;

  static from(v: JsonValue): M {
    const m = new M();
    m.id = v.getString("id", "");
    m.match = v.getString("arena", "");
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
    m.lastHitBy = v.getString("lastHitBy", "");
    m.boostAt = v.getNumber("boostAt", 0);
    return m;
  }

  key(): string { return marbleKey(this.match, this.id); }

  // Hand-rolled JSON — runs 60x/sec per live marble in the tick writeback.
  // `arena` holds the lobbyId string so clients subscribe WHERE arena='<lobbyId>'.
  toJson(): string {
    return "{\"id\":\"" + this.id +
      "\",\"arena\":\"" + esc(this.match) +
      "\",\"owner\":\"" + esc(this.owner) +
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
      ",\"lastHitBy\":\"" + esc(this.lastHitBy) +
      "\",\"boostAt\":" + this.boostAt.toString() + "}";
  }
}

function esc(s: string): string {
  let out = "";
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c == 0x22 || c == 0x5c) out += "\\";
    if (c >= 0x20) out += String.fromCharCode(c);
  }
  return out;
}

// ---- player / lobby helpers -------------------------------------------------

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

function writePlayer(session: string, name: string, color: string, mkey: string, lobbyId: string): void {
  writeTable("players", session, JsonValue.newObject()
    .setString("session", session)
    .setString("name", name)
    .setString("color", color)
    .setString("mkey", mkey)
    .setString("lobbyId", lobbyId)
    .setNumber("updated", <f64>nowMs())
    .toString());
}

function writeLobby(id: string, name: string, host: string, cap: i32, count: i32,
                    state: string, createdMs: f64, startMs: f64, endMs: f64,
                    winner: string, winnerName: string): void {
  writeTable("lobbies", id, "{\"id\":\"" + id +
    "\",\"name\":\"" + esc(name) +
    "\",\"host\":\"" + esc(host) +
    "\",\"cap\":" + cap.toString() +
    ",\"count\":" + count.toString() +
    ",\"state\":\"" + state +
    "\",\"createdMs\":" + createdMs.toString() +
    ",\"startMs\":" + startMs.toString() +
    ",\"endMs\":" + endMs.toString() +
    ",\"winner\":\"" + esc(winner) +
    "\",\"winnerName\":\"" + esc(winnerName) + "\"}");
}

class Lobby {
  id: string = "";
  name: string = "";
  host: string = "";
  cap: i32 = DEFAULT_CAP;
  count: i32 = 0;
  state: string = "waiting";
  createdMs: f64 = 0;
  startMs: f64 = 0;
  endMs: f64 = 0;
  winner: string = "";
  winnerName: string = "";

  static of(id: string, v: JsonValue): Lobby {
    const l = new Lobby();
    l.id = id;
    l.name = v.getString("name", "");
    l.host = v.getString("host", "");
    l.cap = <i32>v.getNumber("cap", DEFAULT_CAP);
    l.count = <i32>v.getNumber("count", 0);
    l.state = v.getString("state", "waiting");
    l.createdMs = v.getNumber("createdMs", 0);
    l.startMs = v.getNumber("startMs", 0);
    l.endMs = v.getNumber("endMs", 0);
    l.winner = v.getString("winner", "");
    l.winnerName = v.getString("winnerName", "");
    return l;
  }
  static load(id: string): Lobby | null {
    const v = readTable("lobbies", id);
    if (v == null) return null;
    return Lobby.of(id, JsonValue.parse(v!));
  }
  save(): void {
    writeLobby(this.id, this.name, this.host, this.cap, this.count, this.state,
      this.createdMs, this.startMs, this.endMs, this.winner, this.winnerName);
  }
}

// ---- lobby reducers ---------------------------------------------------------

function createLobby(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const lobbyName = args.length > 1 ? args[1].asString() : "";
  const pname = args.length > 2 ? args[2].asString() : "";
  let color = args.length > 3 ? args[3].asString() : "";
  if (session.length == 0) return JsonValue.newObject().setString("error", "no session");
  if (color.length == 0) color = "#4da3ff";

  const id = generateId().toString();
  const now = <f64>nowMs();
  const nm = lobbyName.length > 0 ? lobbyName : ("Lobby-" + id.substring(id.length - 4));
  writeLobby(id, nm, session, DEFAULT_CAP, 1, "waiting", now, 0, 0, "", "");
  addMember(id, session, pname, color);
  return JsonValue.newObject().setString("lobbyId", id).setString("state", "waiting");
}

function addMember(lobbyId: string, session: string, name: string, color: string): void {
  writeTable("members", memberKey(lobbyId, session), JsonValue.newObject()
    .setString("lobbyId", lobbyId)
    .setString("session", session)
    .setString("name", name.length > 0 ? name : "guest")
    .setString("color", color.length > 0 ? color : "#4da3ff")
    .setNumber("joinedMs", <f64>nowMs())
    .toString());
}

function joinLobby(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const lobbyId = args.length > 1 ? args[1].asString() : "";
  const pname = args.length > 2 ? args[2].asString() : "";
  let color = args.length > 3 ? args[3].asString() : "";
  if (session.length == 0 || lobbyId.length == 0) return JsonValue.newObject().setString("error", "bad args");
  if (color.length == 0) color = "#4da3ff";

  const l = Lobby.load(lobbyId);
  if (l == null) return JsonValue.newObject().setString("error", "no such lobby");
  if (l!.state == "active") return JsonValue.newObject().setString("error", "match in progress").setString("state", "active");
  if (l!.state == "done") return JsonValue.newObject().setString("error", "match over").setString("state", "done");

  const already = readTable("members", memberKey(lobbyId, session)) != null;
  if (!already) {
    if (l!.count >= l!.cap) return JsonValue.newObject().setString("error", "lobby full");
    addMember(lobbyId, session, pname, color);
    l!.count += 1;
    l!.save();
  }
  if (l!.count >= l!.cap) startMatch(l!);
  const cur = Lobby.load(lobbyId)!;
  return JsonValue.newObject().setString("ok", "1").setString("state", cur.state).setString("match", lobbyId);
}

function startNow(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const lobbyId = args.length > 1 ? args[1].asString() : "";
  const l = Lobby.load(lobbyId);
  if (l == null) return JsonValue.newObject().setString("error", "no such lobby");
  if (l!.host != session) return JsonValue.newObject().setString("error", "host only");
  if (l!.state != "waiting") return JsonValue.newObject().setString("error", "not waiting");
  if (l!.count < MIN_START) return JsonValue.newObject().setString("error", "need >= 2 players");
  startMatch(l!);
  const cur = Lobby.load(lobbyId)!;
  return JsonValue.newObject().setString("ok", "1").setString("state", cur.state).setString("match", lobbyId);
}

// Begin a match: spawn one marble per member (one life), flip the lobby active.
// The match id IS the lobbyId; marbles are namespaced m:<lobbyId>:<id>.
function startMatch(l: Lobby): void {
  clearMatchMarbles(l.id);   // clear any stale marbles for this id
  const members = JsonValue.parse(scanTable("members", memberPrefix(l.id), 256));
  const n = members.length;
  const now = <f64>nowMs();
  const p = new StaticArray<f64>(2);
  for (let i = 0; i < n; i++) {
    const mv = members.at(i).get("value");
    const sess = mv.getString("session", "");
    if (sess.length == 0) continue;
    const nm = mv.getString("name", "guest");
    const col = mv.getString("color", "#4da3ff");
    const id = generateId().toString();
    const m = new M();
    m.id = id; m.match = l.id; m.owner = sess; m.name = nm; m.color = col;
    ringPoint(i, n, p);
    m.x = p[0]; m.y = p[1]; m.alive = true;
    writeTable("marbles", m.key(), m.toJson());
    writePlayer(sess, nm, col, m.key(), l.id);
  }
  l.state = "active"; l.startMs = now;
  l.save();
}

function clearMatchMarbles(match: string): void {
  const rows = JsonValue.parse(scanTable("marbles", marblePrefix(match), SCAN_LIMIT));
  for (let i = 0; i < rows.length; i++) deleteTable("marbles", rows.at(i).getString("key", ""));
}
function clearMembers(lobbyId: string): void {
  const rows = JsonValue.parse(scanTable("members", memberPrefix(lobbyId), 256));
  for (let i = 0; i < rows.length; i++) deleteTable("members", rows.at(i).getString("key", ""));
}

function leaveLobby(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const lobbyId = args.length > 1 ? args[1].asString() : "";
  const l = Lobby.load(lobbyId);
  if (l == null) return null;

  const wasMember = readTable("members", memberKey(lobbyId, session)) != null;
  if (wasMember) { deleteTable("members", memberKey(lobbyId, session)); if (l!.count > 0) l!.count -= 1; }

  if (l!.state == "active") {
    const m = findMarbleBySession(session);
    if (m != null) deleteTable("marbles", m!.key());
    deleteTable("players", session);
    // the next tick's win check will end the match if one remains
  }
  if (l!.state == "waiting" && (l!.count <= 0 || l!.host == session)) {
    clearMembers(lobbyId);
    l!.state = "done"; l!.endMs = <f64>nowMs(); l!.save();
    return null;
  }
  l!.save();
  return null;
}

function endMatch(l: Lobby, winnerSess: string, winnerName: string): void {
  l.state = "done"; l.endMs = <f64>nowMs();
  l.winner = winnerSess; l.winnerName = winnerName;
  l.save();
}

function purgeLobby(l: Lobby): void {
  clearMatchMarbles(l.id);
  clearMembers(l.id);
  deleteTable("lobbies", l.id);
}

// ---- gameplay reducers ------------------------------------------------------

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

function boost(args: Array<JsonValue>): JsonValue | null {
  const session = args.length > 0 ? args[0].asString() : "";
  const m = findMarbleBySession(session);
  if (m == null || !m!.alive) return null;
  const now = <f64>nowMs();
  if (now - m!.boostAt < BOOST_COOLDOWN) return null;
  const dx = m!.tx, dy = m!.ty;
  if (dx * dx + dy * dy < 0.0001) return null;
  m!.vx += dx * BOOST_IMPULSE;
  m!.vy += dy * BOOST_IMPULSE;
  m!.boostAt = now;
  writeTable("marbles", m!.key(), m!.toJson());
  return JsonValue.newObject().setBool("ok", true);
}

// tick: step EVERY active match this frame, and reap finished/stale lobbies.
// One module, one call — active matches are found by scanning the lobbies table
// (tiny). Each match simulates only its own m:<lobbyId>:* slice.
function tick(args: Array<JsonValue>): JsonValue | null {
  let subSteps = <i32>(args.length > 1 ? args[1].asNumber() : 1);
  if (subSteps < 1) subSteps = 1;
  if (subSteps > 8) subSteps = 8;
  const now = <f64>nowMs();

  const lobbies = JsonValue.parse(scanTable("lobbies", "", 4096));
  for (let i = 0; i < lobbies.length; i++) {
    const lv = lobbies.at(i).get("value");
    const st = lv.getString("state", "");
    if (st == "active") {
      simulateMatch(lv.getString("id", ""), subSteps, now);
    } else if (st == "done") {
      if (now - lv.getNumber("endMs", 0) > RESULT_LINGER_MS) purgeLobby(Lobby.of(lv.getString("id", ""), lv));
    } else if (st == "waiting") {
      if (now - lv.getNumber("createdMs", 0) > WAITING_TTL_MS) purgeLobby(Lobby.of(lv.getString("id", ""), lv));
    }
  }
  maybeGc();
  return null;
}

// Run the physics + win check for a single match (elimination — no respawn).
function simulateMatch(match: string, subSteps: i32, now: f64): void {
  if (match.length == 0) return;
  const rows = JsonValue.parse(scanTable("marbles", marblePrefix(match), SCAN_LIMIT));
  const n = rows.length;
  if (n == 0) return;

  const ms = new Array<M>(n);
  for (let i = 0; i < n; i++) ms[i] = M.from(rows.at(i).get("value"));

  const rim = ARENA_R + FALL_MARGIN;
  const rim2 = rim * rim;
  const diam = MARBLE_R * 2.0;
  const diam2 = diam * diam;
  const damp = Math.pow(DAMPING, FIXED_DT);

  for (let step = 0; step < subSteps; step++) {
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

    const grid = new Map<i32, Array<i32>>();
    for (let i = 0; i < n; i++) {
      if (!ms[i].alive) continue;
      const ck = cellKey(ms[i].x, ms[i].y);
      if (!grid.has(ck)) grid.set(ck, new Array<i32>());
      grid.get(ck).push(i);
    }

    const keys = grid.keys();
    for (let gk = 0; gk < keys.length; gk++) {
      const cellIdx = keys[gk];
      const cxx = cellIdx >> 16;
      const cyy = (cellIdx << 16) >> 16;
      const a = grid.get(cellIdx);
      for (let ox = -1; ox <= 1; ox++) {
        for (let oy = -1; oy <= 1; oy++) {
          const nk = packCell(cxx + ox, cyy + oy);
          if (nk < cellIdx || !grid.has(nk)) continue;
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

    for (let i = 0; i < n; i++) {
      const m = ms[i];
      if (!m.alive) continue;
      if (m.x * m.x + m.y * m.y > rim2) {
        m.alive = false; m.dirty = true;
        if (m.lastHitBy.length > 0 && m.lastHitBy != m.owner) {
          for (let j = 0; j < n; j++) {
            if (ms[j].owner == m.lastHitBy) { ms[j].score += 1; ms[j].dirty = true; break; }
          }
        }
      }
    }
  }

  for (let i = 0; i < n; i++) if (ms[i].dirty) writeTable("marbles", ms[i].key(), ms[i].toJson());

  // win check: last marble standing
  let aliveCount = 0;
  let lastAlive = -1;
  for (let i = 0; i < n; i++) if (ms[i].alive) { aliveCount++; lastAlive = i; }
  if (aliveCount <= 1) {
    const l = Lobby.load(match);
    if (l != null && l!.state == "active") {
      if (lastAlive >= 0) endMatch(l!, ms[lastAlive].owner, ms[lastAlive].name);
      else endMatch(l!, "", "draw");
    }
  }
}

let g_tick_count: i32 = 0;
function maybeGc(): void {
  g_tick_count++;
  if (g_tick_count >= 12) { g_tick_count = 0; __collect(); }
}

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
    const jImp = -(1.0 + RESTITUTION) * vn * 0.5;
    a.vx -= jImp * nx; a.vy -= jImp * ny;
    b.vx += jImp * nx; b.vy += jImp * ny;
  }
  a.lastHitBy = b.owner;
  b.lastHitBy = a.owner;
  a.dirty = true; b.dirty = true;
}

function packCell(cx: i32, cy: i32): i32 { return (cx << 16) | (cy & 0xffff); }
function cellKey(x: f64, y: f64): i32 {
  const cx = <i32>Math.floor(x / CELL);
  const cy = <i32>Math.floor(y / CELL);
  return packCell(cx, cy);
}

// ---- registration -----------------------------------------------------------

registerReducer("createLobby", createLobby, ["session", "lobbyName", "name", "color"]);
registerReducer("joinLobby", joinLobby, ["session", "lobbyId", "name", "color"]);
registerReducer("startNow", startNow, ["session", "lobbyId"]);
registerReducer("leaveLobby", leaveLobby, ["session", "lobbyId"]);
registerReducer("steer", steer, ["session", "dirX", "dirY"]);
registerReducer("boost", boost, ["session"]);
registerReducer("tick", tick, ["dtMs", "subSteps"]);
